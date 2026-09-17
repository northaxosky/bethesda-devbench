import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { ToolListChangedNotificationSchema } from "@modelcontextprotocol/sdk/types.js";

import { coreTools } from "../dist/catalog.js";
import { SessionConfigProvider } from "../dist/config.js";
import { IdentityChangedError, RemoteClient, UncertainMutationError } from "../dist/remote.js";
import {
  createRuntimeTarget,
  readRuntimeIdentity,
} from "../dist/runtime.js";

const bridgeEntry = new URL("../dist/index.js", import.meta.url).pathname.slice(1);

test("Node bridge cold-starts with the selected game's offline catalog", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-cold-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  for (const game of ["fo4", "se", "vr"]) {
    const { client } = await startBridge(undefined, join(root, "local"), game);
    t.after(() => client.close());

    const tools = await client.listTools();
    assert.deepEqual(tools.tools.map((tool) => tool.name), offlineToolNames(game));
    assert.ok(tools.tools.slice(1).every((tool) => tool.description.includes("Offline schema")));
    const offlinePing = await client.callTool({ name: "ping", arguments: {} });
    assert.equal(offlinePing.isError, true);
    assert.equal(JSON.parse(textResult(offlinePing)).availability, "offline");
    const status = await client.callTool({
      name: "devbench.session",
      arguments: { action: "status" },
    });
    assert.equal(status.isError, undefined);
    assert.equal(JSON.parse(textResult(status)).available, true);
  }
});

test("cold offline bridge reconnects across new ports and instances", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const localAppData = join(root, "local");
  const { client, transport, notifications } = await startBridge(
    runtimeFile,
    localAppData,
  );
  t.after(() => client.close());

  const offline = await client.listTools();
  assert.deepEqual(offline.tools.map((tool) => tool.name), offlineToolNames());
  const offlineStatus = await client.callTool({
    name: "devbench.session",
    arguments: { action: "status" },
  });
  assert.equal(offlineStatus.isError, undefined);

  const first = await startMock({
    instanceId: instanceId(4101, "01DC000000000001"),
    pid: 4101,
    descriptors: [
      {
        name: "ping",
        description: "Live self-test.",
        inputSchema: {
          type: "object",
          properties: { echo: { type: "string" } },
          additionalProperties: false,
        },
        readOnly: true,
      },
    ],
  });
  t.after(() => first.close());
  await writeRuntime(runtimeFile, first);
  const firstList = await waitForTools(client, ["devbench.session", "ping"]);
  assert.deepEqual(firstList.tools[1].inputSchema, {
    type: "object",
    properties: { echo: { type: "string" } },
    additionalProperties: false,
  });
  const ping = await client.callTool({
    name: "ping",
    arguments: { echo: "hello" },
  });
  assert.equal(ping.isError, undefined);
  assert.equal(first.headers.at(-1).values[0], first.instanceId);
  assert.ok(first.headers.every((header) => header.values.length === 1));

  await first.close();
  await waitForTools(client, offlineToolNames());
  const second = await startMock({
    instanceId: instanceId(4102, "01DC000000000002"),
    pid: 4102,
    descriptors: [
      {
        name: "inspect",
        description: "Inspect state.",
        inputSchema: {
          type: "object",
          required: ["kind"],
          properties: { kind: { type: "string", enum: ["state"] } },
        },
        readOnly: true,
      },
    ],
  });
  t.after(() => second.close());
  await writeRuntime(runtimeFile, second);
  await waitForTools(client, ["devbench.session", "inspect"]);
  assert.ok(notifications.count >= 1);
  assert.equal(transport.pid === null, false);
});

function offlineToolNames(game = "fo4") {
  return ["devbench.session", ...coreTools(game).map((tool) => tool.name)];
}

test("explicit runtime files cannot attach to another game before HTTP dispatch", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-game-identity-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const mock = await startMock({
    instanceId: instanceId(4301, "01DC000000000009"),
    pid: 4301,
    exePath: "C:\\Games\\Skyrim Special Edition\\SkyrimSE.exe",
    runtime: "1.6.1170.0",
    descriptors: [],
  });
  t.after(() => mock.close());
  await writeRuntime(runtimeFile, mock);
  const wrongGame = new RemoteClient(createRuntimeTarget({
    game: "fo4", runtimeFile, localAppData: join(root, "local"),
  }));
  await assert.rejects(
    wrongGame.callTool("game", { action: "load", name: "fixture" }, { mutation: true }),
    /--game fo4 requires Fallout4.exe/,
  );
  assert.equal(mock.healthCalls, 0);
  assert.equal(mock.toolCalls, 0);

  const selected = new RemoteClient(createRuntimeTarget({
    game: "skyrimse", runtimeFile, localAppData: join(root, "local"),
  }));
  assert.equal((await selected.resolve()).identity.instanceId, mock.instanceId);
  await writeFile(runtimeFile, JSON.stringify({ ...mock, gameId: "fo4" }));
  await assert.rejects(readRuntimeIdentity(runtimeFile), /gameId inconsistent/);
  assert.equal(mock.toolCalls, 0);
});

test("game aliases isolate runtime and controller state and reject the Starfield placeholder", () => {
  for (const [alias, id, extender, executable] of [
    ["fallout4", "fo4", "F4SE", "Fallout4.exe"],
    ["skyrimse", "se", "SKSE", "SkyrimSE.exe"],
    ["skyrimvr", "vr", "SKSE", "SkyrimVR.exe"],
  ]) {
    const target = createRuntimeTarget({
      game: alias, install: "C:\\Games\\Test", localAppData: "C:\\Local",
    });
    assert.equal(target.game, id);
    assert.equal(target.profile.executable, executable);
    assert.deepEqual(target.runtimeFiles, [
      join("C:\\Games\\Test", "Data", extender, "Plugins", "devbench", "runtime.json"),
      join("C:\\Local", "devbench", id, "runtime.json"),
    ]);
    const config = new SessionConfigProvider({
      game: alias, explicit: false, localAppData: "C:\\Local",
    });
    assert.equal(config.configPath, join("C:\\Local", "devbench", id, "session.json"));
  }
  assert.throws(() => createRuntimeTarget({ game: "starfield" }), /unsupported --game starfield/);
});

test("malformed responses fail and mutation loss is never retried", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-errors-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const mock = await startMock({
    instanceId: instanceId(4201, "01DC000000000003"),
    pid: 4201,
    descriptors: [
      {
        name: "mutate",
        description: "Mutation.",
        inputSchema: { type: "object" },
        readOnly: false,
      },
      {
        name: "broken",
        description: "Broken response.",
        inputSchema: { type: "object" },
        readOnly: true,
      },
      {
        name: "denied",
        description: "Domain failure.",
        inputSchema: { type: "object" },
        readOnly: false,
      },
    ],
    handlers: {
      broken: (_args, response) => {
        response.writeHead(200, { "Content-Type": "application/json" });
        response.end("{not-json");
      },
      denied: (_args, response) => {
        json(response, { error: "permission disabled", code: 403 }, 403);
      },
      mutate: async (_args, response) => {
        mock.mutationCount++;
        mock.mutationSeen();
        await new Promise((resolve) => setTimeout(resolve, 100));
        response.writeHead(200, { "Content-Type": "application/json" });
        response.end(JSON.stringify({ accepted: true }));
      },
    },
  });
  t.after(() => mock.close());
  await writeRuntime(runtimeFile, mock);
  const { client } = await startBridge(runtimeFile, join(root, "local"));
  t.after(() => client.close());
  await waitForTools(client, [
    "devbench.session",
    "mutate",
    "broken",
    "denied",
  ]);

  const broken = await client.callTool({ name: "broken", arguments: {} });
  assert.equal(broken.isError, true);
  assert.match(textResult(broken), /malformed JSON/);

  const denied = await client.callTool({ name: "denied", arguments: {} });
  assert.equal(denied.isError, true);
  assert.match(textResult(denied), /permission disabled/);

  const mutationPromise = client.callTool({ name: "mutate", arguments: {} });
  await mock.waitForMutation();
  await writeFile(
    runtimeFile,
    JSON.stringify({
      port: mock.port,
      pid: 4202,
      instanceId: instanceId(4202, "01DC000000000004"),
      exePath: "C:\\Games\\Fallout 4\\Fallout4.exe",
      runtime: "1.11.240.0",
      dllPath: "C:\\Mods\\DevBench\\devbench.dll",
    }),
  );
  const mutation = await mutationPromise;
  assert.equal(mutation.isError, true);
  assert.match(textResult(mutation), /instance changed during the call/);
  assert.equal(mock.mutationCount, 1);
});

test("timed out mutation reports uncertainty without retry", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-timeout-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const mock = await startMock({
    instanceId: instanceId(4301, "01DC000000000005"),
    pid: 4301,
    descriptors: [],
    handlers: {
      mutate: async (_args, response) => {
        mock.mutationCount++;
        await new Promise((resolve) => setTimeout(resolve, 100));
        json(response, { accepted: true });
      },
    },
  });
  t.after(() => mock.close());
  await writeRuntime(runtimeFile, mock);
  const remote = new RemoteClient(
    createRuntimeTarget({
      game: "fo4",
      runtimeFile,
      localAppData: join(root, "local"),
    }),
    { discoveryTimeoutMs: 100, callTimeoutMs: 30 },
  );
  await assert.rejects(
    remote.callTool("mutate", {}, { mutation: true }),
    UncertainMutationError,
  );
  await new Promise((resolve) => setTimeout(resolve, 150));
  assert.equal(mock.mutationCount, 1);
});

test("stale runtime identity receives a pre-dispatch 409 and exposes no remote tools", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-stale-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const mock = await startMock({
    instanceId: instanceId(4401, "01DC000000000006"),
    pid: 4401,
    descriptors: [
      {
        name: "ping",
        description: "Should not dispatch.",
        inputSchema: { type: "object" },
        readOnly: true,
      },
    ],
  });
  t.after(() => mock.close());
  await writeFile(
    runtimeFile,
    JSON.stringify({
      port: mock.port,
      pid: 4401,
      instanceId: instanceId(4401, "01DC000000000007"),
      exePath: mock.exePath,
      runtime: mock.runtime,
      dllPath: mock.dllPath,
    }),
  );
  const { client } = await startBridge(runtimeFile, join(root, "local"));
  t.after(() => client.close());

  const tools = await client.listTools();
  assert.deepEqual(tools.tools.map((tool) => tool.name), offlineToolNames());
  assert.ok(mock.guardRejects >= 1);
  assert.equal(mock.healthCalls, 0);
  assert.equal(mock.toolCalls, 0);
});

test("an owned session never follows a replacement instance between calls", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-pinned-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const first = await startMock({
    instanceId: instanceId(4501, "01DC000000000011"), pid: 4501, descriptors: [],
  });
  const second = await startMock({
    instanceId: instanceId(4502, "01DC000000000012"), pid: 4502, descriptors: [],
  });
  t.after(() => first.close());
  t.after(() => second.close());
  await writeRuntime(runtimeFile, first);
  const remote = new RemoteClient(createRuntimeTarget({
    game: "fo4", runtimeFile, localAppData: join(root, "local"),
  }));
  remote.pin((await remote.resolve()).identity);
  await writeRuntime(runtimeFile, second);
  await assert.rejects(remote.callTool("mutate", {}, { mutation: true }), IdentityChangedError);
  assert.equal(second.toolCalls, 0);
  assert.equal(first.toolCalls, 0);
});

test("a truncated mutation response body is uncertain and is not retried", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-truncated-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  const mock = await startMock({
    instanceId: instanceId(4503, "01DC000000000013"), pid: 4503, descriptors: [],
    handlers: {
      mutate: (_args, response) => {
        mock.mutationCount++;
        response.writeHead(200, { "Content-Type": "application/json", "Content-Length": "1000" });
        response.write('{"started":true,');
        setTimeout(() => response.destroy(), 20);
      },
    },
  });
  t.after(() => mock.close());
  await writeRuntime(runtimeFile, mock);
  const remote = new RemoteClient(createRuntimeTarget({
    game: "fo4", runtimeFile, localAppData: join(root, "local"),
  }));
  await assert.rejects(remote.callTool("mutate", {}, { mutation: true }), UncertainMutationError);
  assert.equal(mock.mutationCount, 1);
});

test("old or malformed runtime files stay safely offline", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-bridge-runtime-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const runtimeFile = join(root, "runtime.json");
  await writeFile(
    runtimeFile,
    JSON.stringify({
      port: 12345,
      pid: 99,
      exePath: "C:\\Games\\Fallout 4\\Fallout4.exe",
    }),
  );
  const { client } = await startBridge(runtimeFile, join(root, "local"));
  t.after(() => client.close());
  const tools = await client.listTools();
  assert.deepEqual(tools.tools.map((tool) => tool.name), offlineToolNames());
  const call = await client.callTool({ name: "ping", arguments: {} });
  assert.equal(call.isError, true);
  assert.match(textResult(call), /upgrade the DevBench plugin/);

  await writeFile(
    runtimeFile,
    JSON.stringify({
      port: 12345,
      pid: 99,
      instanceId: "99-01dc000000000001",
      exePath: "C:\\Games\\Fallout 4\\Fallout4.exe",
      runtime: "1.11.240.0",
      dllPath: "C:\\Mods\\DevBench\\devbench.dll",
    }),
  );
  await assert.rejects(
    readRuntimeIdentity(runtimeFile),
    /expected <pid>-<16 uppercase hex creation FILETIME ticks>/,
  );
});

async function startBridge(runtimeFile, localAppData, game = "fo4") {
  const notifications = { count: 0 };
  const args = [bridgeEntry, "--game", game];
  if (runtimeFile) args.push("--runtime-file", runtimeFile);
  const transport = new StdioClientTransport({
    command: process.execPath,
    args,
    cwd: new URL("..", import.meta.url).pathname.slice(1),
    env: {
      PATH: process.env.PATH ?? "",
      LOCALAPPDATA: localAppData,
      SystemRoot: process.env.SystemRoot ?? "C:\\Windows",
    },
    stderr: "pipe",
  });
  const client = new Client({ name: "bridge-test", version: "0.0.0" });
  client.setNotificationHandler(ToolListChangedNotificationSchema, () => {
    notifications.count++;
  });
  await client.connect(transport);
  return { client, transport, notifications };
}

async function waitForTools(client, expected) {
  const deadline = Date.now() + 5_000;
  let last;
  while (Date.now() < deadline) {
    last = await client.listTools();
    if (
      JSON.stringify(last.tools.map((tool) => tool.name)) ===
      JSON.stringify(expected)
    ) {
      return last;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  assert.fail(`timed out waiting for tools ${expected.join(", ")}; last=${JSON.stringify(last)}`);
}

async function startMock(options) {
  const headers = [];
  let mutationResolver;
  const mutationPromise = new Promise((resolve) => {
    mutationResolver = resolve;
  });
  const state = {
    instanceId: options.instanceId,
    pid: options.pid,
    port: 0,
    runtime: options.runtime ?? "1.11.240.0",
    exePath: options.exePath ?? "C:\\Games\\Fallout 4\\Fallout4.exe",
    dllPath: "C:\\Mods\\DevBench\\devbench.dll",
    headers,
    healthCalls: 0,
    toolCalls: 0,
    guardRejects: 0,
    mutationCount: 0,
    mutationSeen: () => mutationResolver(),
    waitForMutation: () => mutationPromise,
  };
  const server = createServer(async (request, response) => {
    const header = instanceHeaders(request.rawHeaders);
    headers.push(header);
    if (
      header.values.length > 1 ||
      (header.values.length === 1 &&
        (header.values[0] === "" || header.values[0] !== state.instanceId))
    ) {
      state.guardRejects++;
      response.writeHead(409, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ error: "instance mismatch" }));
      return;
    }
    if (request.method === "GET" && request.url === "/api/health") {
      state.healthCalls++;
      json(response, {
        ok: true,
        exe: state.exePath.split("\\").at(-1),
        port: state.port,
        pid: state.pid,
        instanceId: state.instanceId,
        exePath: state.exePath,
        runtime: state.runtime,
        dllPath: state.dllPath,
      });
      return;
    }
    if (request.method === "GET" && request.url === "/api/tools") {
      json(response, {
        tools: options.descriptors,
        mcp_bridge: {
          available: true,
          controllerAvailable: true,
          exePath:
            "C:\\Mods\\DevBench\\F4SE\\Plugins\\devbench\\devbench-bridge.exe",
          args: ["--game", "fo4"],
        },
      });
      return;
    }
    const match = request.url?.match(/^\/api\/tool\/([^/?]+)$/);
    if (request.method === "POST" && match) {
      state.toolCalls++;
      const args = await readBody(request);
      const name = decodeURIComponent(match[1]);
      const handler = options.handlers?.[name];
      if (handler) {
        await handler(args, response);
      } else {
        json(response, { ok: true, name, args });
      }
      return;
    }
    response.writeHead(404, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: "not found" }));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  state.port = server.address().port;
  state.close = () =>
    new Promise((resolve) => {
      if (!server.listening) resolve();
      else server.close(resolve);
    });
  return state;
}

async function writeRuntime(path, mock) {
  await writeFile(
    path,
    JSON.stringify({
      port: mock.port,
      pid: mock.pid,
      instanceId: mock.instanceId,
      exePath: mock.exePath,
      runtime: mock.runtime,
      dllPath: mock.dllPath,
    }),
  );
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

function textResult(result) {
  return result.content
    .filter((content) => content.type === "text")
    .map((content) => content.text)
    .join("\n");
}

function instanceId(pid, creationTicks) {
  return `${pid}-${creationTicks}`;
}

function instanceHeaders(rawHeaders) {
  const values = [];
  for (let index = 0; index < rawHeaders.length; index += 2) {
    if (rawHeaders[index].toLowerCase() === "x-devbench-instance") {
      values.push(rawHeaders[index + 1]);
    }
  }
  return { values };
}
