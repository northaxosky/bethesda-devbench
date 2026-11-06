import assert from "node:assert/strict";
import { appendFile, mkdir, mkdtemp, open, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { RemoteProxy } from "../dist/proxy.js";
import { RemoteClient } from "../dist/remote.js";
import { createRuntimeTarget } from "../dist/runtime.js";
import {
  canLoad,
  eventOccurred,
  framesElapsed,
  menuState,
  playerReady,
} from "../dist/waits/conditions.js";
import { ExitTracker } from "../dist/waits/exits.js";
import { awaitMarkerLine } from "../dist/waits/files.js";
import { awaitCondition } from "../dist/waits/waiter.js";
import {
  FakePlatform,
  GAME_PID,
  createFixture,
  remoteFor,
  startGameServer,
  writeRuntime,
} from "./support/fake-game.mjs";

const TIMINGS = { stallMs: 5_000, ceilingMs: 10_000 };

async function setup(t, serverOptions = {}) {
  const fixture = await createFixture(t);
  const server = await startGameServer(fixture, serverOptions);
  t.after(() => server.close());
  await writeRuntime(fixture, server);
  return { fixture, server, remote: remoteFor(fixture) };
}

function wait(remote, condition, extra = {}) {
  return awaitCondition({ remote, condition, timings: TIMINGS, ...extra });
}

test("playerReady ignores another operation and completes on its own", async (t) => {
  const { server, remote } = await setup(t);
  server.game.op = { operationId: 99, phase: "failed", success: false, active: false };
  const pending = wait(remote, playerReady({ operationId: 31 }));
  await server.nextHold();
  server.game.op = { operationId: 31, phase: "succeeded", success: true, active: false };
  server.game.playerLoaded = true;
  server.publish("lifecycle", { event: "postLoadGame", success: true });
  const result = await pending;
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.equal(result.satisfied.operation.operationId, 31);
});

test("a failed postLoadGame after the action cursor ends playerReady at once", async (t) => {
  const { server, remote } = await setup(t);
  const actionCursor = server.seq;
  const pending = wait(remote, playerReady({ actionCursor }));
  await server.nextHold();
  server.publish("lifecycle", { event: "postLoadGame", success: false });
  const result = await pending;
  assert.equal(result.ok, false);
  assert.equal(result.reason, "loadFailed");
});

test("an event wait with a correlation id matches an event fired before the wait", async (t) => {
  const { server, remote } = await setup(t);
  server.publish("communityshaders.capture.completed", { requestId: "r-0" });
  server.publish("communityshaders.capture.completed", { requestId: "r-1", frames: 2 });
  const result = await wait(remote, eventOccurred({
    topic: "communityshaders.capture.completed", match: { requestId: "r-1" },
  }));
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.equal(result.satisfied.event.data.frames, 2);
});

test("an event wait after a cursor fails when the ring dropped those events", async (t) => {
  const { server, remote } = await setup(t, { ringSize: 2 });
  for (let index = 0; index < 5; index++) server.publish("noise", { index });
  const result = await wait(remote, eventOccurred({ topic: "capture.saved", after: 1 }));
  assert.equal(result.ok, false);
  assert.equal(result.reason, "eventHistoryLost");
});

test("a menu wait reads current state first and ignores other menus", async (t) => {
  const { server, remote } = await setup(t);
  server.game.openMenus = ["PipboyMenu"];
  assert.equal((await wait(remote, menuState({ name: "PipboyMenu", open: true }))).ok, true);

  const pending = wait(remote, menuState({ name: "ContainerMenu", open: true }));
  await server.nextHold();
  server.publish("menu", { name: "LevelUpMenu", opening: true });
  server.publish("menu", { name: "ContainerMenu", opening: false });
  server.publish("menu", { name: "ContainerMenu", opening: true });
  const result = await pending;
  assert.equal(result.ok, true);
  assert.equal(result.satisfied.event.data.name, "ContainerMenu");
  assert.equal(result.satisfied.event.data.opening, true);
});

test("a message box opening during canLoad fails as modal with its text", async (t) => {
  const { server, remote } = await setup(t, { ready: false });
  const pending = wait(remote, canLoad());
  await server.nextHold();
  server.publish("menu", { name: "MessageBoxMenu", opening: true });
  const result = await pending;
  assert.equal(result.ok, false);
  assert.equal(result.reason, "modal");
  assert.equal(result.detail.snapshot.text, "Missing content");
});

test("disabled native actions fail canLoad immediately", async (t) => {
  const { remote } = await setup(t, { actionsAllowed: false });
  const result = await wait(remote, canLoad());
  assert.equal(result.reason, "actionsDisabled");
});

test("a frames wait finishes when the game renders that many frames", async (t) => {
  const { server, remote } = await setup(t);
  const pending = wait(remote, framesElapsed(3));
  await server.nextHold();
  server.advanceFrame();
  server.advanceFrame(2);
  const result = await pending;
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.equal(result.satisfied.endFrame - result.satisfied.startFrame >= 3, true);
});

test("a foreground main-thread stall fails the wait with the health snapshot", async (t) => {
  const { server, remote } = await setup(t);
  const pending = wait(remote, eventOccurred({ topic: "never" }), {
    timings: { stallMs: 40, ceilingMs: 10_000 },
  });
  await server.nextHold();
  server.publish("health.stalled", { frame: 100, stalledForMs: 0, foreground: true });
  const result = await pending;
  assert.equal(result.reason, "stalled");
  assert.equal(result.detail.pendingTasks, 3);
});

test("a stall already in progress when the wait starts is detected from the ring", async (t) => {
  const { server, remote } = await setup(t);
  server.publish("health.stalled", { frame: 100, stalledForMs: 0, foreground: true });
  const result = await wait(remote, eventOccurred({ topic: "never" }), {
    timings: { stallMs: 30, ceilingMs: 10_000 },
  });
  assert.equal(result.reason, "stalled");
});

test("a stall while the window is in the background is reported, not failed", async (t) => {
  const { server, remote } = await setup(t);
  server.game.foreground = false;
  const messages = [];
  let sawBackground;
  const background = new Promise((resolve) => { sawBackground = resolve; });
  const pending = wait(remote, eventOccurred({ topic: "done" }), {
    timings: { stallMs: 30, ceilingMs: 10_000 },
    progress: (message) => {
      messages.push(message);
      if (message.includes("(background)")) sawBackground();
    },
  });
  await server.nextHold();
  server.publish("health.stalled", { frame: 100, stalledForMs: 0, foreground: false });
  await background;
  server.publish("health.resumed", { frame: 101, stalledForMs: 60 });
  server.publish("done");
  const result = await pending;
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.ok(messages.some((message) => message.includes("(background)")), messages.join("|"));
});

function ownedSignals(fixture, platform) {
  return {
    instanceId: "5501-01DC000000000008",
    exits: new ExitTracker(platform, { pid: GAME_PID }),
    crashLogs: [{ path: fixture.crashDir, pattern: "crash-*.log", maxBytes: 1, crash: true }],
  };
}

test("a crash log that appears or grows after the wait began fails it as crashed", async (t) => {
  const { fixture, server, remote } = await setup(t);
  const platform = new FakePlatform(fixture);
  platform.running = true;
  const old = join(fixture.crashDir, "crash-old.log");
  await writeFile(old, "from an earlier run");
  const owned = ownedSignals(fixture, platform);
  t.after(() => owned.exits.dispose());

  const grown = wait(remote, eventOccurred({ topic: "never" }), { owned });
  await server.nextHold();
  await appendFile(old, " plus a new crash");
  const grownResult = await grown;
  assert.equal(grownResult.reason, "crashed");
  assert.equal(grownResult.detail.path, old);

  const fresh = wait(remote, eventOccurred({ topic: "never" }), { owned });
  await server.nextHold();
  const created = join(fixture.crashDir, "crash-new.log");
  await writeFile(created, "boom");
  assert.equal((await fresh).detail.path, created);
});

test("a game exit ends the wait as exited long before the ceiling", async (t) => {
  const { fixture, server, remote } = await setup(t);
  const platform = new FakePlatform(fixture);
  platform.running = true;
  const owned = ownedSignals(fixture, platform);
  t.after(() => owned.exits.dispose());
  const pending = wait(remote, eventOccurred({ topic: "never" }), { owned });
  await server.nextHold();
  platform.exitGame(3);
  const result = await pending;
  assert.equal(result.reason, "exited");
  assert.equal(result.detail.exitCode, 3);
});

test("a listener that goes away mid-wait ends it as unavailable", async (t) => {
  const { server, remote } = await setup(t);
  const pending = wait(remote, eventOccurred({ topic: "never" }));
  await server.nextHold();
  await server.close();
  const result = await pending;
  assert.equal(result.reason, "unavailable");
});

test("cancellation ends a wait at once", async (t) => {
  const { server, remote } = await setup(t);
  const request = new AbortController();
  const pending = wait(remote, eventOccurred({ topic: "never" }), { signal: request.signal });
  await server.nextHold();
  request.abort();
  assert.equal((await pending).reason, "cancelled");
});

test("the ceiling is reported as ceiling when nothing else fires", async (t) => {
  const { server, remote } = await setup(t);
  const pending = wait(remote, eventOccurred({ topic: "never" }), {
    timings: { stallMs: 5_000, ceilingMs: 60 },
  });
  await server.nextHold();
  assert.equal((await pending).reason, "ceiling");
});

function proxyFor(fixture, environment) {
  const proxy = new RemoteProxy(remoteFor(fixture), { pollIntervalMs: 0, ...(environment ? { environment } : {}) });
  return proxy;
}

test("a proxied game load awaits its outcome by default and never re-sends", async (t) => {
  const { fixture, server } = await setup(t);
  const proxy = proxyFor(fixture);
  t.after(() => proxy.close());
  const result = await proxy.callTool("game", { action: "load", name: "Fixture" });
  assert.equal(result.isError, false, JSON.stringify(result.value));
  assert.equal(result.value.queued, true);
  assert.equal(result.value.outcome.ok, true);
  assert.equal(result.value.outcome.until, "playerReady");
  assert.equal(server.loadCalls, 1);

  const bare = await proxy.callTool("game", { action: "load", name: "Fixture", await: false });
  assert.equal(bare.isError, false);
  assert.equal(bare.value.outcome, undefined);
  assert.equal(server.loadCalls, 2);
});

test("a proxied load failure is an error outcome with the receipt kept", async (t) => {
  const { fixture, server } = await setup(t, { loadOutcome: "fail" });
  const proxy = proxyFor(fixture);
  t.after(() => proxy.close());
  const result = await proxy.callTool("game", { action: "load", name: "Fixture" });
  assert.equal(result.isError, true);
  assert.equal(result.value.operationId, 31);
  assert.equal(result.value.outcome.reason, "loadFailed");
  assert.equal(server.loadCalls, 1);
});

test("await {event, correlate} waits for the correlated event and is not forwarded", async (t) => {
  const { fixture, server } = await setup(t, { captureEvent: true });
  const proxy = proxyFor(fixture);
  t.after(() => proxy.close());
  const result = await proxy.callTool("capture", {
    frames: 1,
    await: { event: "communityshaders.capture.completed", correlate: "requestId" },
  });
  assert.equal(result.isError, false, JSON.stringify(result.value));
  assert.equal(result.value.outcome.satisfied.event.data.requestId, "r-1");
  assert.deepEqual(server.mutationCalls[0].args, { frames: 1 });

  const missing = await proxy.callTool("capture", {
    await: { event: "x.done", correlate: "nonexistent" },
  });
  assert.equal(missing.isError, true);
  assert.equal(missing.value.outcome.reason, "uncorrelated");
});

test("mutating tools advertise await and read-only tools do not, live and offline", async (t) => {
  const { fixture, server } = await setup(t);
  server.tools = [
    { name: "act", description: "x", inputSchema: { type: "object", properties: {} }, readOnly: false },
    { name: "look", description: "x", inputSchema: { type: "object", properties: {} }, readOnly: true },
  ];
  const live = proxyFor(fixture);
  t.after(() => live.close());
  const liveTools = await live.listRemoteTools();
  assert.ok(liveTools.find((tool) => tool.name === "act").inputSchema.properties.await);
  assert.equal(liveTools.find((tool) => tool.name === "look").inputSchema.properties.await, undefined);

  const root = await mkdtemp(join(tmpdir(), "devbench-offline-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(join(root, "local"));
  const offline = new RemoteProxy(
    new RemoteClient(createRuntimeTarget({
      game: "fo4", runtimeFile: join(root, "missing.json"), localAppData: join(root, "local"),
    })),
    { pollIntervalMs: 0 },
  );
  t.after(() => offline.close());
  const offlineTools = await offline.listRemoteTools();
  const mutating = offlineTools.filter((tool) => tool.inputSchema.properties?.await);
  assert.ok(mutating.length > 0);
  assert.ok(offlineTools.some((tool) => tool.annotations?.readOnlyHint && !tool.inputSchema.properties?.await));
});

test("a marker search reads only appended lines and survives truncation", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-marker-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const path = join(root, "mo.log");
  await writeFile(path, "Clear complete! old\nsome more text\n");
  const { size: offset } = await stat(path);
  const controller = new AbortController();
  const found = awaitMarkerLine({ path, match: "Clear complete!", offset }, controller.signal);
  await appendFile(path, "unrelated line\n");
  await writeFile(path, "rotated\nClear complete! new\n");
  assert.equal(await found, "Clear complete! new");
});

// MO2 keeps its log open; Windows sends no change event for such appends.
test("a marker appended through a held-open handle is still found", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-marker-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const path = join(root, "mo.log");
  await writeFile(path, "old\n");
  const writer = await open(path, "a");
  t.after(() => writer.close());
  const controller = new AbortController();
  const found = awaitMarkerLine({ path, match: "Clear complete!", offset: 4 }, controller.signal);
  // Let the first probe miss so only a later re-read can find the line.
  await new Promise((resolve) => setTimeout(resolve, 100));
  await writer.write("[RootBuilder] Clear complete!\n");
  assert.equal(await found, "[RootBuilder] Clear complete!");
});
