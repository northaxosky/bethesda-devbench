import assert from "node:assert/strict";
import { appendFile, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import test from "node:test";

import {
  FakePlatform,
  createController,
  createFixture,
  startGameServer,
} from "./support/fake-game.mjs";

const RUN = { action: "run", fixture: "DevBenchFixture01", steps: [] };

async function setup(t, { server: serverOptions = {}, platform: platformOptions = {}, timings, config } = {}) {
  const fixture = await createFixture(t, config);
  const server = await startGameServer(fixture, serverOptions);
  t.after(() => server.close());
  const platform = new FakePlatform(fixture, { closeExits: true, server, ...platformOptions });
  const controller = createController(fixture, platform, timings);
  t.after(() => controller.shutdown());
  return { fixture, server, platform, controller };
}

test("run orchestrates fixture, scenario, evidence, and graceful stop", async (t) => {
  const { server, platform, controller } = await setup(t);
  const result = await controller.handle({
    ...RUN,
    steps: [{ assert: { tool: "inspect", args: { kind: "state" }, path: "/playerLoaded", eq: true } }],
  });
  assert.equal(result.isError, false, JSON.stringify(result.value));
  const report = result.value;
  assert.equal(report.phase, "complete");
  assert.equal(report.fixture, "DevBenchFixture01");
  assert.equal(platform.closeCalls, 1);
  assert.equal(platform.terminateCalls, 0);
  assert.equal(platform.releaseCalls, 1);
  assert.equal(platform.releasedWhileShimRunning, false);
  assert.equal(server.loadCalls, 1);
  assert.equal(server.scenarioRunCalls, 1);
  assert.deepEqual(
    report.transcript.filter((entry) => entry.result?.until).map((entry) => entry.stage),
    ["canLoad", "playerReady", "scenario"],
  );
  assert.equal(report.cleanup.shimExited, true);
  assert.equal(report.evidence.logs.length, 1);
  const saved = JSON.parse(await readFile(report.evidence.resultPath, "utf8"));
  assert.equal(saved.phase, "complete");
});

test("start blocks until canLoad and detach returns the job id at once", async (t) => {
  const { fixture, server, platform, controller } = await setup(t, {
    server: { ready: false },
  });
  const blocking = controller.handle({ action: "start" });
  await server.nextHold().catch(() => undefined);
  setTimeout(() => server.becomeReady(), 20);
  const started = await blocking;
  assert.equal(started.isError, false, JSON.stringify(started.value));
  assert.equal(started.value.phase, "complete");
  assert.equal(started.value.transcript.at(-1).stage, "canLoad");
  assert.equal(controller.status().ownership.owned, true);
  assert.equal((await controller.handle({ action: "stop" })).isError, false);

  const detached = createController(fixture, platform);
  t.after(() => detached.shutdown());
  const queued = await detached.handle({ action: "start", detach: true });
  assert.equal(queued.value.queued, true);
  const waited = await detached.handle({ action: "wait", runId: queued.value.runId });
  assert.equal(waited.value.job.state, "done");
  assert.equal(waited.value.job.result.ok, true);
});

test("leave-running releases ownership and a later stop is refused", async (t) => {
  const { platform, controller } = await setup(t);
  const result = await controller.handle({ ...RUN, finish: "leave-running" });
  assert.equal(result.value.ok, true);
  assert.equal(result.value.cleanup.released, true);
  assert.equal(platform.closeCalls, 0);
  assert.equal(platform.releaseCalls, 1);

  const stop = await controller.handle({ action: "stop" });
  assert.equal(stop.isError, true);
  assert.match(stop.value.reason, /does not own/);
});

test("run waits for the game to reach canLoad after the listener is up", async (t) => {
  const { server, controller } = await setup(t, {
    server: { ready: false, bootDelayMs: 20 },
  });
  const result = await controller.handle(RUN);
  assert.equal(result.value.ok, true, JSON.stringify(result.value));
  assert.equal(server.loadCalls, 1);
});

test("a failed load ends the run at once with loadFailed, not at the ceiling", async (t) => {
  const { server, platform, controller } = await setup(t, {
    server: { loadOutcome: "fail" },
  });
  const result = await controller.handle(RUN);
  assert.equal(result.isError, true);
  assert.equal(result.value.phase, "loadfailed");
  assert.equal(result.value.reason, "loadFailed");
  assert.equal(server.loadCalls, 1);
  assert.equal(platform.closeCalls, 1);
});

test("a game that exits mid-wait fails the wait as exited", async (t) => {
  const { server, platform, controller } = await setup(t, {
    server: { loadOutcome: "hang" },
  });
  const pending = controller.handle(RUN);
  await server.nextHold();
  platform.exitGame(1);
  const result = await pending;
  assert.equal(result.value.phase, "loadfailed");
  assert.equal(result.value.reason, "exited");
  assert.equal(result.value.detail.exitCode, 1);
  assert.equal(result.value.cleanup.alreadyExited, true);
});

test("a crash log written before the exit classifies the failure as crashed", async (t) => {
  const { fixture, server, platform, controller } = await setup(t, {
    server: { loadOutcome: "hang" },
  });
  const pending = controller.handle(RUN);
  await server.nextHold();
  const crashLog = join(fixture.crashDir, "crash-2026.log");
  await writeFile(crashLog, "Unhandled exception");
  platform.exitGame(139);
  const result = await pending;
  assert.equal(result.value.reason, "crashed");
  assert.equal(result.value.detail.path, crashLog);
});

test("the game exiting before DevBench is ready is a launch failure", async (t) => {
  const { platform, controller } = await setup(t, {
    platform: { exitOnStart: true, server: undefined },
  });
  const result = await controller.handle({ action: "start" });
  assert.equal(result.value.phase, "launchfailed");
  assert.equal(result.value.reason, "exited");
  assert.match(result.value.error, /exited before DevBench became ready/);
  assert.equal(result.value.pid, 5501);
  assert.equal(platform.startCalls, 1);
});

test("a transient main-thread 504 is re-evaluated on the next event", async (t) => {
  const { server, controller } = await setup(t, { server: { inspectStalls: 2 } });
  const result = await controller.handle(RUN);
  assert.equal(result.value.ok, true, JSON.stringify(result.value));
  assert.equal(server.loadCalls, 1);
});

test("stop waits for the shim exit and for a marker line written after this launch", async (t) => {
  const markerLine = "[RootBuilder] Clear complete!";
  const fixture = await createFixture(t, {});
  const config = JSON.parse(await readFile(fixture.configFile, "utf8"));
  config.exitMarker = { path: fixture.marker, match: markerLine };
  await writeFile(fixture.configFile, JSON.stringify(config));
  await writeFile(fixture.marker, `${markerLine} run 1\nunrelated\n`);
  const server = await startGameServer(fixture);
  t.after(() => server.close());
  const platform = new FakePlatform(fixture, {
    closeExits: true,
    server,
    shimExitDelayMs: 20,
    onShimExit: () => setTimeout(() => {
      appendFile(fixture.marker, `${markerLine} run 2\n`).catch(() => undefined);
    }, 20),
  });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());

  assert.equal((await controller.handle({ action: "start" })).value.ok, true);
  const stop = await controller.handle({ action: "stop" });
  assert.equal(stop.isError, false, JSON.stringify(stop.value));
  assert.equal(stop.value.shimExited, true);
  assert.equal(stop.value.exitMarker.matched, true);
  assert.equal(stop.value.exitMarker.line, `${markerLine} run 2`);
  assert.equal(platform.releasedWhileShimRunning, false);
});

test("a marker line from an earlier launch never satisfies stop", async (t) => {
  const markerLine = "[RootBuilder] Clear complete!";
  const fixture = await createFixture(t, {});
  const config = JSON.parse(await readFile(fixture.configFile, "utf8"));
  config.exitMarker = { path: fixture.marker, match: markerLine };
  await writeFile(fixture.configFile, JSON.stringify(config));
  await writeFile(fixture.marker, `${markerLine} run 1\n`);
  const server = await startGameServer(fixture);
  t.after(() => server.close());
  const platform = new FakePlatform(fixture, { closeExits: true, server });
  const controller = createController(fixture, platform, { stopCeilingMs: 150 });
  t.after(() => controller.shutdown());

  assert.equal((await controller.handle({ action: "start" })).value.ok, true);
  const stop = await controller.handle({ action: "stop" });
  assert.equal(stop.isError, true);
  assert.equal(stop.value.exited, true);
  assert.equal(stop.value.exitMarker.matched, false);
  assert.equal(stop.value.exitMarker.reason, "ceiling");
});

test("stop never adopts borrowed games and force terminates at the stop ceiling", async (t) => {
  const fixture = await createFixture(t);
  const idlePlatform = new FakePlatform(fixture, { closeExits: true });
  const idleController = createController(fixture, idlePlatform);
  t.after(() => idleController.shutdown());
  const borrowedStop = await idleController.handle({ action: "stop", force: true });
  assert.equal(borrowedStop.isError, true);
  assert.equal(idlePlatform.terminateCalls, 0);

  const { platform, controller } = await setup(t, {
    platform: { closeExits: false },
    timings: { stopCeilingMs: 50 },
  });
  const result = await controller.handle({ ...RUN, forceStop: true });
  assert.equal(result.value.ok, true, JSON.stringify(result.value));
  assert.equal(result.value.cleanup.forced, true);
  assert.equal(platform.closeCalls, 1);
  assert.equal(platform.terminateCalls, 1);
});

test("a new bridge reclaims and force-stops a game an earlier bridge left running", async (t) => {
  const { fixture, platform, controller: first } = await setup(t, {
    platform: { closeExits: false },
    timings: { stopCeilingMs: 50 },
  });
  assert.equal((await first.handle({ action: "start" })).value.ok, true);
  await first.shutdown();

  const second = createController(fixture, platform, { stopCeilingMs: 50 });
  t.after(() => second.shutdown());
  const stop = await second.handle({ action: "stop", force: true });
  assert.equal(stop.isError, false, JSON.stringify(stop.value));
  assert.equal(stop.value.exited, true);
  assert.equal(stop.value.forced, true);
  assert.equal(platform.reclaimCalls, 1);
  assert.equal(platform.terminateCalls, 1);

  const again = await second.handle({ action: "stop", force: true });
  assert.equal(again.isError, true);
  assert.match(again.value.reason, /does not own/);
});

test("stopping a reclaimed game whose shim is gone does not wait for shim exit", async (t) => {
  const { fixture, platform, controller: first } = await setup(t, {
    platform: { closeExits: true, reclaimWithoutShim: true },
    timings: { stopCeilingMs: 60_000 },
  });
  assert.equal((await first.handle({ action: "start" })).value.ok, true);
  await first.shutdown();

  const second = createController(fixture, platform, { stopCeilingMs: 60_000 });
  t.after(() => second.shutdown());
  const started = performance.now();
  const stop = await second.handle({ action: "stop" });
  assert.equal(stop.isError, false, JSON.stringify(stop.value));
  assert.equal(stop.value.shimExited, true);
  assert.ok(performance.now() - started < 5_000, "stop waited on the ceiling");
});

test("force stops a game after an uncertain mutation and clears the uncertainty", async (t) => {
  const { platform, controller } = await setup(t, { server: { dropLoad: true } });
  const result = await controller.handle(RUN);
  assert.equal(controller.status().uncertainWrite, true, JSON.stringify(result.value));
  assert.equal(platform.closeCalls, 0);

  const refused = await controller.handle({ action: "stop" });
  assert.equal(refused.isError, true);
  assert.match(refused.value.reason, /force:true/);
  const forced = await controller.handle({ action: "stop", force: true });
  assert.equal(forced.isError, false, JSON.stringify(forced.value));
  assert.equal(forced.value.exited, true);
  assert.equal(controller.status().uncertainWrite, false);
});

test("scenario failure is not a pass and still captures fresh evidence", async (t) => {
  const { platform, controller } = await setup(t, { server: { scenarioOk: false } });
  const result = await controller.handle({ ...RUN, steps: [{ wait: 1 }] });
  assert.equal(result.value.ok, false);
  assert.equal(result.value.phase, "scenariofailed");
  assert.equal(result.value.scenario, undefined);
  assert.equal(platform.closeCalls, 1);
  assert.equal(result.value.evidence.logs.length, 1);
  assert.match(JSON.stringify(result.value), /scenario completed without ok=true/);
});

test("native identity mismatch fails closed and cleans only the owned process", async (t) => {
  const { fixture, platform, controller } = await setup(t);
  platform.options.pluginPath = join(dirname(fixture.dll), "wrong-devbench.dll");
  const result = await controller.handle({ action: "start" });
  assert.equal(result.value.ok, false);
  assert.equal(result.value.phase, "identitymismatch");
  assert.equal(platform.closeCalls, 1);
  assert.equal(platform.terminateCalls, 0);
});

test("a game that exits after a timed-out stop is never reused by the next run", async (t) => {
  const { platform, controller } = await setup(t, {
    platform: { closeExits: false },
    timings: { stopCeilingMs: 50 },
  });
  const first = await controller.handle(RUN);
  assert.equal(first.value.phase, "cleanupfailed");
  assert.equal(controller.status().ownership.owned, true);
  const firstInstance = first.value.instanceId;

  platform.running = false;
  platform.options.closeExits = true;
  const second = await controller.handle(RUN);
  assert.equal(second.value.ok, true, JSON.stringify(second.value));
  assert.equal(platform.startCalls, 2);
  assert.notEqual(second.value.instanceId, firstInstance);
});

test("cancelling a blocking run cancels the job and stops the owned game", async (t) => {
  const { server, platform, controller } = await setup(t, {
    server: { loadOutcome: "hang" },
  });
  const request = new AbortController();
  const pending = controller.handle(RUN, { signal: request.signal });
  await server.nextHold();
  request.abort();
  const result = await pending;
  assert.equal(result.value.phase, "cancelled");
  assert.equal(result.value.reason, "cancelled");
  assert.equal(platform.closeCalls, 1);
});

test("wait until runs a condition against the owned game and ends on cancellation", async (t) => {
  const { server, controller } = await setup(t);
  assert.equal((await controller.handle({ action: "start" })).value.ok, true);

  const menu = controller.handle({ action: "wait", until: "menu", name: "PipboyMenu" });
  await server.nextHold();
  server.publish("menu", { name: "MessageBoxMenu", opening: true });
  server.publish("menu", { name: "PipboyMenu", opening: true });
  const opened = await menu;
  assert.equal(opened.value.ok, true);
  assert.equal(opened.value.satisfied.event.data.name, "PipboyMenu");

  const request = new AbortController();
  const frames = controller.handle(
    { action: "wait", until: "frames", count: 1_000_000 },
    { signal: request.signal },
  );
  await server.nextHold();
  request.abort();
  assert.equal((await frames).value.reason, "cancelled");

  const refused = await controller.handle({ action: "wait", until: "canLoad", timeoutMs: 5 });
  assert.equal(refused.isError, true);
  assert.match(refused.value.reason, /timeoutMs was removed/);
});

test("wait until needs a controller-owned game", async (t) => {
  const fixture = await createFixture(t);
  const controller = createController(fixture, new FakePlatform(fixture));
  t.after(() => controller.shutdown());
  const result = await controller.handle({ action: "wait", until: "canLoad" });
  assert.equal(result.isError, true);
  assert.match(result.value.reason, /controller owns/);
});
