import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  appendFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

import { SessionConfigProvider } from "../dist/config.js";
import { RemoteClient } from "../dist/remote.js";
import { createRuntimeTarget } from "../dist/runtime.js";
import { SessionController } from "../dist/session.js";

test("session run orchestrates fixture, scenario, evidence, and graceful stop", async (t) => {
  const fixture = await createFixture(t);
  const server = await startWorkflowServer(fixture, { scenarioOk: true });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const platform = new FakePlatform(fixture, { closeExits: true });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());

  const queued = await controller.handle({
    action: "run",
    fixture: "DevBenchFixture01",
    steps: [{ assert: { tool: "inspect", args: { kind: "state" }, path: "/playerLoaded", eq: true } }],
  });
  assert.equal(queued.isError, false);
  const runId = queued.value.runId;
  const status = await waitForJob(controller, runId);
  assert.equal(status.job.result.ok, true);
  assert.equal(status.job.result.phase, "complete");
  assert.equal(status.job.result.fixture, "DevBenchFixture01");
  assert.equal(platform.closeCalls, 1);
  assert.equal(platform.terminateCalls, 0);
  assert.equal(platform.releaseCalls, 1);
  assert.equal(server.loadCalls, 1);
  assert.equal(server.scenarioRunCalls, 1);
  assert.ok(status.job.result.transcript.some((entry) => entry.stage === "load.ready"));
  assert.equal(status.job.result.evidence.logs.length, 1);
  const saved = JSON.parse(
    await readFile(status.job.result.evidence.resultPath, "utf8"),
  );
  assert.equal(saved.phase, "complete");
});

test("leave-running releases ownership and a later stop is refused", async (t) => {
  const fixture = await createFixture(t);
  const server = await startWorkflowServer(fixture, { scenarioOk: true });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const platform = new FakePlatform(fixture, { closeExits: true });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());

  const queued = await controller.handle({
    action: "run",
    fixture: "DevBenchFixture02",
    steps: [],
    finish: "leave-running",
  });
  const status = await waitForJob(controller, queued.value.runId);
  assert.equal(status.job.result.ok, true);
  assert.equal(status.job.result.cleanup.released, true);
  assert.equal(platform.closeCalls, 0);
  assert.equal(platform.releaseCalls, 1);

  const stop = await controller.handle({ action: "stop" });
  assert.equal(stop.isError, true);
  assert.match(stop.value.reason, /does not own/);
});

test("run waits for native load availability after the HTTP listener starts", async (t) => {
  const fixture = await createFixture(t);
  const server = await startWorkflowServer(fixture, { scenarioOk: true, bootPolls: 3 });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const platform = new FakePlatform(fixture, { closeExits: true });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());
  const queued = await controller.handle({
    action: "run", fixture: "DevBenchFixture01", steps: [],
  });
  const status = await waitForJob(controller, queued.value.runId);
  assert.equal(status.job.result.ok, true);
  assert.equal(server.loadCalls, 1);
});

test("stop never adopts borrowed games and force is explicit", async (t) => {
  const fixture = await createFixture(t);
  const idlePlatform = new FakePlatform(fixture, { closeExits: true });
  const idleController = createController(fixture, idlePlatform);
  t.after(() => idleController.shutdown());
  const borrowedStop = await idleController.handle({ action: "stop", force: true });
  assert.equal(borrowedStop.isError, true);
  assert.equal(idlePlatform.terminateCalls, 0);

  const server = await startWorkflowServer(fixture, { scenarioOk: true });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const forcePlatform = new FakePlatform(fixture, { closeExits: false });
  const forceController = createController(fixture, forcePlatform);
  t.after(() => forceController.shutdown());
  const queued = await forceController.handle({
    action: "run",
    fixture: "DevBenchFixture03",
    steps: [],
    forceStop: true,
  });
  const status = await waitForJob(forceController, queued.value.runId);
  assert.equal(status.job.result.ok, true);
  assert.equal(status.job.result.cleanup.forced, true);
  assert.equal(forcePlatform.closeCalls, 1);
  assert.equal(forcePlatform.terminateCalls, 1);
});

test("scenario failure is not a pass and still captures fresh evidence", async (t) => {
  const fixture = await createFixture(t);
  const server = await startWorkflowServer(fixture, { scenarioOk: false });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const platform = new FakePlatform(fixture, { closeExits: true });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());

  const queued = await controller.handle({
    action: "run",
    fixture: "DevBenchFixture04",
    steps: [{ wait: 1 }],
  });
  const status = await waitForJob(controller, queued.value.runId);
  assert.equal(status.job.result.ok, false);
  assert.equal(status.job.result.phase, "scenariofailed");
  assert.equal(status.job.result.scenario, undefined);
  assert.equal(platform.closeCalls, 1);
  assert.equal(status.job.result.evidence.logs.length, 1);
  assert.match(
    JSON.stringify(status.job.result),
    /scenario completed without ok=true/,
  );
});

test("native identity mismatch fails closed and cleans only the owned process", async (t) => {
  const fixture = await createFixture(t);
  const server = await startWorkflowServer(fixture, { scenarioOk: true });
  t.after(() => server.close());
  await writeRuntime(fixture.runtimeFile, server.identity);
  const platform = new FakePlatform(fixture, {
    closeExits: true,
    pluginPath: join(dirname(fixture.dll), "wrong-devbench.dll"),
  });
  const controller = createController(fixture, platform);
  t.after(() => controller.shutdown());

  const queued = await controller.handle({ action: "start" });
  const status = await waitForJob(controller, queued.value.runId);
  assert.equal(status.job.result.ok, false);
  assert.equal(status.job.result.phase, "identitymismatch");
  assert.equal(platform.closeCalls, 1);
  assert.equal(platform.terminateCalls, 0);
});

test("startup timeout is a launch failure with owned-process evidence and cleanup", async (t) => {
  const fixture = await createFixture(t);
  await writeFile(
    fixture.runtimeFile,
    JSON.stringify({
      port: 65534,
      pid: 5501,
      instanceId: "5501-01DC000000000009",
      exePath: fixture.gameExe,
      runtime: "1.11.240.0",
      dllPath: fixture.dll,
    }),
  );
  const platform = new FakePlatform(fixture, { closeExits: true });
  const controller = createController(fixture, platform, {
    startupTimeoutMs: 40,
    pollIntervalMs: 5,
  });
  t.after(() => controller.shutdown());

  const queued = await controller.handle({ action: "start" });
  const status = await waitForJob(controller, queued.value.runId);
  assert.equal(status.job.result.ok, false);
  assert.equal(status.job.result.phase, "launchfailed");
  assert.equal(status.job.result.pid, 5501);
  assert.equal(platform.closeCalls, 1);
});

class FakePlatform {
  constructor(fixture, options) {
    this.fixture = fixture;
    this.options = options;
    this.owned = false;
    this.running = false;
    this.closeCalls = 0;
    this.terminateCalls = 0;
    this.releaseCalls = 0;
  }

  async start() {
    if (this.running) throw new Error("existing game");
    this.owned = true;
    this.running = true;
    return this.process();
  }

  async status() {
    return this.owned
      ? this.process()
      : { owned: false, running: this.running };
  }

  async inspect() {
    return {
      pid: 5501,
      creationTime: "2026-09-15T17:00:00.000Z",
      exe: this.fixture.gameExe,
      running: this.running,
      runtimeVersion: "1.11.240.0",
      plugin: {
        path: this.options.pluginPath ?? this.fixture.dll,
        sha256: this.fixture.dllSha256,
      },
    };
  }

  async close() {
    this.closeCalls++;
    if (this.options.closeExits) this.running = false;
    return this.options.closeExits
      ? { exited: true, owned: true }
      : { exited: false, owned: true, reason: "timeout" };
  }

  async terminate() {
    this.terminateCalls++;
    this.running = false;
    return { exited: true, owned: true };
  }

  async release() {
    this.releaseCalls++;
    this.owned = false;
  }

  async shutdown() {
    if (this.owned) await this.release();
  }

  process() {
    return {
      owned: true,
      running: this.running,
      pid: 5501,
      creationTime: "2026-09-15T17:00:00.000Z",
      exe: this.fixture.gameExe,
      mo2Pid: 5500,
    };
  }
}

function createController(fixture, platform, timingOverrides = {}) {
  const config = new SessionConfigProvider({
    configPath: fixture.configFile,
    explicit: true,
    localAppData: fixture.root,
  });
  return new SessionController({
    config,
    platformFactory: () => platform,
    remoteFactory: () =>
      new RemoteClient(
        createRuntimeTarget({
          game: "fo4",
          runtimeFile: fixture.runtimeFile,
          localAppData: join(fixture.root, "empty-local"),
        }),
        { discoveryTimeoutMs: 100, callTimeoutMs: 500 },
      ),
    timings: {
      startupTimeoutMs: 1_000,
      loadTimeoutMs: 1_000,
      scenarioTimeoutMs: 1_000,
      gracefulStopTimeoutMs: 50,
      platformStartTimeoutMs: 100,
      pollIntervalMs: 5,
      ...timingOverrides,
    },
  });
}

async function createFixture(t) {
  const root = await mkdtemp(join(tmpdir(), "devbench-session-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const profileDir = join(root, "profiles", "DevBench");
  const resultsDir = join(root, "results");
  await mkdir(profileDir, { recursive: true });
  await mkdir(resultsDir, { recursive: true });
  const mo2Exe = join(root, "ModOrganizer.exe");
  const gameExe = join(root, "Fallout4.exe");
  const dll = join(root, "devbench.dll");
  const log = join(root, "devbench.log");
  const runtimeFile = join(root, "runtime.json");
  const configFile = join(root, "session.json");
  await writeFile(mo2Exe, "fake");
  await writeFile(gameExe, "fake");
  await writeFile(dll, "devbench-test-dll");
  await writeFile(log, "old log line\n");
  const dllSha256 = createHash("sha256")
    .update("devbench-test-dll")
    .digest("hex");
  await writeFile(
    configFile,
    JSON.stringify({
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
      logs: [{ path: log, maxBytes: 4096 }],
    }),
  );
  return {
    root,
    mo2Exe,
    gameExe,
    dll,
    log,
    runtimeFile,
    configFile,
    dllSha256,
  };
}

async function startWorkflowServer(fixture, options) {
  const identity = {
    port: 0,
    pid: 5501,
    instanceId: "5501-01DC000000000008",
    exePath: fixture.gameExe,
    runtime: "1.11.240.0",
    dllPath: fixture.dll,
  };
  let gameStatusCalls = 0;
  let scenarioStatusCalls = 0;
  const state = {
    identity,
    loadCalls: 0,
    scenarioRunCalls: 0,
  };
  const server = createServer(async (request, response) => {
    if (request.headers["x-devbench-instance"] !== identity.instanceId) {
      return json(response, { error: "instance mismatch" }, 409);
    }
    if (request.method === "GET" && request.url === "/api/health") {
      return json(response, { ok: true, ...identity });
    }
    if (request.method === "GET" && request.url?.startsWith("/api/events")) {
      return json(response, {
        headSeq: 50 + scenarioStatusCalls,
        events:
          scenarioStatusCalls > 0
            ? [
                {
                  seq: 50 + scenarioStatusCalls,
                  topic: "scenario.step",
                  data: { runId: 77 },
                },
              ]
            : [],
      });
    }
    const match = request.url?.match(/^\/api\/tool\/([^/?]+)$/);
    if (request.method !== "POST" || !match) {
      return json(response, { error: "not found" }, 404);
    }
    const args = await readBody(request);
    const tool = decodeURIComponent(match[1]);
    if (tool === "game" && args.action === "load") {
      if (gameStatusCalls < (options.bootPolls ?? 1)) {
        return json(response, { error: "load attempted before game initialization" }, 409);
      }
      state.loadCalls++;
      return json(response, {
        queued: true,
        operationId: 31,
        actionCursor: 10,
      });
    }
    if (tool === "game" && args.action === "status") {
      gameStatusCalls++;
      return json(response, {
        actionsAllowed: true,
        gameLoaded: gameStatusCalls >= (options.bootPolls ?? 1),
        canLoad: gameStatusCalls >= (options.bootPolls ?? 1),
        operation: {
          operationId: 31,
          phase: gameStatusCalls > 1 ? "succeeded" : "queued",
          success: gameStatusCalls > 1 ? true : null,
        },
      });
    }
    if (tool === "inspect") {
      return json(response, { playerLoaded: true });
    }
    if (tool === "scenario" && args.action === "run") {
      state.scenarioRunCalls++;
      await appendFile(fixture.log, "current run line\n");
      return json(response, { queued: true, runId: 77 });
    }
    if (tool === "scenario" && args.action === "status") {
      scenarioStatusCalls++;
      return json(response, {
        runId: 77,
        done: scenarioStatusCalls > 1,
        ...(scenarioStatusCalls > 1
          ? {
              ok: options.scenarioOk,
              result: {
                ok: options.scenarioOk,
                transcript: [{ index: 0, ok: options.scenarioOk }],
                ...(options.scenarioOk ? {} : { error: "assertion failed" }),
              },
            }
          : { status: "running" }),
      });
    }
    if (tool === "scenario" && args.action === "cancel") {
      return json(response, { runId: 77, cancelRequested: true });
    }
    return json(response, { error: "unexpected call" }, 400);
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  identity.port = server.address().port;
  state.close = () =>
    new Promise((resolve) => {
      if (!server.listening) resolve();
      else server.close(resolve);
    });
  return state;
}

async function writeRuntime(path, identity) {
  await writeFile(path, JSON.stringify(identity));
}

async function waitForJob(controller, runId) {
  const deadline = Date.now() + 5_000;
  while (Date.now() < deadline) {
    const status = controller.status();
    if (status.job?.runId === runId && status.job.state === "done") return status;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.fail(`timed out waiting for ${runId}`);
}

async function readBody(request) {
  let body = "";
  for await (const chunk of request) body += chunk;
  return body ? JSON.parse(body) : {};
}

function json(response, body, status = 200) {
  response.writeHead(status, { "Content-Type": "application/json" });
  response.end(JSON.stringify(body));
}
