import { createHash } from "node:crypto";
import { appendFile, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { SessionConfigProvider } from "../../dist/config.js";
import { RemoteClient } from "../../dist/remote.js";
import { createRuntimeTarget, processInstanceId } from "../../dist/runtime.js";
import { SessionController } from "../../dist/session.js";

export const GAME_PID = 5501;
export const SHIM_PID = 5500;

export async function createFixture(t, extraConfig = {}) {
  const root = await mkdtemp(join(tmpdir(), "devbench-session-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const profileDir = join(root, "profiles", "DevBench");
  const resultsDir = join(root, "results");
  const crashDir = join(root, "crash");
  await mkdir(profileDir, { recursive: true });
  await mkdir(resultsDir, { recursive: true });
  await mkdir(crashDir, { recursive: true });
  const mo2Exe = join(root, "ModOrganizer.exe");
  const gameExe = join(root, "Fallout4.exe");
  const dll = join(root, "devbench.dll");
  const log = join(root, "devbench.log");
  const marker = join(root, "mo_interface.log");
  const runtimeFile = join(root, "runtime.json");
  const configFile = join(root, "session.json");
  await writeFile(mo2Exe, "fake");
  await writeFile(gameExe, "fake");
  await writeFile(dll, "devbench-test-dll");
  await writeFile(log, "old log line\n");
  const dllSha256 = createHash("sha256").update("devbench-test-dll").digest("hex");
  const config = {
    mo2Exe,
    profilesDir: join(root, "profiles"),
    profile: "DevBench",
    executable: "Fallout 4",
    gameExe,
    loaderExe: join(root, "f4se_loader.exe"),
    devbenchDll: dll,
    runtimeFile,
    expectedRuntime: "1.11.240.0",
    expectedDllSha256: dllSha256,
    resultsDir,
    logs: [
      { path: log, maxBytes: 4096 },
      { path: crashDir, pattern: "crash-*.log", maxBytes: 4096, crash: true },
    ],
    ...extraConfig,
  };
  await writeFile(configFile, JSON.stringify(config));
  return {
    root, mo2Exe, gameExe, dll, log, marker, crashDir,
    runtimeFile, configFile, dllSha256, resultsDir,
  };
}

// Tiny ceilings: a passing test proves the event path, not the ceiling.
export function createController(fixture, platform, timings = {}) {
  const config = new SessionConfigProvider({
    configPath: fixture.configFile,
    explicit: true,
    localAppData: fixture.root,
  });
  return new SessionController({
    config,
    platformFactory: () => platform,
    remoteFactory: () => remoteFor(fixture),
    timings: { stallMs: 30_000, ceilingMs: 20_000, stopCeilingMs: 20_000, ...timings },
  });
}

export function remoteFor(fixture) {
  return new RemoteClient(
    createRuntimeTarget({
      game: "fo4",
      runtimeFile: fixture.runtimeFile,
      localAppData: join(fixture.root, "empty-local"),
    }),
    { discoveryTimeoutMs: 500, callTimeoutMs: 2_000 },
  );
}

export class FakePlatform {
  constructor(fixture, options = {}) {
    this.fixture = fixture;
    this.options = options;
    this.owned = false;
    this.running = false;
    this.shimRunning = false;
    this.recorded = false;
    this.listeners = new Set();
    this.creationTime = "2026-09-15T17:00:00.000Z";
    this.startCalls = 0;
    this.closeCalls = 0;
    this.terminateCalls = 0;
    this.releaseCalls = 0;
    this.reclaimCalls = 0;
    this.releasedWhileShimRunning = false;
  }

  async start() {
    if (this.running) throw new Error("existing game");
    this.startCalls++;
    this.owned = this.recorded = this.running = this.shimRunning = true;
    this.creationTime = new Date().toISOString();
    const server = this.options.server;
    if (server) {
      server.identity.instanceId = processInstanceId(GAME_PID, this.creationTime);
      await writeFile(this.fixture.runtimeFile, JSON.stringify(server.identity));
    }
    if (this.options.exitOnStart) setImmediate(() => this.exitGame(1));
    return this.process();
  }

  async status() {
    return this.owned ? this.process() : { owned: false, running: this.running };
  }

  async inspect() {
    return {
      pid: GAME_PID,
      creationTime: this.creationTime,
      exe: this.fixture.gameExe,
      running: this.running,
      runtimeVersion: "1.11.240.0",
      plugin: {
        path: this.options.pluginPath ?? this.fixture.dll,
        sha256: this.fixture.dllSha256,
      },
    };
  }

  async requestClose() {
    this.closeCalls++;
    if (this.options.closeExits) setImmediate(() => this.exitGame(0));
    return { requested: true, running: this.running };
  }

  async terminate() {
    this.terminateCalls++;
    this.exitGame(1);
    return { exited: true, owned: true };
  }

  async release() {
    if (!this.running && this.shimRunning) this.releasedWhileShimRunning = true;
    this.releaseCalls++;
    this.owned = this.recorded = false;
  }

  async reclaim() {
    if (this.owned || !this.recorded || !this.running) throw new Error("NO_RECLAIMABLE_GAME");
    this.reclaimCalls++;
    this.owned = true;
    if (this.options.reclaimWithoutShim) this.shimRunning = false;
    return this.process();
  }

  async shutdown() {
    this.owned = false;
  }

  onEvent(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  emit(event) {
    for (const listener of [...this.listeners]) listener(event);
  }

  // The game dies (or closes); the shim follows after shimExitDelayMs.
  exitGame(exitCode) {
    if (!this.running) return;
    this.running = this.recorded = false;
    this.emit({
      event: "gameExited", pid: GAME_PID, creationTime: this.creationTime, exitCode,
    });
    if (!this.shimRunning) return;
    setTimeout(() => {
      this.shimRunning = false;
      this.emit({ event: "shimExited", pid: SHIM_PID });
      this.options.onShimExit?.();
    }, this.options.shimExitDelayMs ?? 0);
  }

  process() {
    return {
      owned: true,
      running: this.running,
      pid: GAME_PID,
      creationTime: this.creationTime,
      exe: this.fixture.gameExe,
      mo2Pid: SHIM_PID,
      shimRunning: this.shimRunning,
    };
  }
}

// Fake listener: holds /api/await/events until publish() or waitMs.
export async function startGameServer(fixture, options = {}) {
  const identity = {
    port: 0,
    pid: GAME_PID,
    instanceId: "5501-01DC000000000008",
    exePath: fixture.gameExe,
    runtime: "1.11.240.0",
    dllPath: fixture.dll,
  };
  const game = {
    ready: options.ready ?? true,
    op: null,
    playerLoaded: false,
    openMenus: [],
    messageBoxOpen: false,
    scenarioDone: false,
    frame: 100,
    foreground: true,
  };
  const state = {
    identity,
    game,
    seq: 0,
    events: [],
    oldestSeq: 1,
    eventWaiters: new Set(),
    frameWaiters: new Set(),
    heldAwaits: 0,
    holdListeners: [],
    closeListeners: [],
    closedAwaits: 0,
    loadCalls: 0,
    scenarioRunCalls: 0,
    mutationCalls: [],
    inspectStalls: options.inspectStalls ?? 0,
    tools: [],
  };

  state.publish = (topic, data = {}) => {
    const event = { seq: ++state.seq, frame: game.frame, topic, data };
    state.events.push(event);
    if (options.ringSize && state.events.length > options.ringSize) {
      state.events.shift();
      state.oldestSeq = state.events[0].seq;
    }
    for (const waiter of [...state.eventWaiters]) waiter.check();
    return event;
  };
  state.nextHold = () => new Promise((resolve) => state.holdListeners.push(resolve));
  state.nextClosedAwait = () => new Promise((resolve) => state.closeListeners.push(resolve));
  state.advanceFrame = (count = 1) => {
    game.frame += count;
    for (const waiter of [...state.frameWaiters]) waiter.check();
  };
  state.becomeReady = () => {
    game.ready = true;
    state.publish("lifecycle", { event: "gameLoaded" });
  };
  if (!game.ready && options.bootDelayMs !== undefined) {
    setTimeout(() => state.becomeReady(), options.bootDelayMs);
  }

  const eventBody = (since) => ({
    headSeq: state.seq,
    oldestSeq: state.events.length > 0 ? state.oldestSeq : state.seq + 1,
    events: state.events.filter((event) => event.seq > since),
  });

  const hold = (waiters, request, response, ready, body, waitMs) => {
    state.heldAwaits++;
    for (const resolve of state.holdListeners.splice(0)) resolve();
    const waiter = {
      check: () => {
        if (!ready()) return;
        finish();
        json(response, body());
      },
    };
    const timer = setTimeout(() => {
      finish();
      json(response, body());
    }, waitMs);
    function finish() {
      clearTimeout(timer);
      waiters.delete(waiter);
    }
    response.on("close", () => {
      if (waiters.delete(waiter)) {
        state.closedAwaits++;
        for (const resolve of state.closeListeners.splice(0)) resolve();
      }
      clearTimeout(timer);
    });
    waiters.add(waiter);
  };

  const server = createServer(async (request, response) => {
    if (request.headers["x-devbench-instance"] !== identity.instanceId) {
      return json(response, { error: "instance mismatch" }, 409);
    }
    const url = new URL(request.url, "http://127.0.0.1");
    if (request.method === "GET" && url.pathname === "/api/health") {
      return json(response, { ok: true, ...identity });
    }
    if (request.method === "GET" && url.pathname === "/api/tools") {
      return json(response, { tools: state.tools });
    }
    if (request.method === "GET" && url.pathname === "/api/events") {
      return json(response, eventBody(Number(url.searchParams.get("since"))));
    }
    if (request.method === "GET" && url.pathname === "/api/await/events") {
      const since = Number(url.searchParams.get("since"));
      const waitMs = Math.min(Number(url.searchParams.get("waitMs")), 25_000);
      const body = () => eventBody(since);
      if (body().events.length > 0 || waitMs === 0) return json(response, body());
      return hold(state.eventWaiters, request, response, () => body().events.length > 0, body, waitMs);
    }
    if (request.method === "GET" && url.pathname === "/api/await/frame") {
      const after = Number(url.searchParams.get("after"));
      const waitMs = Math.min(Number(url.searchParams.get("waitMs")), 25_000);
      const body = () => ({ frame: game.frame, advanced: game.frame > after });
      if (game.frame > after || waitMs === 0) return json(response, body());
      return hold(state.frameWaiters, request, response, () => game.frame > after, body, waitMs);
    }
    const match = url.pathname.match(/^\/api\/tool\/([^/]+)$/);
    if (request.method !== "POST" || !match) {
      return json(response, { error: "not found" }, 404);
    }
    const args = await readBody(request);
    return handleTool(decodeURIComponent(match[1]), args, request, response);
  });

  function handleTool(tool, args, request, response) {
    if (tool === "game" && args.action === "status") {
      return json(response, {
        actionsAllowed: options.actionsAllowed ?? true,
        gameLoaded: game.ready,
        canLoad: game.ready,
        operation: game.op,
      });
    }
    if (tool === "game" && args.action === "load") {
      if (!game.ready) {
        return json(response, { error: "load attempted before game initialization" }, 409);
      }
      state.loadCalls++;
      if (options.dropLoad) return request.socket.destroy();
      game.op = { operationId: 31, phase: "queued", success: null, active: true };
      state.publish("lifecycle", { event: "preLoadGame" });
      const receipt = { queued: true, operationId: 31, actionCursor: state.seq };
      const outcome = options.loadOutcome ?? "succeed";
      if (outcome !== "hang") {
        setTimeout(() => {
          const success = outcome === "succeed";
          game.op = {
            operationId: 31,
            phase: success ? "succeeded" : "failed",
            success,
            active: false,
          };
          game.playerLoaded = success;
          state.publish("lifecycle", { event: "postLoadGame", success });
        }, 5);
      }
      return json(response, receipt);
    }
    if (tool === "inspect" && args.kind === "state") {
      if (state.inspectStalls-- > 0) {
        // The game keeps publishing events while its main thread is busy.
        setTimeout(() => state.publish("lifecycle", { event: "tick" }), 5);
        return json(response, { code: 504, error: "main-thread task did not start" }, 504);
      }
      return json(response, { playerLoaded: game.playerLoaded });
    }
    if (tool === "inspect" && args.kind === "health") {
      return json(response, {
        frame: game.frame, pendingTasks: 3, lastTaskFrame: 40, foreground: game.foreground,
      });
    }
    if (tool === "menu" && args.action === "list") {
      return json(response, { openMenus: game.openMenus, messageBoxOpen: game.messageBoxOpen });
    }
    if (tool === "menu" && args.action === "describe") {
      return json(response, { menu: "MessageBoxMenu", text: "Missing content", buttons: ["OK"] });
    }
    if (tool === "menu" && (args.action === "open" || args.action === "close")) {
      state.mutationCalls.push({ tool, args });
      const opening = args.action === "open";
      game.openMenus = opening
        ? [...game.openMenus, args.name]
        : game.openMenus.filter((name) => name !== args.name);
      setTimeout(() => state.publish("menu", { name: args.name, opening }), 5);
      return json(response, { queued: true, acknowledged: true });
    }
    if (tool === "scenario" && args.action === "run") {
      state.scenarioRunCalls++;
      game.scenarioDone = false;
      appendFile(fixture.log, "current run line\n").catch(() => undefined);
      state.publish("scenario.started", { runId: 77 });
      setTimeout(() => {
        game.scenarioDone = true;
        state.publish("scenario.finished", { runId: 77 });
      }, 5);
      return json(response, { queued: true, runId: 77 });
    }
    if (tool === "scenario" && args.action === "status") {
      const ok = options.scenarioOk ?? true;
      return json(response, {
        runId: 77,
        done: game.scenarioDone,
        ...(game.scenarioDone
          ? {
              ok,
              result: {
                ok,
                transcript: [{ index: 0, ok }],
                ...(ok ? {} : { error: "assertion failed" }),
              },
            }
          : { status: "running" }),
      });
    }
    if (tool === "scenario" && args.action === "cancel") {
      return json(response, { runId: 77, cancelRequested: true });
    }
    if (tool === "capture") {
      state.mutationCalls.push({ tool, args });
      if (options.captureEvent) {
        state.publish("communityshaders.capture.completed", {
          requestId: "r-1", frames: 1, captures: [],
        });
      }
      return json(response, { queued: true, requestId: "r-1" });
    }
    return json(response, { error: "unexpected call" }, 400);
  }

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  identity.port = server.address().port;
  state.close = () =>
    new Promise((resolve) => {
      if (!server.listening) return resolve();
      server.closeAllConnections();
      server.close(resolve);
    });
  return state;
}

export async function writeRuntime(fixture, server) {
  await writeFile(fixture.runtimeFile, JSON.stringify(server.identity));
}

export async function readBody(request) {
  let body = "";
  for await (const chunk of request) body += chunk;
  return body ? JSON.parse(body) : {};
}

export function json(response, body, status = 200) {
  response.writeHead(status, { "Content-Type": "application/json" });
  response.end(JSON.stringify(body));
}
