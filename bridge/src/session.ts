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
import {
  GameUnavailableError,
  IdentityChangedError,
  RemoteClient,
  RemoteDomainError,
  UncertainMutationError,
} from "./remote.js";
import {
  sameWindowsPath,
  type RuntimeIdentity,
} from "./runtime.js";

export type SessionPhase =
  | "launchfailed"
  | "identitymismatch"
  | "loadfailed"
  | "scenariofailed"
  | "cancelled"
  | "cleanupfailed"
  | "complete";

export interface SessionTimings {
  startupTimeoutMs: number;
  loadTimeoutMs: number;
  scenarioTimeoutMs: number;
  gracefulStopTimeoutMs: number;
  pollIntervalMs: number;
  platformStartTimeoutMs: number;
}

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
  ) {
    super(message);
  }
}

const DEFAULT_TIMINGS: SessionTimings = {
  startupTimeoutMs: 60_000,
  loadTimeoutMs: 180_000,
  scenarioTimeoutMs: 600_000,
  gracefulStopTimeoutMs: 15_000,
  pollIntervalMs: 250,
  platformStartTimeoutMs: 60_000,
};

export const SESSION_TOOL: Tool = {
  name: "devbench.session",
  description:
    "Control one explicitly configured, controller-owned Fallout 4 DevBench session. " +
    "status is always local. start/run are asynchronous and return a runId. run loads a required named fixture, waits for the correlated native operation and player readiness, runs a server-side scenario asynchronously, captures evidence, and stops or releases only the owned game. No deployment, profile edits, arbitrary commands, or adoption of pre-existing games.",
  inputSchema: {
    type: "object",
    additionalProperties: false,
    required: ["action"],
    properties: {
      action: {
        type: "string",
        enum: ["status", "start", "run", "stop", "cancel"],
      },
      runId: { type: "string" },
      fixture: {
        type: "string",
        minLength: 1,
        description: "Extensionless Fallout 4 save basename required by run.",
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
  private readonly timings: SessionTimings;
  private readonly logger: (message: string) => void;
  private platform: NativePlatform | undefined;
  private ownedProcess: StartedProcess | undefined;
  private ownershipReleased = false;
  private activeJob: JobRecord | undefined;
  private lastJob: JobRecord | undefined;
  private nextRunId = 1;
  private scenarioRunId: number | undefined;
  private uncertainWrite = false;
  private shuttingDown = false;

  constructor(private readonly options: SessionControllerOptions) {
    this.timings = { ...DEFAULT_TIMINGS, ...options.timings };
    this.logger = options.logger ?? ((message) => console.error(message));
  }

  async handle(args: JsonObject): Promise<{ value: JsonValue; isError: boolean }> {
    const action = args.action;
    if (typeof action !== "string") {
      return localError("devbench.session requires a string action");
    }
    try {
      switch (action) {
        case "status":
          return { value: this.status(), isError: false };
        case "start":
          return { value: this.queueStart(), isError: false };
        case "run":
          return {
            value: this.queueRun(await parseRunArguments(args)),
            isError: false,
          };
        case "stop":
          return {
            value: await this.stop(args.force === true),
            isError: false,
          };
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

  async shutdown(): Promise<void> {
    this.shuttingDown = true;
    this.activeJob?.abort.abort();
    this.ownershipReleased = true;
    if (this.platform) {
      await this.platform.shutdown().catch((error: unknown) => {
        this.logger(
          `devbench-bridge: platform release during shutdown failed: ${errorMessage(error)}`,
        );
      });
    }
  }

  private queueStart(): JsonObject {
    const job = this.createJob("start");
    void this.executeJob(job, () => this.executeStart(job));
    return { queued: true, runId: job.id, action: "start" };
  }

  private queueRun(args: RunArguments): JsonObject {
    const job = this.createJob("run");
    void this.executeJob(job, () => this.executeRun(job, args));
    return { queued: true, runId: job.id, action: "run" };
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
        "a previous remote mutation has an uncertain outcome; no further writes are allowed until the bridge is restarted and the operator verifies game state",
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

  private async executeStart(job: JobRecord): Promise<JsonObject> {
    let config: SessionConfig | undefined;
    let snapshot: EvidenceSnapshot | undefined;
    let report: JsonObject;
    try {
      config = await this.options.config.load();
      snapshot = await snapshotEvidence(config);
      const ready = await this.ensureReady(config, job.abort.signal);
      report = successReport(job, ready, []);
    } catch (error) {
      report = failureReport(job, classifyFailure(error), error);
      report = await this.enrichFailure(report, config);
      if (this.ownedProcess && !this.uncertainWrite && !this.shuttingDown) {
        const cleanup = await this.cleanup(false);
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
      snapshot = await snapshotEvidence(config);
      ready = await this.ensureReady(config, job.abort.signal);
      transcript.push(asJsonValue({
        stage: "ready",
        identity: ready.identity,
        inspect: ready.inspect,
      }));

      await this.waitForLoadAvailability(ready, job.abort.signal, transcript);
      const eventStart = await ready.remote.events(0);
      const eventCursor = eventStart.value.headSeq;
      const load = await invoke(
        ready.remote,
        "game",
        { action: "load", name: args.fixture },
        true,
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
      const operationId = receipt.operationId as number;
      await this.waitForLoad(
        ready,
        operationId,
        job.abort.signal,
        transcript,
      );

      const scenarioArgs = {
        ...args.scenario,
        action: "run",
        async: true,
      };
      const launched = await invoke(
        ready.remote,
        "scenario",
        scenarioArgs,
        true,
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
      const scenarioResult = await this.waitForScenario(
        ready,
        this.scenarioRunId,
        eventCursor,
        job.abort.signal,
        transcript,
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
          ? await this.cleanup(args.forceStop)
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
              result: await invoke(
                ready.remote,
                "scenario",
                { action: "cancel", runId: this.scenarioRunId },
                true,
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
          const cleanup = await this.cleanup(args.forceStop);
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
              ? "bridge shutdown released ownership; no further writes or automatic stop were attempted"
              : "remote mutation outcome is uncertain; no further writes or automatic stop were attempted",
        };
      }
    } finally {
      this.scenarioRunId = undefined;
    }
    return this.finishEvidence(job, config, snapshot, report);
  }

  private async ensureReady(
    config: SessionConfig,
    signal: AbortSignal,
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
      process = await platform.start({
        mo2Exe: config.mo2Exe,
        profilesDir: config.profilesDir,
        profile: config.profile,
        executable: config.executable,
        gameExe: config.gameExe,
        loaderExe: config.loaderExe,
        timeoutMs: this.timings.platformStartTimeoutMs,
      });
      this.ownedProcess = process;
      this.ownershipReleased = false;
    }
    const remote = this.options.remoteFactory(config);
    const deadline = deadlineAfter(this.timings.startupTimeoutMs);
    let lastError = "DevBench did not become ready";
    while (!deadline.expired()) {
      throwIfAborted(signal);
      try {
        const resolved = await remote.resolve();
        if (resolved.identity.pid !== process.pid) {
          throw new WorkflowFailure(
            "identitymismatch",
            `runtime pid ${String(resolved.identity.pid)} does not match owned pid ${String(process.pid)}`,
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
      } catch (error) {
        if (error instanceof WorkflowFailure) throw error;
        lastError = errorMessage(error);
      }
      await delay(this.timings.pollIntervalMs, signal);
    }
    throw new WorkflowFailure(
      "launchfailed",
      `timed out waiting for DevBench health and native module identity: ${lastError}`,
    );
  }

  private async waitForLoadAvailability(
    ready: ReadySession,
    signal: AbortSignal,
    transcript: JsonValue[],
  ): Promise<void> {
    const deadline = deadlineAfter(this.timings.startupTimeoutMs);
    while (!deadline.expired()) {
      throwIfAborted(signal);
      const status = requireObject(
        await invoke(ready.remote, "game", { action: "status" }, false),
        "game status",
      );
      if (status.actionsAllowed !== true) {
        throw new WorkflowFailure("loadfailed", "Native game actions are disabled; allowGameActions must be enabled before launch");
      }
      if (status.gameLoaded === true && status.canLoad === true &&
        (!isObject(status.operation) || status.operation.active !== true)) {
        transcript.push({ stage: "load.available", status: asJsonValue(status) });
        return;
      }
      await delay(this.timings.pollIntervalMs, signal);
    }
    throw new WorkflowFailure("loadfailed", "Timed out waiting for the game to accept a native fixture load");
  }

  private async waitForLoad(
    ready: ReadySession,
    operationId: number,
    signal: AbortSignal,
    transcript: JsonValue[],
  ): Promise<void> {
    const deadline = deadlineAfter(this.timings.loadTimeoutMs);
    while (!deadline.expired()) {
      throwIfAborted(signal);
      const statusValue = await invoke(
        ready.remote,
        "game",
        { action: "status" },
        false,
      );
      const status = requireObject(statusValue, "game status");
      const operation = status.operation;
      if (isObject(operation) && operation.operationId === operationId) {
        transcript.push({
          stage: "load.status",
          phase: asJsonValue(operation.phase),
          success: asJsonValue(operation.success),
        });
        if (
          operation.phase === "failed" ||
          operation.phase === "cancelled" ||
          operation.success === false
        ) {
          throw new WorkflowFailure(
            "loadfailed",
            "the correlated native load operation failed",
            asJsonValue(operation),
          );
        }
        if (operation.phase === "succeeded" && operation.success === true) {
          const stateValue = await invoke(
            ready.remote,
            "inspect",
            { kind: "state" },
            false,
          );
          const state = requireObject(stateValue, "inspect state");
          if (state.playerLoaded === true) {
            transcript.push({ stage: "load.ready", state: asJsonValue(state) });
            return;
          }
        }
      }
      await delay(this.timings.pollIntervalMs, signal);
    }
    throw new WorkflowFailure(
      "loadfailed",
      `timed out waiting for operationId ${String(operationId)} and player readiness`,
    );
  }

  private async waitForScenario(
    ready: ReadySession,
    runId: number,
    initialCursor: number,
    signal: AbortSignal,
    transcript: JsonValue[],
  ): Promise<JsonValue> {
    const deadline = deadlineAfter(this.timings.scenarioTimeoutMs);
    let cursor = initialCursor;
    while (!deadline.expired()) {
      throwIfAborted(signal);
      const statusValue = await invoke(
        ready.remote,
        "scenario",
        { action: "status", runId },
        false,
      );
      const status = requireObject(statusValue, "scenario status");
      const eventBatch = await ready.remote.events(cursor).catch((error: unknown) => {
        transcript.push({
          stage: "scenario.events.unavailable",
          error: errorMessage(error),
        });
        return undefined;
      });
      if (eventBatch) {
        cursor = eventBatch.value.headSeq;
        const progress = eventBatch.value.events.filter(
          (event) =>
            isObject(event) &&
            typeof event.topic === "string" &&
            event.topic.startsWith("scenario."),
        );
        if (progress.length > 0) {
          transcript.push({ stage: "scenario.events", events: progress });
        }
      }
      if (status.done === true) {
        transcript.push({ stage: "scenario.done", status: asJsonValue(status) });
        return status.result === undefined
          ? asJsonValue(status)
          : asJsonValue(status.result);
      }
      await delay(this.timings.pollIntervalMs, signal);
    }
    throw new WorkflowFailure(
      "scenariofailed",
      `timed out waiting for scenario runId ${String(runId)}`,
    );
  }

  private async stop(force: boolean): Promise<JsonObject> {
    if (this.activeJob) {
      throw new Error(
        `cannot stop while ${this.activeJob.id} is active; cancel it first`,
      );
    }
    if (this.shuttingDown) {
      throw new Error("the session controller is shutting down");
    }
    if (this.ownershipReleased || !this.ownedProcess || !this.platform) {
      throw new Error(
        "session.stop refused: this controller does not own a retained game process",
      );
    }
    if (this.uncertainWrite) {
      throw new Error(
        "session.stop refused after an uncertain remote mutation; verify the game manually before deciding whether to force cleanup",
      );
    }
    return this.cleanup(force);
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

  private async cleanup(force: boolean): Promise<JsonObject> {
    if (!this.platform || !this.ownedProcess || this.ownershipReleased) {
      return { ok: false, reason: "no retained owned process" };
    }
    const status = await this.platform.status();
    if (!status.owned) {
      this.ownershipReleased = true;
      return {
        ok: false,
        reason: "platform helper no longer proves ownership; no close was attempted",
      };
    }
    if (!status.running) {
      await this.platform.release();
      this.ownershipReleased = true;
      return { ok: true, exited: true, alreadyExited: true };
    }
    const graceful = await this.platform.close(this.timings.gracefulStopTimeoutMs);
    if (graceful.exited) {
      await this.platform.release();
      this.ownershipReleased = true;
      return { ok: true, exited: true, forced: false };
    }
    if (!force) {
      return {
        ok: false,
        exited: false,
        forced: false,
        reason: graceful.reason ?? "graceful close timed out",
      };
    }
    const terminated = await this.platform.terminate();
    if (terminated.exited) {
      await this.platform.release();
      this.ownershipReleased = true;
    }
    return {
      ok: terminated.exited,
      exited: terminated.exited,
      forced: true,
      ...(terminated.reason ? { reason: terminated.reason } : {}),
    };
  }

  private async releaseOwnership(): Promise<JsonObject> {
    if (!this.platform || !this.ownedProcess || this.ownershipReleased) {
      return { ok: false, reason: "no retained owned process" };
    }
    await this.platform.release();
    this.ownershipReleased = true;
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

async function invoke(
  remote: RemoteClient,
  name: string,
  args: JsonObject,
  mutation: boolean,
): Promise<JsonValue> {
  try {
    return (await remote.callTool(name, args, { mutation })).value;
  } catch (error) {
    if (error instanceof RemoteDomainError) {
      throw new WorkflowFailure(
        name === "game" ? "loadfailed" : "scenariofailed",
        `DevBench ${name} call failed with HTTP ${String(error.status)}`,
        error.body,
      );
    }
    throw error;
  }
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

function deadlineAfter(timeoutMs: number): {
  expired: () => boolean;
} {
  const end = performance.now() + timeoutMs;
  return { expired: () => performance.now() >= end };
}

async function delay(timeoutMs: number, signal: AbortSignal): Promise<void> {
  await new Promise<void>((resolvePromise, reject) => {
    if (signal.aborted) {
      reject(new DOMException("session job cancelled", "AbortError"));
      return;
    }
    const timeout = setTimeout(resolvePromise, timeoutMs);
    signal.addEventListener(
      "abort",
      () => {
        clearTimeout(timeout);
        reject(new DOMException("session job cancelled", "AbortError"));
      },
      { once: true },
    );
  });
}

function throwIfAborted(signal: AbortSignal): void {
  if (signal.aborted) {
    throw new DOMException("session job cancelled", "AbortError");
  }
}
