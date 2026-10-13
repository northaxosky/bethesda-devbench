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

export interface SessionTimings {
  healthTimeoutMs: number;
  mainMenuTimeoutMs: number;
  loadTimeoutMs: number;
  scenarioTimeoutMs: number;
  gracefulStopTimeoutMs: number;
  pollIntervalMs: number;
  platformStartTimeoutMs: number;
  mainThreadStallBudgetMs: number;
}

export const DEFAULT_SESSION_TIMINGS: SessionTimings = {
  healthTimeoutMs: 5_000,
  mainMenuTimeoutMs: 50_000,
  loadTimeoutMs: 10_000,
  scenarioTimeoutMs: 60_000,
  // The launch helper rejects close timeouts above 30 s.
  gracefulStopTimeoutMs: 10_000,
  pollIntervalMs: 250,
  // MO2 forward to shim ownership transfer; the helper caps it at 60 s.
  platformStartTimeoutMs: 10_000,
  // First world frames after a load can stall the main thread for seconds.
  mainThreadStallBudgetMs: 30_000,
};

const MAX_TIMING_MS = 3_600_000;
export const TIMING_MAXIMA: Readonly<Record<keyof SessionTimings, number>> = {
  healthTimeoutMs: MAX_TIMING_MS,
  mainMenuTimeoutMs: MAX_TIMING_MS,
  loadTimeoutMs: MAX_TIMING_MS,
  scenarioTimeoutMs: MAX_TIMING_MS,
  gracefulStopTimeoutMs: 30_000,
  pollIntervalMs: 60_000,
  platformStartTimeoutMs: 60_000,
  mainThreadStallBudgetMs: MAX_TIMING_MS,
};

/** Parses the optional session config `timings` overrides. */
export function parseSessionTimings(value: unknown): Partial<SessionTimings> {
  if (value === undefined) return {};
  if (!isObject(value)) throw new Error("session config.timings must be an object");
  const timings: Partial<SessionTimings> = {};
  for (const [key, entry] of Object.entries(value)) {
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

const seconds = (ms: number): string => `${String(ms / 1000)} s`;

/** Bounds paragraph of the devbench.session description, from the defaults. */
export function sessionBoundsDescription(t: SessionTimings = DEFAULT_SESSION_TIMINGS): string {
  return (
    "Bounds (defaults; raise with session.json timings): " +
    `launch to health <=${seconds(t.healthTimeoutMs)}, ` +
    `health to main menu (canLoad) <=${seconds(t.mainMenuTimeoutMs)}, ` +
    `fixture load to player ready <=${seconds(t.loadTimeoutMs)}, ` +
    `scenario run <=${seconds(t.scenarioTimeoutMs)} of scenario time, ` +
    `graceful stop <=${seconds(t.gracefulStopTimeoutMs)}. ` +
    "Poll status every 1-2 s; when a bound is exceeded, stop and report, and never retry a mutation."
  );
}
