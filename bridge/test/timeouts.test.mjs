import assert from "node:assert/strict";
import test from "node:test";

import { coreTools } from "../dist/catalog.js";
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

test("session timing overrides reject unknown keys and out-of-range values", () => {
  assert.deepEqual(parseSessionTimings({ loadTimeoutMs: 90_000 }), {
    loadTimeoutMs: 90_000,
  });
  assert.throws(() => parseSessionTimings({ loadTimeout: 1 }), /not a known timing/);
  assert.throws(() => parseSessionTimings({ gracefulStopTimeoutMs: 31_000 }), /from 1 to 30000/);
});
