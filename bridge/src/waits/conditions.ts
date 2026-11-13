import {
  isObject,
  requireObject,
  type JsonObject,
  type JsonValue,
} from "../json.js";
import {
  RemoteDomainError,
  type DevBenchEvent,
  type RemoteClient,
} from "../remote.js";
import { AWAIT_POLL_MS } from "../timeouts.js";
import type { WaitFailure } from "./types.js";

/** ok:false means the main thread was too busy to answer (HTTP 503/504). */
export type ReadResult = { ok: true; value: JsonObject } | { ok: false };

export interface ConditionContext {
  remote: RemoteClient;
  signal: AbortSignal;
  read(tool: string, args: JsonObject): Promise<ReadResult>;
}

export interface ConditionInput {
  events: readonly DevBenchEvent[];
  // Retained events at or before the cursor; first evaluation only.
  history: readonly DevBenchEvent[];
  first: boolean;
  // Events were lost to ring overflow since the last poll.
  gap: boolean;
  oldestSeq: number;
}

export type Verdict =
  | { state: "satisfied"; evidence: JsonObject }
  | ({ state: "failed" } & WaitFailure)
  // retry: inconclusive state read; re-evaluate on the next poll.
  | { state: "pending"; retry: boolean };

export interface WaitCondition {
  readonly until: string;
  readonly topics: readonly string[];
  readonly label?: string;
  evaluate?(ctx: ConditionContext, input: ConditionInput): Promise<Verdict>;
  drive?(ctx: ConditionContext): Promise<JsonObject>;
}

export function readerFor(remote: RemoteClient): ConditionContext["read"] {
  return async (tool, args) => {
    try {
      const { value } = await remote.callTool(tool, args, { mutation: false, internal: true });
      return { ok: true, value: requireObject(value, `${tool} response`) as JsonObject };
    } catch (error) {
      if (error instanceof RemoteDomainError && (error.status === 503 || error.status === 504)) {
        return { ok: false };
      }
      throw error;
    }
  };
}

const pending = (retry = false): Verdict => ({ state: "pending", retry });
const satisfied = (evidence: JsonObject): Verdict => ({ state: "satisfied", evidence });
const failed = (reason: WaitFailure["reason"], detail: JsonValue): Verdict => ({
  state: "failed",
  reason,
  detail,
});

const isLifecycle = (event: DevBenchEvent, name: string): boolean =>
  event.topic === "lifecycle" && event.data.event === name;

function loadAvailable(status: JsonObject): boolean {
  return (
    status.gameLoaded === true &&
    status.canLoad === true &&
    !(isObject(status.operation) && status.operation.active === true)
  );
}

// A message box blocks the game; accept it via menu, then wait again.
async function modalVerdict(
  ctx: ConditionContext,
  input: ConditionInput,
): Promise<Verdict | undefined> {
  let open = input.events.some(
    (event) =>
      event.topic === "menu" &&
      event.data.name === "MessageBoxMenu" &&
      event.data.opening === true,
  );
  if (!open && input.first) {
    const list = await ctx.read("menu", { action: "list" });
    open = list.ok && list.value.messageBoxOpen === true;
  }
  if (!open) return undefined;
  const snapshot = await ctx.read("menu", { action: "describe" });
  return failed("modal", {
    menu: "MessageBoxMenu",
    snapshot: snapshot.ok ? snapshot.value : { unavailable: true },
  });
}

export function canLoad(): WaitCondition {
  return {
    until: "canLoad",
    topics: ["lifecycle", "menu"],
    async evaluate(ctx, input) {
      const status = await ctx.read("game", { action: "status" });
      if (status.ok) {
        if (status.value.actionsAllowed !== true) {
          return failed(
            "actionsDisabled",
            "Native game actions are disabled; allowGameActions must be enabled before launch",
          );
        }
        if (loadAvailable(status.value)) return satisfied({ status: status.value });
      }
      return (await modalVerdict(ctx, input)) ?? pending(!status.ok);
    },
  };
}

export function playerReady(target: {
  operationId?: number;
  actionCursor?: number;
}): WaitCondition {
  const { operationId, actionCursor } = target;
  return {
    until: "playerReady",
    topics: ["lifecycle", "menu"],
    async evaluate(ctx, input) {
      const candidates =
        actionCursor === undefined
          ? input.events
          : [...input.history, ...input.events].filter((event) => event.seq > actionCursor);
      const failure = candidates.find(
        (event) => isLifecycle(event, "postLoadGame") && event.data.success === false,
      );
      if (failure) return failed("loadFailed", { event: failure });

      const status = await ctx.read("game", { action: "status" });
      if (status.ok) {
        const operation = status.value.operation;
        if (
          isObject(operation) &&
          (operationId === undefined || operation.operationId === operationId)
        ) {
          if (
            operation.phase === "failed" ||
            operation.phase === "cancelled" ||
            operation.success === false
          ) {
            return failed("loadFailed", { operation: operation });
          }
          if (operation.phase === "succeeded" && operation.success === true) {
            const state = await ctx.read("inspect", { kind: "state" });
            if (state.ok && state.value.playerLoaded === true) {
              return satisfied({
                operation: operation,
                state: { playerLoaded: true },
              });
            }
            return pending(true);
          }
        }
      }
      return (await modalVerdict(ctx, input)) ?? pending(!status.ok);
    },
  };
}

function menuNames(list: JsonObject): string[] {
  const open = list.openMenus;
  if (!Array.isArray(open)) return [];
  return open.map((entry) =>
    typeof entry === "string" ? entry : isObject(entry) && typeof entry.name === "string" ? entry.name : "",
  );
}

export function menuState(target: { name: string; open: boolean }): WaitCondition {
  let needState = true;
  return {
    until: "menu",
    topics: ["menu"],
    async evaluate(ctx, input) {
      const hit = input.events.find(
        (event) =>
          event.topic === "menu" &&
          event.data.name === target.name &&
          event.data.opening === target.open,
      );
      if (hit) return satisfied({ event: hit });
      if (!needState && !input.gap) return pending();
      const list = await ctx.read("menu", { action: "list" });
      if (!list.ok) return pending(true);
      needState = false;
      const names = menuNames(list.value);
      return names.includes(target.name) === target.open
        ? satisfied({ openMenus: names })
        : pending();
    },
  };
}

function shallowMatch(data: JsonObject, match: JsonObject | undefined): boolean {
  if (!match) return true;
  return Object.entries(match).every(
    ([key, value]) => JSON.stringify(data[key]) === JSON.stringify(value),
  );
}

// Without `after`, `match` also scans the ring for pre-wait events.
export function eventOccurred(target: {
  topic: string;
  match?: JsonObject;
  after?: number;
}): WaitCondition {
  const { topic, match, after } = target;
  const check = (input: ConditionInput): Verdict => {
    const lostBeforeFirstRead =
      input.first && after !== undefined && input.oldestSeq > after + 1;
    if (input.gap || lostBeforeFirstRead) {
      return failed("eventHistoryLost", { oldestSeq: input.oldestSeq, after: after ?? null });
    }
    const scanHistory = input.first && (after !== undefined || match !== undefined);
    const hit = [...(scanHistory ? input.history : []), ...input.events].find(
      (event) =>
        event.topic === topic &&
        (after === undefined || event.seq > after) &&
        shallowMatch(event.data, match),
    );
    return hit ? satisfied({ event: hit }) : pending();
  };
  return {
    until: "event",
    topics: [topic],
    label: `event "${topic}"${match ? ` ${JSON.stringify(match)}` : ""}`,
    evaluate: (_ctx, input) => Promise.resolve(check(input)),
  };
}

export function framesElapsed(count: number): WaitCondition {
  return {
    until: "frames",
    topics: [],
    async drive(ctx) {
      const start = (await ctx.remote.awaitFrame(0, { waitMs: 0, signal: ctx.signal })).value.frame;
      let frame = start;
      while (frame < start + count) {
        frame = (
          await ctx.remote.awaitFrame(frame, { waitMs: AWAIT_POLL_MS, signal: ctx.signal })
        ).value.frame;
      }
      return { frames: count, startFrame: start, endFrame: frame };
    },
  };
}

export function scenarioFinished(runId: number): WaitCondition {
  let retry = false;
  return {
    until: "scenario",
    topics: ["scenario.finished"],
    async evaluate(ctx, input) {
      const finished = input.events.some(
        (event) => event.topic === "scenario.finished" && event.data.runId === runId,
      );
      if (!finished && !input.first && !retry) return pending();
      const status = await ctx.read("scenario", { action: "status", runId });
      retry = !status.ok || finished;
      if (status.ok && status.value.done === true) return satisfied({ scenario: status.value });
      return pending(retry);
    },
  };
}

export function operationTerminal(operationId: number): WaitCondition {
  return {
    until: "operation",
    topics: ["lifecycle"],
    async evaluate(ctx) {
      const status = await ctx.read("game", { action: "status" });
      if (!status.ok) return pending(true);
      const operation = status.value.operation;
      if (!isObject(operation) || operation.operationId !== operationId) return pending();
      if (
        operation.phase === "failed" ||
        operation.phase === "cancelled" ||
        operation.success === false
      ) {
        return failed("operationFailed", { operation: operation });
      }
      if (operation.phase === "succeeded" || operation.phase === "completed") {
        return satisfied({ operation: operation });
      }
      return pending();
    },
  };
}

function optionalInteger(args: JsonObject, key: string): number | undefined {
  const value = args[key];
  if (value === undefined) return undefined;
  if (!Number.isSafeInteger(value)) throw new Error(`wait ${key} must be an integer`);
  return value as number;
}

export function parseCondition(args: JsonObject): WaitCondition {
  switch (args.until) {
    case "canLoad":
      return canLoad();
    case "playerReady": {
      const operationId = optionalInteger(args, "operationId");
      const actionCursor = optionalInteger(args, "actionCursor");
      return playerReady({
        ...(operationId !== undefined ? { operationId } : {}),
        ...(actionCursor !== undefined ? { actionCursor } : {}),
      });
    }
    case "menu": {
      if (typeof args.name !== "string" || args.name.length === 0) {
        throw new Error("wait until menu requires a menu name");
      }
      if (args.open !== undefined && typeof args.open !== "boolean") {
        throw new Error("wait open must be a boolean");
      }
      return menuState({ name: args.name, open: args.open ?? true });
    }
    case "event": {
      if (typeof args.topic !== "string" || args.topic.length === 0) {
        throw new Error("wait until event requires a topic");
      }
      if (args.match !== undefined && !isObject(args.match)) {
        throw new Error("wait match must be an object");
      }
      const after = optionalInteger(args, "after");
      return eventOccurred({
        topic: args.topic,
        ...(args.match !== undefined ? { match: args.match } : {}),
        ...(after !== undefined ? { after } : {}),
      });
    }
    case "frames": {
      const count = optionalInteger(args, "count");
      if (count === undefined || count < 1) {
        throw new Error("wait until frames requires a positive integer count");
      }
      return framesElapsed(count);
    }
    default:
      throw new Error(
        `wait until must be one of canLoad, playerReady, menu, event, frames (got ${JSON.stringify(args.until)})`,
      );
  }
}
