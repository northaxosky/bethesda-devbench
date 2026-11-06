import { errorMessage, type JsonObject } from "../json.js";
import {
  IdentityChangedError,
  RemoteDomainError,
  type DevBenchEvent,
  type RemoteClient,
} from "../remote.js";
import { AWAIT_POLL_MS, type WaitTimings } from "../timeouts.js";
import {
  readerFor,
  type ConditionContext,
  type ConditionInput,
  type WaitCondition,
} from "./conditions.js";
import { describeEvent } from "./progress.js";
import { WaitFailedError, WaitSignals, type OwnedSignals } from "./signals.js";
import type {
  ProgressSink,
  WaitFailure,
  WaitResult,
} from "./types.js";

export interface WaitRequest {
  remote: RemoteClient;
  condition: WaitCondition;
  // Events at or before this seq predate the wait; default is head.
  since?: number;
  timings: WaitTimings;
  signal?: AbortSignal | undefined;
  progress?: ProgressSink | undefined;
  owned?: OwnedSignals | undefined;
}

const MAX_EVENT_PROGRESS_PER_POLL = 5;

export async function awaitCondition(request: WaitRequest): Promise<WaitResult> {
  const { remote, condition, progress } = request;
  const started = performance.now();
  let cursor = request.since ?? 0;
  const finish = (outcome: { ok: true; satisfied: JsonObject } | ({ ok: false } & WaitFailure)): WaitResult => ({
    ...outcome,
    until: condition.until,
    cursor,
    elapsedMs: Math.round(performance.now() - started),
  });

  let signals: WaitSignals | undefined;
  try {
    signals = await WaitSignals.start({
      ceilingMs: request.timings.ceilingMs,
      stallMs: request.timings.stallMs,
      signal: request.signal,
      owned: request.owned,
      progress,
      readHealth: async () =>
        (await remote.callTool("inspect", { kind: "health" }, { mutation: false }))
          .value as JsonObject,
    });
    const active = signals;
    const satisfied = await active.guard(pump(active));
    return finish({ ok: true, satisfied });
  } catch (error) {
    return finish({ ok: false, ...toFailure(error) });
  } finally {
    signals?.dispose();
  }

  async function pump(active: WaitSignals): Promise<JsonObject> {
    const context: ConditionContext = {
      remote,
      signal: active.signal,
      read: readerFor(remote),
    };
    const head = await remote.awaitEvents(0, { waitMs: 0, signal: active.signal });
    const instanceId = head.identity.instanceId;
    active.observe(head.value.events);
    if (request.since === undefined) cursor = head.value.headSeq;
    const driven = condition.drive?.(context);

    let input: ConditionInput = {
      events: head.value.events.filter((event) => event.seq > cursor),
      history: head.value.events.filter((event) => event.seq <= cursor),
      first: true,
      gap: request.since !== undefined && head.value.oldestSeq > request.since + 1,
      oldestSeq: head.value.oldestSeq,
    };
    if (input.events.length > 0) reportProgress(condition, input.events, started, progress);
    const loop = async (): Promise<JsonObject> => {
      let retry = false;
      for (;;) {
        if (condition.evaluate && shouldEvaluate(condition, input, retry)) {
          const verdict = await condition.evaluate(context, input);
          if (verdict.state === "satisfied") return verdict.evidence;
          if (verdict.state === "failed") throw new WaitFailedError(verdict);
          retry = verdict.retry;
        }
        const polled = await remote.awaitEvents(cursor, {
          waitMs: AWAIT_POLL_MS,
          signal: active.signal,
        });
        if (polled.identity.instanceId !== instanceId) {
          throw new IdentityChangedError(
            `DevBench instance changed during the wait (expected ${instanceId}, found ${polled.identity.instanceId})`,
          );
        }
        const { events, headSeq, oldestSeq } = polled.value;
        active.observe(events);
        reportProgress(condition, events, started, progress);
        input = {
          events,
          history: [],
          first: false,
          gap: oldestSeq > cursor + 1 && headSeq > cursor,
          oldestSeq,
        };
        const last = events.at(-1);
        if (last) cursor = last.seq;
      }
    };
    return driven ? await Promise.race([loop(), driven]) : await loop();
  }
}

function shouldEvaluate(
  condition: WaitCondition,
  input: ConditionInput,
  retry: boolean,
): boolean {
  if (input.first || input.gap || retry) return true;
  return (
    condition.topics.includes("*") ||
    input.events.some((event) => condition.topics.includes(event.topic))
  );
}

function reportProgress(
  condition: WaitCondition,
  events: readonly DevBenchEvent[],
  started: number,
  progress: ProgressSink | undefined,
): void {
  if (!progress) return;
  if (events.length === 0) {
    const seconds = Math.round((performance.now() - started) / 1000);
    progress(`waiting for ${condition.until} (${String(seconds)} s)`);
    return;
  }
  for (const event of events.slice(0, MAX_EVENT_PROGRESS_PER_POLL)) {
    progress(describeEvent(event));
  }
}

function toFailure(error: unknown): WaitFailure {
  if (error instanceof WaitFailedError) return error.failure;
  if (error instanceof RemoteDomainError) {
    return {
      reason: "unavailable",
      detail: { status: error.status, body: error.body },
    };
  }
  if (error instanceof DOMException && error.name === "AbortError") {
    return { reason: "cancelled", detail: errorMessage(error) };
  }
  return { reason: "unavailable", detail: errorMessage(error) };
}
