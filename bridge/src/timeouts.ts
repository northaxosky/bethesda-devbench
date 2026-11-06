import { isObject, type JsonObject } from "./json.js";

export const DEFAULT_CALL_TIMEOUT_MS = 30_000;
// Must match the host's ScenarioConfig::maxRunTime (guarded by a test).
export const SCENARIO_RUN_CAP_MS = 60_000;
const CALL_MARGIN_MS = 15_000;

type CallBound = (args: JsonObject) => number | undefined;

function requestedTimeout(args: JsonObject): number | undefined {
  const value = args.timeoutMs;
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

// Tools whose synchronous call may legitimately outlive the default timeout.
const CALL_BOUNDS: Readonly<Record<string, CallBound>> = {
  scenario: (args) =>
    (args.action ?? "run") === "run" && args.async !== true
      ? SCENARIO_RUN_CAP_MS
      : undefined,
  capture: requestedTimeout,
  papyrus: requestedTimeout,
};

/** Per-call HTTP timeout, or undefined when the default already covers it. */
export function callTimeoutFor(
  tool: string,
  args: JsonObject,
  defaultMs = DEFAULT_CALL_TIMEOUT_MS,
): number | undefined {
  const bound = CALL_BOUNDS[tool]?.(args);
  if (bound === undefined) return undefined;
  const timeout = bound + CALL_MARGIN_MS;
  return timeout > defaultMs ? timeout : undefined;
}

// Transport bounds for the held long-poll requests, not flow waits.
export const AWAIT_POLL_MS = 20_000;
export const AWAIT_CALL_MARGIN_MS = 10_000;
// The launch helper rejects start handshakes above 60 s.
export const PLATFORM_START_CEILING_MS = 60_000;

export interface SessionTimings {
  stallMs: number;
  // Safety ceiling of every wait; never the expected way to finish.
  ceilingMs: number;
  stopCeilingMs: number;
}

export type WaitTimings = Pick<SessionTimings, "stallMs" | "ceilingMs">;

export const DEFAULT_SESSION_TIMINGS: SessionTimings = {
  stallMs: 120_000,
  ceilingMs: 600_000,
  stopCeilingMs: 120_000,
};

const MAX_TIMING_MS = 3_600_000;
export const TIMING_MAXIMA: Readonly<Record<keyof SessionTimings, number>> = {
  stallMs: MAX_TIMING_MS,
  ceilingMs: MAX_TIMING_MS,
  stopCeilingMs: MAX_TIMING_MS,
};

const REMOVED_TIMING_REPLACEMENTS: Readonly<Record<string, string>> = {
  healthTimeoutMs: "launch waits on runtime.json and process exit; use ceilingMs",
  mainMenuTimeoutMs: "waits end on events; use ceilingMs",
  loadTimeoutMs: "waits end on events; use ceilingMs",
  scenarioTimeoutMs: "waits end on events; use ceilingMs",
  gracefulStopTimeoutMs: "use stopCeilingMs",
  pollIntervalMs: "nothing polls any more; remove it",
  platformStartTimeoutMs: "the start handshake ceiling is fixed at 60 s; remove it",
  mainThreadStallBudgetMs: "use stallMs",
};

/** Parses the optional session config `timings` overrides. */
export function parseSessionTimings(value: unknown): Partial<SessionTimings> {
  if (value === undefined) return {};
  if (!isObject(value)) throw new Error("session config.timings must be an object");
  const timings: Partial<SessionTimings> = {};
  for (const [key, entry] of Object.entries(value)) {
    if (Object.hasOwn(REMOVED_TIMING_REPLACEMENTS, key)) {
      throw new Error(
        `session config.timings.${key} was removed: ${REMOVED_TIMING_REPLACEMENTS[key] ?? ""}`,
      );
    }
    if (!Object.hasOwn(TIMING_MAXIMA, key)) {
      throw new Error(`session config.timings.${key} is not a known timing`);
    }
    const name = key as keyof SessionTimings;
    const maximum = TIMING_MAXIMA[name];
    if (!Number.isSafeInteger(entry) || (entry as number) < 1 || (entry as number) > maximum) {
      throw new Error(`session config.timings.${key} must be an integer from 1 to ${String(maximum)}`);
    }
    timings[name] = entry as number;
  }
  return timings;
}
