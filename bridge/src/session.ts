import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { readFile } from "node:fs/promises";

import type { Tool } from "@modelcontextprotocol/sdk/types.js";

import {
  SessionConfigProvider,
  validateLaunchInputs,
  type SessionConfig,
} from "./config.js";
import {
  collectEvidence,
  snapshotEvidence,
  type EvidenceSnapshot,
} from "./evidence.js";
import {
  asJsonValue,
  errorMessage,
  isObject,
  requireObject,
  type JsonObject,
  type JsonValue,
} from "./json.js";
import type {
  NativePlatform,
  StartedProcess,
  InspectedProcess,
} from "./platform.js";
import type { WaitEnvironment } from "./proxy.js";
import {
  GameUnavailableError,
  IdentityChangedError,
  RemoteClient,
  RemoteDomainError,
  UncertainMutationError,
  type ResolvedRemote,
} from "./remote.js";
import {
  processInstanceId,
  readRuntimeIdentity,
  sameWindowsPath,
  type RuntimeIdentity,
} from "./runtime.js";
import {
  DEFAULT_SESSION_TIMINGS,
  PLATFORM_START_CEILING_MS,
  type SessionTimings,
} from "./timeouts.js";
import {
  canLoad,
  parseCondition,
  playerReady,
  scenarioFinished,
  type WaitCondition,
} from "./waits/conditions.js";
import { ExitTracker } from "./waits/exits.js";
import {
  abortableDelay,
  awaitFileProbe,
  awaitMarkerLine,
  fileSize,
  fileTarget,
} from "./waits/files.js";
import {
  CrashLogWatch,
  WaitFailedError,
  WaitSignals,
  whenAborted,
  withCeiling,
  type OwnedSignals,
} from "./waits/signals.js";
import type {
  CallContext,
  ProgressSink,
  WaitFailure,
  WaitFailureReason,
  WaitResult,
} from "./waits/types.js";
import { awaitCondition } from "./waits/waiter.js";

export type SessionPhase =
  | "launchfailed"
  | "identitymismatch"
  | "loadfailed"
  | "scenariofailed"
  | "cancelled"
  | "cleanupfailed"
  | "complete";

export interface SessionControllerOptions {
  config: SessionConfigProvider;
  platformFactory: () => NativePlatform;
  remoteFactory: (config: SessionConfig) => RemoteClient;
  timings?: Partial<SessionTimings>;
  logger?: (message: string) => void;
}

interface RunArguments {
  fixture: string;
  scenario: JsonObject;
  finish: "stop" | "leave-running";
  forceStop: boolean;
}

interface JobRecord {
  id: string;
  kind: "start" | "run";
  state: "queued" | "running" | "done";
  startedAt: string;
  updatedAt: string;
  abort: AbortController;
  progress?: ProgressSink;
  finished?: Promise<void>;
  result?: JsonObject;
}

interface ReadySession {
  config: SessionConfig;
  remote: RemoteClient;
  process: StartedProcess;
  identity: RuntimeIdentity;
  inspect: InspectedProcess;
  dllSha256: string;
}

class WorkflowFailure extends Error {
  constructor(
    readonly phase: SessionPhase,
    message: string,
    readonly detail?: JsonValue,
    readonly reason?: WaitFailureReason,
  ) {
    super(message);
  }
}

// The listener thread starts just before runtime.json is written, so the
// first connection after the file appears can still be refused; retry briefly.
const STARTUP_RACE_ATTEMPTS = 20;
const STARTUP_RACE_DELAY_MS = 50;

export const SESSION_TOOL: Tool = {
  name: "devbench.session",
  description:
    "Control one explicitly configured, controller-owned DevBench game session. " +
    "Every call blocks until the real event happens and returns the result; never sleep, poll status, or use REST. " +
    "status is always local. " +
    "start launches the game and returns at canLoad. " +
    "run launches (or reuses the owned game), waits for canLoad, loads the required named fixture, waits for player readiness of that load, runs a server-side scenario until its scenario.finished event, captures evidence, then stops or releases only the owned game. " +
    "detach:true on start/run returns {queued, runId} at once; wait {runId} then blocks until that job finishes. " +
    "wait {until} blocks on a condition against the owned game: canLoad, playerReady {operationId?, actionCursor?}, menu {name, open?}, event {topic, match?, after?} (match alone also scans events fired before the wait), frames {count}; " +
    "for a game this controller does not own, use the await argument of proxied tools instead. " +
    "stop closes the owned game and returns after the game exits, the launch shim exits, and the configured exit marker line (RootBuilder cleanup) appears; force:true terminates it when the stop ceiling passes, even after an uncertain mutation; it also reclaims a game an earlier bridge launched and never released. " +
    "Waits end at once on the real failure signal, with reason exited, crashed, stalled, modal, loadFailed, actionsDisabled, eventHistoryLost, unavailable, cancelled, or ceiling (a safety ceiling, 10 min by default, never the expected path). " +
    "A modal failure carries the message box text; accept it with the menu tool and wait again. " +
    "Long calls send MCP progress notifications and are cancellable; cancelling a blocking start/run cancels the job cooperatively and cannot undo an accepted game mutation. " +
    "Mutations are never retried. No deployment, profile edits, arbitrary commands, or adoption of other games.",
  inputSchema: {
    type: "object",
    additionalProperties: false,
    required: ["action"],
    properties: {
      action: {
        type: "string",
        enum: ["status", "start", "run", "wait", "stop", "cancel"],
      },
      runId: { type: "string", description: "Job to wait on or cancel." },
      detach: {
        type: "boolean",
        default: false,
        description: "start/run: return {queued, runId} instead of blocking.",
      },
      until: {
        type: "string",
        enum: ["canLoad", "playerReady", "menu", "event", "frames"],
        description: "wait: condition to block on against the owned game.",
      },
      operationId: { type: "integer", description: "playerReady: the load operation to correlate." },
      actionCursor: { type: "integer", description: "playerReady: events after this seq count for load failure." },
      name: { type: "string", description: "menu: menu name." },
      open: { type: "boolean", default: true, description: "menu: wait for open (true) or closed (false)." },
      topic: { type: "string", description: "event: event topic." },
      match: { type: "object", description: "event: shallow payload match." },
      after: { type: "integer", description: "event: only events after this seq; fails eventHistoryLost if they were dropped." },
      count: { type: "integer", minimum: 1, description: "frames: rendered frames to wait for." },
      fixture: {
        type: "string",
        minLength: 1,
        description: "Extensionless game save basename required by run.",
      },
      steps: {
        type: "array",
        description: "Inline DevBench scenario steps.",
      },
      scenarioFile: {
        type: "string",
        description: "Absolute path to a JSON scenario document.",
      },
      finish: {
        type: "string",
        enum: ["stop", "leave-running"],
        default: "stop",
      },
      force: { type: "boolean", default: false },
      forceStop: { type: "boolean", default: false },
    },
  },
};

export class SessionController {
  private timings: SessionTimings;
  private readonly logger: (message: string) => void;
  private platform: NativePlatform | undefined;
  private ownedProcess: StartedProcess | undefined;
  private ownershipReleased = false;
  private exits: ExitTracker | undefined;
  private ready: ReadySession | undefined;
  // Exit-marker log size when this launch began; unknown for a reclaimed game.
  private launchMarkerOffset: number | undefined;
  private activeJob: JobRecord | undefined;
  private lastJob: JobRecord | undefined;
  private nextRunId = 1;
  private scenarioRunId: number | undefined;
  private uncertainWrite = false;
  private shuttingDown = false;

  constructor(private readonly options: SessionControllerOptions) {
    this.timings = { ...DEFAULT_SESSION_TIMINGS, ...options.timings };
    this.logger = options.logger ?? ((message) => console.error(message));
  }

  async handle(
    args: JsonObject,
    context: CallContext = {},
  ): Promise<{ value: JsonValue; isError: boolean }> {
    const action = args.action;
    if (typeof action !== "string") {
      return localError("devbench.session requires a string action");
    }
    try {
      switch (action) {
        case "status":
          return { value: this.status(), isError: false };
        case "start":
          return await this.runJob("start", args, context, (job) => this.executeStart(job));
        case "run": {
          const runArgs = await parseRunArguments(args);
          return await this.runJob("run", args, context, (job) =>
            this.executeRun(job, runArgs),
          );
        }
        case "wait":
          return await this.wait(args, context);
        case "stop": {
          const result = await this.stop(args.force === true, context);
          return { value: result, isError: result.ok !== true };
        }
        case "cancel":
          return { value: this.cancel(args.runId), isError: false };
        default:
          return localError(`unknown devbench.session action '${action}'`);
      }
    } catch (error) {
      return localError(errorMessage(error));
    }
  }

  status(): JsonObject {
    const job = this.activeJob ?? this.lastJob;
    return {
      available: true,
      config: this.options.config.describe(),
      ownership: {
        owned: this.ownedProcess !== undefined && !this.ownershipReleased,
        released: this.ownershipReleased,
        ...(this.ownedProcess
          ? {
              pid: this.ownedProcess.pid,
              creationTime: this.ownedProcess.creationTime,
              exe: this.ownedProcess.exe,
            }
          : {}),
      },
      uncertainWrite: this.uncertainWrite,
      ...(job
        ? {
            job: {
              runId: job.id,
              kind: job.kind,
              state: job.state,
              startedAt: job.startedAt,
              updatedAt: job.updatedAt,
              ...(job.result ? { result: job.result } : {}),
            },
          }
        : {}),
    };
  }

  /** Timings and, for the owned game only, exit and crash-log signals. */
  async waitEnvironment(instanceId: string): Promise<WaitEnvironment> {
    try {
      this.applyConfigTimings(await this.options.config.load());
    } catch {
      // Proxy waits work without a session config; defaults apply.
    }
    const ready = this.ownedReady();
    return {
      timings: this.timings,
      owned: ready?.identity.instanceId === instanceId ? this.ownedSignals(ready) : undefined,
    };
  }

  async shutdown(): Promise<void> {
    this.shuttingDown = true;
    this.activeJob?.abort.abort();
    this.ownershipReleased = true;
    this.dropTracking();
    if (this.platform) {
      await this.platform.shutdown().catch((error: unknown) => {
        this.logger(
          `devbench-bridge: platform shutdown failed: ${errorMessage(error)}`,
        );
      });
    }
  }

  // Blocking calls forward MCP cancellation to the job; detached ones return at once.
  private async runJob(
    kind: JobRecord["kind"],
    args: JsonObject,
    context: CallContext,
    work: (job: JobRecord) => Promise<JsonObject>,
  ): Promise<{ value: JsonValue; isError: boolean }> {
    if (args.detach !== undefined && typeof args.detach !== "boolean") {
      throw new Error("detach must be a boolean");
    }
    const job = this.createJob(kind);
    const detach = args.detach === true;
    if (!detach && context.progress) job.progress = context.progress;
    job.finished = this.executeJob(job, () => work(job));
    if (detach) {
      return { value: { queued: true, runId: job.id, action: kind }, isError: false };
    }
    const cancel = (): void => job.abort.abort();
    if (context.signal?.aborted) cancel();
    else context.signal?.addEventListener("abort", cancel, { once: true });
    await job.finished;
    context.signal?.removeEventListener("abort", cancel);
    const result = job.result ?? {};
    return { value: result, isError: result.ok !== true };
  }

  private async wait(
    args: JsonObject,
    context: CallContext,
  ): Promise<{ value: JsonValue; isError: boolean }> {
    if (args.timeoutMs !== undefined) {
      throw new Error("timeoutMs was removed; waits end on the real event, with a safety ceiling");
    }
    if (args.until !== undefined) return await this.waitUntil(args, context);
    const runId = args.runId;
    const job = this.activeJob;
    if (runId !== undefined && runId !== job?.id && runId !== this.lastJob?.id) {
      throw new Error(`unknown session job ${JSON.stringify(runId)}`);
    }
    if (job?.finished && (runId === undefined || runId === job.id)) {
      await Promise.race([job.finished, whenAborted(context.signal)]);
    }
    const status = this.status();
    return {
      value: context.signal?.aborted ? { ...status, waitCancelled: true } : status,
      isError: false,
    };
  }

  private async waitUntil(
    args: JsonObject,
    context: CallContext,
  ): Promise<{ value: JsonValue; isError: boolean }> {
    const ready = this.ownedReady();
    if (!ready) {
      throw new Error(
        "wait until needs a game this controller owns and has started; for any other game pass await to the proxied tool instead of waiting here",
      );
    }
    const result = await awaitCondition({
      remote: ready.remote,
      condition: parseCondition(args),
      timings: this.timings,
      signal: context.signal,
      progress: context.progress,
      owned: this.ownedSignals(ready),
    });
    return { value: asJsonValue(result), isError: !result.ok };
  }

  private createJob(kind: "start" | "run"): JobRecord {
    if (this.shuttingDown) {
      throw new Error("the session controller is shutting down");
    }
    if (this.activeJob) {
      throw new Error(
        `another session job is active (${this.activeJob.id}); wait or cancel it first`,
      );
    }
    if (this.uncertainWrite) {
      throw new Error(
        "a previous remote mutation has an uncertain outcome; verify the game, then end it with stop force:true before further writes",
      );
    }
    const now = new Date().toISOString();
    const job: JobRecord = {
      id: `session-${String(this.nextRunId++)}`,
      kind,
      state: "queued",
      startedAt: now,
      updatedAt: now,
      abort: new AbortController(),
    };
    this.activeJob = job;
    return job;
  }

  private async executeJob(
    job: JobRecord,
    work: () => Promise<JsonObject>,
  ): Promise<void> {
    job.state = "running";
    job.updatedAt = new Date().toISOString();
    try {
      job.result = await work();
    } catch (error) {
      job.result = {
        phase: "launchfailed",
        ok: false,
        error: errorMessage(error),
      };
    } finally {
      job.state = "done";
      job.updatedAt = new Date().toISOString();
      this.lastJob = job;
      if (this.activeJob === job) this.activeJob = undefined;
    }
  }

  // Precedence: defaults, then session.json timings, then constructor options.
  private applyConfigTimings(config: SessionConfig): void {
    this.timings = {
      ...DEFAULT_SESSION_TIMINGS,
      ...config.timings,
      ...this.options.timings,
    };
  }

  private async executeStart(job: JobRecord): Promise<JsonObject> {
    let config: SessionConfig | undefined;
    let snapshot: EvidenceSnapshot | undefined;
    let ready: ReadySession | undefined;
    const transcript: JsonValue[] = [];
    let report: JsonObject;
    try {
      config = await this.options.config.load();
      this.applyConfigTimings(config);
      snapshot = await snapshotEvidence(config);
      ready = await this.ensureReady(config, job);
      await this.awaitStage(ready, job, transcript, canLoad(), "loadfailed");
      report = successReport(job, ready, transcript);
    } catch (error) {
      report = failureReport(job, classifyFailure(error), error, ready, transcript);
      report = await this.enrichFailure(report, config);
      if (this.ownedProcess && !this.uncertainWrite && !this.shuttingDown) {
        const cleanup = await this.cleanup(false, { progress: job.progress });
        if (!cleanup.ok) {
          report.phase = "cleanupfailed";
          report.cleanup = cleanup;
        }
      }
    }
    return this.finishEvidence(job, config, snapshot, report);
  }

  private async executeRun(
    job: JobRecord,
    args: RunArguments,
  ): Promise<JsonObject> {
    let config: SessionConfig | undefined;
    let snapshot: EvidenceSnapshot | undefined;
    let ready: ReadySession | undefined;
    const transcript: JsonValue[] = [];
    let report: JsonObject;
    this.scenarioRunId = undefined;
    try {
      config = await this.options.config.load();
      this.applyConfigTimings(config);
      snapshot = await snapshotEvidence(config);
      ready = await this.ensureReady(config, job);
      transcript.push(asJsonValue({
        stage: "ready",
        identity: ready.identity,
        inspect: ready.inspect,
      }));

      await this.awaitStage(ready, job, transcript, canLoad(), "loadfailed");
      const loadCursor = await ready.remote.headSeq();
      const load = await mutate(
        ready.remote,
        "game",
        { action: "load", name: args.fixture },
      );
      transcript.push({ stage: "load.queued", result: load });
      const receipt = requireObject(load, "game load receipt");
      if (
        receipt.queued !== true ||
        !Number.isSafeInteger(receipt.operationId)
      ) {
        throw new WorkflowFailure(
          "loadfailed",
          "game load did not return a valid queued operation receipt",
          asJsonValue(receipt),
        );
      }
      await this.awaitStage(
        ready,
        job,
        transcript,
        playerReady({
          operationId: receipt.operationId as number,
          ...(Number.isSafeInteger(receipt.actionCursor)
            ? { actionCursor: receipt.actionCursor as number }
            : {}),
        }),
        "loadfailed",
        loadCursor,
      );

      const scenarioCursor = await ready.remote.headSeq();
      const launched = await mutate(
        ready.remote,
        "scenario",
        { ...args.scenario, action: "run", async: true },
      );
      transcript.push({ stage: "scenario.queued", result: launched });
      const launchObject = requireObject(launched, "scenario launch receipt");
      if (
        launchObject.queued !== true ||
        !Number.isSafeInteger(launchObject.runId)
      ) {
        throw new WorkflowFailure(
          "scenariofailed",
          "scenario did not return a valid async runId",
          asJsonValue(launchObject),
        );
      }
      this.scenarioRunId = launchObject.runId as number;
      const finished = await this.awaitStage(
        ready,
        job,
        transcript,
        scenarioFinished(this.scenarioRunId),
        "scenariofailed",
        scenarioCursor,
      );
      const scenarioStatus = requireObject(finished.satisfied.scenario, "scenario status");
      const scenarioResult = asJsonValue(
        scenarioStatus.result === undefined ? scenarioStatus : scenarioStatus.result,
      );
      if (!isObject(scenarioResult) || scenarioResult.ok !== true) {
        throw new WorkflowFailure(
          "scenariofailed",
          "scenario completed without ok=true",
          scenarioResult,
        );
      }

      const cleanup =
        args.finish === "stop"
          ? await this.cleanup(args.forceStop, { progress: job.progress })
          : await this.releaseOwnership();
      report = successReport(job, ready, transcript);
      report.fixture = args.fixture;
      report.scenarioRunId = this.scenarioRunId;
      report.scenario = scenarioResult;
      report.cleanup = cleanup;
      if (!cleanup.ok) {
        report.ok = false;
        report.phase = "cleanupfailed";
      }
    } catch (error) {
      if (error instanceof UncertainMutationError) this.uncertainWrite = true;
      report = failureReport(job, classifyFailure(error), error, ready, transcript);
      report = await this.enrichFailure(report, config);
      if (!this.uncertainWrite && !this.shuttingDown) {
        if (this.scenarioRunId !== undefined && ready) {
          try {
            transcript.push({
              stage: "scenario.cancel",
              result: await mutate(
                ready.remote,
                "scenario",
                { action: "cancel", runId: this.scenarioRunId },
              ),
            });
          } catch (cancelError) {
            transcript.push({
              stage: "scenario.cancel.failed",
              error: errorMessage(cancelError),
            });
          }
        }
        if (args.finish === "stop" && this.ownedProcess) {
          const cleanup = await this.cleanup(args.forceStop, { progress: job.progress });
          report.cleanup = cleanup;
          if (!cleanup.ok) report.phase = "cleanupfailed";
        } else if (args.finish === "leave-running" && this.ownedProcess) {
          report.cleanup = await this.releaseOwnership();
        }
      } else {
        report.cleanup = {
          ok: false,
          skipped: true,
          reason:
            this.shuttingDown
              ? "bridge shut down; the game stays reclaimable by a later stop and no further writes were attempted"
              : "remote mutation outcome is uncertain; no further writes or automatic stop were attempted",
        };
      }
    } finally {
      this.scenarioRunId = undefined;
    }
    return this.finishEvidence(job, config, snapshot, report);
  }

  // One waiter run for a job stage; a failed wait becomes a workflow failure.
  private async awaitStage(
    ready: ReadySession,
    job: JobRecord,
    transcript: JsonValue[],
    condition: WaitCondition,
    failurePhase: SessionPhase,
    since?: number,
  ): Promise<Extract<WaitResult, { ok: true }>> {
    const result = await awaitCondition({
      remote: ready.remote,
      condition,
      ...(since !== undefined ? { since } : {}),
      timings: this.timings,
      signal: job.abort.signal,
      progress: job.progress,
      owned: this.ownedSignals(ready),
    });
    transcript.push({ stage: condition.until, result: asJsonValue(result) });
    if (result.ok) return result;
    throw new WorkflowFailure(
      result.reason === "cancelled" ? "cancelled" : failurePhase,
      `${condition.until} wait failed: ${result.reason}`,
      result.detail,
      result.reason,
    );
  }

  private async ensureReady(
    config: SessionConfig,
    job: JobRecord,
  ): Promise<ReadySession> {
    await validateLaunchInputs(config);
    const dllSha256 = await sha256File(config.devbenchDll);
    if (
      config.expectedDllSha256 &&
      config.expectedDllSha256 !== dllSha256
    ) {
      throw new WorkflowFailure(
        "identitymismatch",
        `deployed DevBench DLL hash does not match expectedDllSha256 (expected ${config.expectedDllSha256}, actual ${dllSha256})`,
      );
    }
    const platform = this.getPlatform();
    await this.dropExitedOwnership(platform);
    let process = this.ownedProcess;
    if (!process || this.ownershipReleased) {
      this.ownedProcess = undefined;
      const prior = await platform.status();
      if (prior.running || prior.owned) {
        throw new WorkflowFailure(
          "launchfailed",
          "the platform helper reports an existing game; refusing to adopt or claim ownership",
          asJsonValue(prior),
        );
      }
      this.launchMarkerOffset = config.exitMarker
        ? await fileSize(config.exitMarker.path)
        : undefined;
      const requestedAt = Date.now();
      process = await platform.start({
        mo2Exe: config.mo2Exe,
        profilesDir: config.profilesDir,
        profile: config.profile,
        executable: config.executable,
        gameExe: config.gameExe,
        loaderExe: config.loaderExe,
        timeoutMs: PLATFORM_START_CEILING_MS,
      });
      this.adopt(platform, process);
      // FILETIME and Date.now() share the system clock; allow rounding only.
      if (Date.parse(process.creationTime) < requestedAt - 1_000) {
        throw new WorkflowFailure(
          "identitymismatch",
          `platform start returned pid ${String(process.pid)} created at ${process.creationTime}, before this launch request`,
        );
      }
    }
    const ready = await this.awaitRuntime(
      config,
      this.options.remoteFactory(config),
      process,
      dllSha256,
      job,
    );
    this.ready = ready;
    return ready;
  }

  // Waits on runtime.json changes, game exit, crash logs, and the ceiling.
  private async awaitRuntime(
    config: SessionConfig,
    remote: RemoteClient,
    process: StartedProcess,
    dllSha256: string,
    job: JobRecord,
  ): Promise<ReadySession> {
    const instanceId = processInstanceId(process.pid, process.creationTime);
    const observation = { note: "runtime.json has not appeared" };
    const signals = await WaitSignals.start({
      ceilingMs: this.timings.ceilingMs,
      stallMs: this.timings.stallMs,
      signal: job.abort.signal,
      owned: this.exits
        ? { instanceId, exits: this.exits, crashLogs: config.logs }
        : undefined,
      progress: job.progress,
    });
    try {
      return await signals.guard(
        awaitFileProbe({
          targets: remote.target.runtimeFiles.map(fileTarget),
          signal: signals.signal,
          probe: () =>
            this.probeRuntime(config, remote, process, instanceId, dllSha256, observation, signals.signal),
        }),
      );
    } catch (error) {
      if (error instanceof WaitFailedError) {
        throw launchFailure(error.failure, process, observation.note);
      }
      throw error;
    } finally {
      signals.dispose();
    }
  }

  private async probeRuntime(
    config: SessionConfig,
    remote: RemoteClient,
    process: StartedProcess,
    instanceId: string,
    dllSha256: string,
    observation: { note: string },
    signal: AbortSignal,
  ): Promise<ReadySession | undefined> {
    const platform = this.getPlatform();
    for (const runtimeFile of remote.target.runtimeFiles) {
      let identity: RuntimeIdentity;
      try {
        identity = await readRuntimeIdentity(runtimeFile);
      } catch (error) {
        observation.note = errorMessage(error);
        continue;
      }
      if (identity.instanceId !== instanceId) {
        observation.note = `runtime.json at ${runtimeFile} identifies ${identity.instanceId}, not the owned process ${instanceId}`;
        continue;
      }
      const resolved = await resolveAfterStartup(remote, signal, observation);
      if (!resolved) return undefined;
      if (resolved.identity.instanceId !== instanceId) {
        throw new WorkflowFailure(
          "identitymismatch",
          `runtime instance ${resolved.identity.instanceId} is not the owned process ${instanceId}`,
        );
      }
      if (!sameWindowsPath(resolved.identity.exePath, config.gameExe)) {
        throw new WorkflowFailure(
          "identitymismatch",
          `runtime exePath ${resolved.identity.exePath} does not match configured gameExe ${config.gameExe}`,
        );
      }
      const inspect = await platform.inspect({
        pid: process.pid,
        gameExe: config.gameExe,
        pluginName: "devbench.dll",
      });
      verifyNativeIdentity(config, process, inspect, resolved.identity, dllSha256);
      remote.pin(resolved.identity);
      return {
        config,
        remote,
        process,
        identity: resolved.identity,
        inspect,
        dllSha256,
      };
    }
    return undefined;
  }

  // A stop that timed out keeps ownership; never reuse it once the game exits.
  private async dropExitedOwnership(platform: NativePlatform): Promise<void> {
    if (!this.ownedProcess || this.ownershipReleased) return;
    const status = await platform.status();
    if (status.owned && status.running) return;
    if (status.owned) await platform.release();
    this.ownershipReleased = true;
    this.dropTracking();
    this.logger(
      `devbench-bridge: retained game pid ${String(this.ownedProcess.pid)} has exited; launching a new owned game`,
    );
  }

  private adopt(platform: NativePlatform, process: StartedProcess): void {
    this.dropTracking();
    this.ownedProcess = process;
    this.ownershipReleased = false;
    this.exits = new ExitTracker(platform, process);
  }

  private dropTracking(): void {
    this.exits?.dispose();
    this.exits = undefined;
    this.ready = undefined;
  }

  private ownedReady(): ReadySession | undefined {
    return this.ready && !this.ownershipReleased && this.exits ? this.ready : undefined;
  }

  private ownedSignals(ready: ReadySession): OwnedSignals | undefined {
    if (!this.exits || this.ownershipReleased) return undefined;
    return {
      instanceId: ready.identity.instanceId,
      exits: this.exits,
      crashLogs: ready.config.logs,
    };
  }

  private async stop(force: boolean, context: CallContext): Promise<JsonObject> {
    if (this.activeJob) {
      throw new Error(
        `cannot stop while ${this.activeJob.id} is active; cancel it first`,
      );
    }
    if (this.shuttingDown) {
      throw new Error("the session controller is shutting down");
    }
    if (this.ownershipReleased || !this.ownedProcess) await this.reclaim();
    if (this.uncertainWrite && !force) {
      throw new Error(
        "session.stop refused after an uncertain remote mutation; verify the game, then pass force:true to close it anyway",
      );
    }
    const result = await this.cleanup(force, context);
    if (result.exited === true) this.uncertainWrite = false;
    return result;
  }

  // Only a game an earlier bridge launched and never released is adopted.
  private async reclaim(): Promise<void> {
    const config = await this.options.config.load();
    this.applyConfigTimings(config);
    const platform = this.getPlatform();
    let process: StartedProcess;
    try {
      process = await platform.reclaim(config.gameExe);
    } catch (error) {
      throw new Error(
        `session.stop refused: this controller does not own a retained game process (${errorMessage(error)})`,
        { cause: error },
      );
    }
    this.adopt(platform, process);
    this.launchMarkerOffset = undefined;
  }

  private cancel(runId: unknown): JsonObject {
    if (!this.activeJob) throw new Error("no active session job to cancel");
    if (
      runId !== undefined &&
      (typeof runId !== "string" || runId !== this.activeJob.id)
    ) {
      throw new Error(
        `active session job is ${this.activeJob.id}, not ${JSON.stringify(runId)}`,
      );
    }
    this.activeJob.abort.abort();
    return {
      runId: this.activeJob.id,
      cancelRequested: true,
      note:
        "cancellation is cooperative and cannot undo an accepted game load or scenario mutation",
    };
  }

  // Close, then wait for game exit, shim exit, and the exit marker, each
  // against the stop ceiling; force terminates only after the ceiling passes.
  private async cleanup(force: boolean, context: CallContext): Promise<JsonObject> {
    const platform = this.platform;
    const exits = this.exits;
    if (!platform || !this.ownedProcess || !exits || this.ownershipReleased) {
      return { ok: false, reason: "no retained owned process" };
    }
    const status = await platform.status();
    if (!status.owned) {
      this.ownershipReleased = true;
      this.dropTracking();
      return {
        ok: false,
        reason: "platform helper no longer proves ownership; no close was attempted",
      };
    }
    const config = await this.options.config.load();
    const ceilingMs = this.timings.stopCeilingMs;
    const crash = await CrashLogWatch.snapshot(config.logs);
    const reclaimedMarker = config.exitMarker !== undefined && this.launchMarkerOffset === undefined;
    const markerOffset = config.exitMarker
      ? (this.launchMarkerOffset ?? (await fileSize(config.exitMarker.path)))
      : undefined;
    const cancelled = (forced: boolean): JsonObject => ({
      ok: false,
      exited: false,
      forced,
      reason: "cancelled; the game was left as it is and ownership is retained",
    });

    let exited = !status.running;
    let forced = false;
    let reason: string | undefined;
    if (!exited) {
      const close = await platform.requestClose();
      if (!close.running) {
        exited = true;
      } else {
        const gone = await withCeiling(() => exits.gameExited, ceilingMs, context.signal);
        if (!gone.done && gone.reason === "cancelled") return cancelled(false);
        exited = gone.done;
        reason = gone.done ? undefined : "graceful close did not finish within stopCeilingMs";
      }
      if (!exited && force) {
        forced = true;
        const terminated = await platform.terminate();
        exited = terminated.exited;
        reason = terminated.reason ?? reason;
      }
    }
    if (!exited) {
      return {
        ok: false,
        exited: false,
        forced,
        shimExited: false,
        exitMarker: null,
        ...(reason ? { reason } : {}),
      };
    }
    const crashPath = await crash.scan().catch(() => undefined);

    await exits.confirmShim();
    const shim = await withCeiling(() => exits.shimExited, ceilingMs, context.signal);
    if (!shim.done && shim.reason === "cancelled") return cancelled(forced);
    if (!shim.done) {
      return {
        ok: false,
        exited: true,
        forced,
        shimExited: false,
        exitMarker: null,
        reason: "the launch shim did not exit within stopCeilingMs; ownership is retained",
      };
    }
    await platform.release();
    this.ownershipReleased = true;
    this.dropTracking();

    let exitMarker: JsonObject | null = null;
    if (config.exitMarker && markerOffset !== undefined) {
      const { path, match } = config.exitMarker;
      const found = await withCeiling(
        (signal) => awaitMarkerLine({ path, match, offset: markerOffset }, signal),
        ceilingMs,
        context.signal,
      );
      exitMarker = {
        matched: found.done,
        ...(found.done ? { line: found.value } : { reason: found.reason }),
        ...(reclaimedMarker
          ? { note: "reclaimed game: the marker offset was recorded at stop time" }
          : {}),
      };
    }
    return {
      ok: exitMarker === null || exitMarker.matched === true,
      exited: true,
      forced,
      shimExited: true,
      exitMarker,
      ...(status.running ? {} : { alreadyExited: true }),
      ...(crashPath ? { crash: { path: crashPath } } : {}),
    };
  }

  private async releaseOwnership(): Promise<JsonObject> {
    if (!this.platform || !this.ownedProcess || this.ownershipReleased) {
      return { ok: false, reason: "no retained owned process" };
    }
    await this.platform.release();
    this.ownershipReleased = true;
    this.dropTracking();
    return {
      ok: true,
      released: true,
      note:
        "the game was left running and ownership was released; this controller will refuse a later stop",
    };
  }

  private async finishEvidence(
    job: JobRecord,
    config: SessionConfig | undefined,
    snapshot: EvidenceSnapshot | undefined,
    report: JsonObject,
  ): Promise<JsonObject> {
    if (!config || !snapshot) return report;
    try {
      const evidence = await collectEvidence(config, job.id, snapshot, report);
      return {
        ...report,
        evidence: {
          directory: evidence.directory,
          resultPath: evidence.resultPath,
          logs: evidence.logs,
          unavailable: evidence.unavailable,
        },
      };
    } catch (error) {
      return {
        ...report,
        ok: false,
        phase: "cleanupfailed",
        evidence: {
          unavailable: true,
          reason: errorMessage(error),
        },
      };
    }
  }

  private async enrichFailure(
    report: JsonObject,
    config: SessionConfig | undefined,
  ): Promise<JsonObject> {
    if (!config) return report;
    let dllSha256: string | null = null;
    try {
      dllSha256 = await sha256File(config.devbenchDll);
    } catch {
      // The primary validation error already explains why the configured file
      // could not be used; retain a null actual hash in the structured report.
    }
    return {
      ...report,
      ...(this.ownedProcess
        ? {
            pid: this.ownedProcess.pid,
            creationTime: this.ownedProcess.creationTime,
          }
        : {}),
      dllSha256,
      sourcePaths: {
        sessionConfig: config.sourcePath,
        ...(config.runtimeFile ? { runtimeFile: config.runtimeFile } : {}),
        gameExe: config.gameExe,
        devbenchDll: config.devbenchDll,
      },
      expectedActual: {
        runtime: {
          expected: config.expectedRuntime ?? null,
          actual: report.runtime ?? null,
        },
        dllSha256: {
          expected: config.expectedDllSha256 ?? dllSha256,
          actual: dllSha256,
        },
        dllPath: {
          expected: config.devbenchDll,
          actual: null,
        },
      },
    };
  }

  private getPlatform(): NativePlatform {
    this.platform ??= this.options.platformFactory();
    return this.platform;
  }
}

// Retries only a refused connection, once per runtime.json change.
async function resolveAfterStartup(
  remote: RemoteClient,
  signal: AbortSignal,
  observation: { note: string },
): Promise<ResolvedRemote | undefined> {
  for (let attempt = 0; attempt < STARTUP_RACE_ATTEMPTS; attempt++) {
    try {
      return await remote.resolve();
    } catch (error) {
      if (!(error instanceof GameUnavailableError)) throw error;
      observation.note = error.message;
    }
    await abortableDelay(STARTUP_RACE_DELAY_MS, signal);
  }
  return undefined;
}

function launchFailure(
  failure: WaitFailure,
  process: StartedProcess,
  note: string,
): WorkflowFailure {
  const pid = String(process.pid);
  let message: string;
  switch (failure.reason) {
    case "exited":
      message = `owned game pid ${pid} exited before DevBench became ready: ${note}`;
      break;
    case "crashed":
      message = `owned game pid ${pid} crashed before DevBench became ready: ${note}`;
      break;
    case "ceiling":
      message = `timed out waiting for DevBench to publish its runtime identity: ${note}`;
      break;
    default:
      message = `launch wait failed (${failure.reason}): ${note}`;
  }
  return new WorkflowFailure(
    failure.reason === "cancelled" ? "cancelled" : "launchfailed",
    message,
    failure.detail,
    failure.reason,
  );
}

async function parseRunArguments(args: JsonObject): Promise<RunArguments> {
  if (typeof args.fixture !== "string" || args.fixture.length === 0) {
    throw new Error("session.run requires a non-empty fixture basename");
  }
  if (args.finish !== undefined && typeof args.finish !== "string") {
    throw new Error("session.run finish must be a string");
  }
  const finish = args.finish ?? "stop";
  if (finish !== "stop" && finish !== "leave-running") {
    throw new Error("session.run finish must be 'stop' or 'leave-running'");
  }
  const inline = args.steps;
  const scenarioFile = args.scenarioFile;
  if ((inline === undefined) === (scenarioFile === undefined)) {
    throw new Error(
      "session.run requires exactly one of steps or scenarioFile",
    );
  }
  let scenario: JsonObject;
  if (inline !== undefined) {
    if (!Array.isArray(inline)) throw new Error("session.run steps must be an array");
    scenario = { steps: inline.map(asJsonValue) };
  } else {
    if (typeof scenarioFile !== "string" || scenarioFile.length === 0) {
      throw new Error("session.run scenarioFile must be a non-empty path");
    }
    let raw: string;
    try {
      raw = await readFile(scenarioFile, "utf8");
    } catch (error) {
      throw new Error(
        `cannot read scenarioFile ${scenarioFile}: ${errorMessage(error)}`,
        { cause: error },
      );
    }
    let value: unknown;
    try {
      value = JSON.parse(raw) as unknown;
    } catch (error) {
      throw new Error(
        `invalid scenarioFile JSON at ${scenarioFile}: ${errorMessage(error)}`,
        { cause: error },
      );
    }
    scenario = asJsonValue(requireObject(value, "scenarioFile")) as JsonObject;
  }
  return {
    fixture: args.fixture,
    scenario,
    finish,
    forceStop: args.forceStop === true,
  };
}

async function mutate(
  remote: RemoteClient,
  name: string,
  args: JsonObject,
): Promise<JsonValue> {
  try {
    return (await remote.callTool(name, args, { mutation: true })).value;
  } catch (error) {
    throw domainFailure(name, error);
  }
}

function domainFailure(name: string, error: unknown): unknown {
  if (!(error instanceof RemoteDomainError)) return error;
  return new WorkflowFailure(
    name === "game" ? "loadfailed" : "scenariofailed",
    `DevBench ${name} call failed with HTTP ${String(error.status)}`,
    error.body,
  );
}

function verifyNativeIdentity(
  config: SessionConfig,
  process: StartedProcess,
  inspect: InspectedProcess,
  identity: RuntimeIdentity,
  dllSha256: string,
): void {
  if (
    !inspect.running ||
    inspect.pid !== process.pid ||
    inspect.creationTime !== process.creationTime ||
    !sameWindowsPath(inspect.exe, config.gameExe)
  ) {
    throw new WorkflowFailure(
      "identitymismatch",
      "native process inspection does not match the retained owned process",
      asJsonValue(inspect),
    );
  }
  if (!inspect.plugin) {
    throw new WorkflowFailure(
      "identitymismatch",
      "devbench.dll is not loaded in the owned game; verify the selected MO2 profile enables the deployed plugin",
    );
  }
  if (!sameWindowsPath(inspect.plugin.path, config.devbenchDll)) {
    throw new WorkflowFailure(
      "identitymismatch",
      `loaded devbench.dll path ${inspect.plugin.path} does not match configured deployment ${config.devbenchDll}`,
    );
  }
  if (inspect.plugin.sha256.toLowerCase() !== dllSha256) {
    throw new WorkflowFailure(
      "identitymismatch",
      `loaded devbench.dll hash does not match the pre-launch deployed file (expected ${dllSha256}, actual ${inspect.plugin.sha256})`,
    );
  }
  const expectedRuntime = config.expectedRuntime ?? identity.runtime;
  if (
    identity.runtime !== expectedRuntime ||
    inspect.runtimeVersion !== expectedRuntime
  ) {
    throw new WorkflowFailure(
      "identitymismatch",
      `runtime version mismatch (expected ${expectedRuntime}, runtime.json ${identity.runtime}, process ${inspect.runtimeVersion})`,
    );
  }
  if (!sameWindowsPath(identity.dllPath, inspect.plugin.path)) {
    throw new WorkflowFailure(
      "identitymismatch",
      `runtime.json dllPath ${identity.dllPath} does not match the loaded module ${inspect.plugin.path}`,
    );
  }
}

function successReport(
  job: JobRecord,
  ready: ReadySession,
  transcript: JsonValue[],
): JsonObject {
  return {
    ok: true,
    phase: "complete",
    runId: job.id,
    instanceId: ready.identity.instanceId,
    pid: ready.process.pid,
    creationTime: ready.process.creationTime,
    runtime: ready.identity.runtime,
    dllSha256: ready.dllSha256,
    sourcePaths: {
      sessionConfig: ready.config.sourcePath,
      runtimeFile: ready.identity.runtimeFile,
      gameExe: ready.config.gameExe,
      devbenchDll: ready.config.devbenchDll,
    },
    expectedActual: {
      runtime: {
        expected: ready.config.expectedRuntime ?? ready.identity.runtime,
        actual: ready.inspect.runtimeVersion,
      },
      dllSha256: {
        expected: ready.config.expectedDllSha256 ?? ready.dllSha256,
        actual: ready.inspect.plugin?.sha256 ?? null,
      },
      dllPath: {
        expected: ready.config.devbenchDll,
        actual: ready.inspect.plugin?.path ?? null,
      },
    },
    transcript,
  };
}

function failureReport(
  job: JobRecord,
  phase: SessionPhase,
  error: unknown,
  ready?: ReadySession,
  transcript: JsonValue[] = [],
): JsonObject {
  const detail =
    error instanceof WorkflowFailure && error.detail !== undefined
      ? error.detail
      : null;
  return {
    ok: false,
    phase,
    runId: job.id,
    error: errorMessage(error),
    ...(error instanceof WorkflowFailure && error.reason ? { reason: error.reason } : {}),
    detail,
    ...(ready
      ? {
          instanceId: ready.identity.instanceId,
          pid: ready.process.pid,
          creationTime: ready.process.creationTime,
          runtime: ready.identity.runtime,
          dllSha256: ready.dllSha256,
          sourcePaths: {
            sessionConfig: ready.config.sourcePath,
            runtimeFile: ready.identity.runtimeFile,
            gameExe: ready.config.gameExe,
            devbenchDll: ready.config.devbenchDll,
          },
        }
      : {}),
    transcript,
  };
}

function classifyFailure(error: unknown): SessionPhase {
  if (error instanceof WorkflowFailure) return error.phase;
  if (error instanceof DOMException && error.name === "AbortError") {
    return "cancelled";
  }
  if (
    error instanceof GameUnavailableError ||
    error instanceof IdentityChangedError
  ) {
    return "identitymismatch";
  }
  return "launchfailed";
}

function localError(message: string): { value: JsonValue; isError: true } {
  return { value: { ok: false, reason: message }, isError: true };
}

async function sha256File(path: string): Promise<string> {
  const hash = createHash("sha256");
  await new Promise<void>((resolvePromise, reject) => {
    const stream = createReadStream(path);
    stream.on("data", (chunk) => hash.update(chunk));
    stream.once("error", reject);
    stream.once("end", resolvePromise);
  });
  return hash.digest("hex");
}

