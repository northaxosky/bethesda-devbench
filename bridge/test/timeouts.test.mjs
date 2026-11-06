import assert from "node:assert/strict";
import test from "node:test";

import { coreTools } from "../dist/catalog.js";
import { parseSessionConfig } from "../dist/config.js";
import {
  SCENARIO_RUN_CAP_MS,
  callTimeoutFor,
  parseSessionTimings,
} from "../dist/timeouts.js";

test("synchronous scenario runs outlive the default call timeout", () => {
  const timeout = callTimeoutFor("scenario", { steps: [] });
  assert.ok(timeout > SCENARIO_RUN_CAP_MS);
  assert.equal(callTimeoutFor("scenario", { async: true }), undefined);
  assert.equal(callTimeoutFor("scenario", { action: "status" }), undefined);
  assert.equal(callTimeoutFor("ping", {}), undefined);
});

test("requested tool timeouts extend the call timeout only when needed", () => {
  assert.equal(callTimeoutFor("capture", { timeoutMs: 5_000 }), undefined);
  assert.equal(callTimeoutFor("capture", { timeoutMs: 60_000 }), 75_000);
});

test("bridge scenario cap matches the host's advertised run cap", () => {
  const scenario = coreTools("fo4").find((tool) => tool.name === "scenario");
  assert.match(scenario.description, new RegExp(`run<=${SCENARIO_RUN_CAP_MS}ms`));
});

test("session timings keep only the stall and ceiling knobs", () => {
  assert.deepEqual(parseSessionTimings({ stallMs: 5_000, ceilingMs: 90_000, stopCeilingMs: 30_000 }), {
    stallMs: 5_000,
    ceilingMs: 90_000,
    stopCeilingMs: 30_000,
  });
  assert.throws(() => parseSessionTimings({ stallMs: 0 }), /from 1 to/);
  assert.throws(() => parseSessionTimings({ stall: 1 }), /not a known timing/);
});

test("removed timing keys fail with the replacement named", () => {
  assert.throws(() => parseSessionTimings({ loadTimeoutMs: 90_000 }), /was removed.*ceilingMs/);
  assert.throws(() => parseSessionTimings({ gracefulStopTimeoutMs: 1 }), /stopCeilingMs/);
  assert.throws(() => parseSessionTimings({ mainThreadStallBudgetMs: 1 }), /stallMs/);
  assert.throws(() => parseSessionTimings({ pollIntervalMs: 1 }), /nothing polls/);
});

test("session config accepts crash-flagged logs and an exit marker", () => {
  const base = {
    mo2Exe: "C:\\MO2\\ModOrganizer.exe",
    profilesDir: "C:\\MO2\\profiles",
    profile: "DevBench",
    executable: "DevBench Launch",
    gameExe: "C:\\Fallout 4\\Fallout4.exe",
    loaderExe: "C:\\Fallout 4\\f4se_loader.exe",
    devbenchDll: "C:\\mods\\devbench.dll",
    resultsDir: "C:\\results",
    logs: [{ path: "C:\\crash", pattern: "crash-*.log", crash: true }],
    exitMarker: { path: "C:\\MO2\\logs\\mo_interface.log", match: "[RootBuilder] Clear complete!" },
  };
  const config = parseSessionConfig(base, "session.json");
  assert.equal(config.logs[0].crash, true);
  assert.equal(config.exitMarker.match, "[RootBuilder] Clear complete!");
  assert.throws(
    () => parseSessionConfig({ ...base, exitMarker: { path: "mo.log", match: "x" } }, "session.json"),
    /absolute Windows path/,
  );
  assert.throws(
    () => parseSessionConfig({ ...base, logs: [{ path: "C:\\crash", crash: "yes" }] }, "session.json"),
    /crash must be a boolean/,
  );
});
