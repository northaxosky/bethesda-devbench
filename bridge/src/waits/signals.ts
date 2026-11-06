import { stat } from "node:fs/promises";

import type { SessionLogSpec } from "../config.js";
import { expandSpec, wildcardExpression } from "../evidence.js";
import { errorMessage, isObject, type JsonObject } from "../json.js";
import type { DevBenchEvent } from "../remote.js";
import type { ExitTracker } from "./exits.js";
import { awaitFileProbe, fileTarget, type WatchTarget } from "./files.js";
import type { ProgressSink, WaitFailure, WaitFailureReason } from "./types.js";

/** What only the controller knows about a game it launched. */
export interface OwnedSignals {
  instanceId: string;
  exits: ExitTracker;
  crashLogs: readonly SessionLogSpec[];
}

export interface SignalOptions {
  ceilingMs: number;
  stallMs: number;
  // Request or job cancellation.
  signal?: AbortSignal | undefined;
  owned?: OwnedSignals | undefined;
  // Reads `inspect health`; stall detection is off without it.
  readHealth?: (() => Promise<JsonObject>) | undefined;
  progress?: ProgressSink | undefined;
}

export class WaitFailedError extends Error {
  constructor(readonly failure: WaitFailure) {
    super(`wait failed: ${failure.reason}`);
  }
}

/** Files under crash-flagged log specs that appeared or grew since snapshot. */
export class CrashLogWatch {
  private constructor(
    private readonly specs: readonly SessionLogSpec[],
    private readonly baseline: ReadonlyMap<string, number>,
  ) {}

  static async snapshot(logs: readonly SessionLogSpec[]): Promise<CrashLogWatch> {
    const specs = logs.filter((spec) => spec.crash === true);
    return new CrashLogWatch(specs, await crashFileSizes(specs));
  }

  get active(): boolean {
    return this.specs.length > 0;
  }

  targets(): WatchTarget[] {
    return this.specs.map((spec) => {
      if (!spec.pattern) return fileTarget(spec.path);
      const expression = wildcardExpression(spec.pattern);
      return { dir: spec.path, accept: (name) => expression.test(name) };
    });
  }

  // Empty files are ignored; a replaced file with new size is a new crash.
  async scan(): Promise<string | undefined> {
    for (const [path, size] of await crashFileSizes(this.specs)) {
      if (size > 0 && this.baseline.get(path) !== size) return path;
    }
    return undefined;
  }
}

async function crashFileSizes(
  specs: readonly SessionLogSpec[],
): Promise<Map<string, number>> {
  const sizes = new Map<string, number>();
  for (const spec of specs) {
    let paths: string[];
    try {
      paths = await expandSpec(spec);
    } catch {
      continue;
    }
    for (const path of paths) {
      try {
        const value = await stat(path);
        if (value.isFile()) sizes.set(path, value.size);
      } catch {
        // A file that vanished mid-scan is not a crash log.
      }
    }
  }
  return sizes;
}

/**
 * The failure-signal set of one wait. The first signal to fire wins, aborts
 * `signal` so in-flight long-polls and watchers stop, and resolves `failed`.
 */
export class WaitSignals {
  readonly failed: Promise<WaitFailure>;
  private readonly controller = new AbortController();
  private resolveFailed!: (failure: WaitFailure) => void;
  private failure: WaitFailure | undefined;
  private readonly cleanups: (() => void)[] = [];
  private stallTimer: NodeJS.Timeout | undefined;
  private stallGeneration = 0;

  private constructor(private readonly options: SignalOptions) {
    this.failed = new Promise((resolve) => {
      this.resolveFailed = resolve;
    });
  }

  static async start(options: SignalOptions): Promise<WaitSignals> {
    const signals = new WaitSignals(options);
    try {
      await signals.arm();
    } catch (error) {
      signals.dispose();
      throw error;
    }
    return signals;
  }

  /** Aborts when a failure fires or the signals are disposed. */
  get signal(): AbortSignal {
    return this.controller.signal;
  }

  fail(reason: WaitFailureReason, detail: WaitFailure["detail"]): void {
    if (this.failure) return;
    this.failure = { reason, detail };
    this.resolveFailed(this.failure);
    this.controller.abort();
  }

  /** Feeds health events to the stall detector, oldest first. */
  observe(events: readonly DevBenchEvent[]): void {
    for (const event of events) {
      if (event.topic === "health.stalled") this.armStall(event.data);
      else if (event.topic === "health.resumed") this.disarmStall();
    }
  }

  /** Resolves with the work's value, or throws the failure that fired first. */
  async guard<T>(work: Promise<T>): Promise<T> {
    const settled = await Promise.race([
      work.then(
        (value) => ({ value }),
        (error: unknown) => ({ error }),
      ),
      this.failed.then((failure) => ({ failure })),
    ]);
    if ("failure" in settled) throw new WaitFailedError(settled.failure);
    if ("error" in settled) throw settled.error;
    return settled.value;
  }

  dispose(): void {
    this.disarmStall();
    for (const cleanup of this.cleanups.splice(0)) cleanup();
    this.controller.abort();
  }

  private async arm(): Promise<void> {
    const { ceilingMs, signal, owned } = this.options;
    const ceiling = setTimeout(() => {
      this.fail("ceiling", { ceilingMs });
    }, ceilingMs);
    ceiling.unref();
    this.cleanups.push(() => {
      clearTimeout(ceiling);
    });

    if (signal) {
      const onCancel = (): void => {
        this.fail("cancelled", errorMessage(signal.reason));
      };
      if (signal.aborted) onCancel();
      else {
        signal.addEventListener("abort", onCancel, { once: true });
        this.cleanups.push(() => {
          signal.removeEventListener("abort", onCancel);
        });
      }
    }
    if (owned) await this.armOwned(owned);
  }

  private async armOwned(owned: OwnedSignals): Promise<void> {
    const crash = await CrashLogWatch.snapshot(owned.crashLogs);
    this.cleanups.push(
      owned.exits.onGameExit((exit) => {
        // One scan so a crash is classified as crashed, not exited.
        void crash.scan().catch(() => undefined).then((path) => {
          if (path) this.fail("crashed", { path, pid: exit.pid, exitCode: exit.exitCode });
          else this.fail("exited", { pid: exit.pid, exitCode: exit.exitCode });
        });
      }),
    );
    if (crash.active) {
      awaitFileProbe({
        targets: crash.targets(),
        probe: () => crash.scan(),
        signal: this.signal,
      }).then(
        (path) => {
          this.fail("crashed", { path });
        },
        (error: unknown) => {
          if (!this.signal.aborted) {
            this.options.progress?.(`crash log watch lost: ${errorMessage(error)}`);
          }
        },
      );
    }
    await owned.exits.confirm();
  }

  private armStall(data: JsonObject): void {
    if (!this.options.readHealth) return;
    const stalledForMs = typeof data.stalledForMs === "number" ? data.stalledForMs : 0;
    this.disarmStall();
    this.scheduleStallCheck(
      Math.max(0, this.options.stallMs - stalledForMs),
      Date.now() - stalledForMs,
    );
  }

  private scheduleStallCheck(delayMs: number, stalledSince: number): void {
    const generation = ++this.stallGeneration;
    this.stallTimer = setTimeout(() => {
      void this.checkStall(generation, stalledSince);
    }, delayMs);
    this.stallTimer.unref();
  }

  private disarmStall(): void {
    this.stallGeneration++;
    clearTimeout(this.stallTimer);
    this.stallTimer = undefined;
  }

  private async checkStall(generation: number, stalledSince: number): Promise<void> {
    const readHealth = this.options.readHealth;
    if (!readHealth || generation !== this.stallGeneration) return;
    const stalledMs = Date.now() - stalledSince;
    let health: JsonObject;
    try {
      health = await readHealth();
    } catch (error) {
      if (generation === this.stallGeneration) {
        this.fail("stalled", { stalledMs, healthReadError: errorMessage(error) });
      }
      return;
    }
    if (generation !== this.stallGeneration) return;
    if (isObject(health) && health.foreground === false) {
      this.options.progress?.(
        `stalled ${String(Math.round(stalledMs / 1000))} s (background)`,
      );
      this.scheduleStallCheck(this.options.stallMs, stalledSince);
      return;
    }
    this.fail("stalled", {
      stalledMs,
      frame: health.frame ?? null,
      pendingTasks: health.pendingTasks ?? null,
      lastTaskFrame: health.lastTaskFrame ?? null,
      foreground: health.foreground ?? null,
    });
  }
}

export type CeilingResult<T> =
  | { done: true; value: T }
  | { done: false; reason: "ceiling" | "cancelled" };

/** Races stage work against a safety ceiling and optional cancellation. */
export async function withCeiling<T>(
  work: (signal: AbortSignal) => Promise<T>,
  ceilingMs: number,
  cancel?: AbortSignal,
): Promise<CeilingResult<T>> {
  const controller = new AbortController();
  let timer: NodeJS.Timeout | undefined;
  let onCancel: (() => void) | undefined;
  const stopped = new Promise<CeilingResult<T>>((resolve) => {
    timer = setTimeout(() => {
      resolve({ done: false, reason: "ceiling" });
    }, ceilingMs);
    onCancel = (): void => {
      resolve({ done: false, reason: "cancelled" });
    };
    if (cancel?.aborted) onCancel();
    else cancel?.addEventListener("abort", onCancel, { once: true });
  });
  try {
    const pending = work(controller.signal).then((value) => ({ done: true as const, value }));
    pending.catch(() => undefined);
    return await Promise.race([pending, stopped]);
  } finally {
    clearTimeout(timer);
    if (onCancel) cancel?.removeEventListener("abort", onCancel);
    controller.abort();
  }
}

/** Resolves when `signal` aborts; never if it is absent. */
export function whenAborted(signal: AbortSignal | undefined): Promise<void> {
  return new Promise((resolve) => {
    if (!signal) return;
    if (signal.aborted) resolve();
    else signal.addEventListener("abort", () => { resolve(); }, { once: true });
  });
}
