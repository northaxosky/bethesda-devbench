#!/usr/bin/env node
import { dirname, resolve } from "node:path";

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

import { SessionConfigProvider } from "./config.js";
import { GAME_PROFILES } from "./discovery/manifest.js";
import {
  asJsonValue,
  errorMessage,
  isObject,
  type JsonObject,
} from "./json.js";
import { WindowsSessionPlatform } from "./platform.js";
import { RemoteProxy } from "./proxy.js";
import { RemoteClient } from "./remote.js";
import {
  SessionController,
  SESSION_TOOL,
} from "./session.js";
import { createRuntimeTarget } from "./runtime.js";
import { printSetupSnippet } from "./setup.js";

interface CliArguments {
  command: "serve" | "setup" | "help";
  game?: string;
  install?: string;
  runtimeFile?: string;
  config?: string;
}

export async function main(argv = process.argv.slice(2)): Promise<void> {
  const args = parseArgs(argv);
  if (args.command === "help") {
    printHelp();
    return;
  }
  const target = createRuntimeTarget({
    ...(args.game ? { game: args.game } : {}),
    ...(args.install ? { install: args.install } : {}),
    ...(args.runtimeFile ? { runtimeFile: args.runtimeFile } : {}),
  });
  if (args.command === "setup") {
    const invocation = bridgeInvocation();
    printSetupSnippet(invocation.command, invocation.scriptArgs, {
      game: target.game,
      ...(args.install ? { install: args.install } : {}),
      ...(args.runtimeFile ? { runtimeFile: args.runtimeFile } : {}),
      ...(args.config ? { config: args.config } : {}),
    });
    return;
  }

  const config = new SessionConfigProvider({
    game: target.game,
    ...(args.config ? { configPath: args.config } : {}),
    explicit: args.config !== undefined,
  });
  await config.validateExplicit();

  const remote = new RemoteClient(target);
  const server = new Server(
    { name: "devbench-bridge", version: "0.1.0" },
    { capabilities: { tools: { listChanged: true } } },
  );
  const proxy = new RemoteProxy(remote, {
    onListChanged: async () => {
      await server.sendToolListChanged();
    },
  });
  const session = new SessionController({
    config,
    platformFactory: () => new WindowsSessionPlatform(),
    remoteFactory: (sessionConfig) => {
      if (!sessionConfig.runtimeFile) return remote;
      return new RemoteClient(
        createRuntimeTarget({
          game: target.game,
          install: dirname(sessionConfig.gameExe),
          runtimeFile: sessionConfig.runtimeFile,
        }),
      );
    },
  });

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: [SESSION_TOOL, ...(await proxy.listRemoteTools())],
  }));
  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const argsObject = normalizeArguments(request.params.arguments);
    const result =
      request.params.name === SESSION_TOOL.name
        ? await session.handle(argsObject)
        : await proxy.callTool(request.params.name, argsObject);
    return {
      content: [
        {
          type: "text",
          text: JSON.stringify(asJsonValue(result.value)),
        },
      ],
      ...(result.isError ? { isError: true } : {}),
    };
  });

  const transport = new StdioServerTransport();
  let closing = false;
  const close = async (): Promise<void> => {
    if (closing) return;
    closing = true;
    proxy.close();
    await session.shutdown();
    await server.close().catch(() => undefined);
  };
  process.once("SIGINT", () => void close());
  process.once("SIGTERM", () => void close());
  process.stdin.once("end", () => void close());
  await server.connect(transport);
}

function normalizeArguments(value: unknown): JsonObject {
  if (value === undefined) return {};
  if (!isObject(value)) throw new Error("tool arguments must be a JSON object");
  return asJsonValue(value) as JsonObject;
}

function parseArgs(argv: string[]): CliArguments {
  let command: CliArguments["command"] = "serve";
  let game: string | undefined;
  let install: string | undefined;
  let runtimeFile: string | undefined;
  let config: string | undefined;
  for (let index = 0; index < argv.length; index++) {
    const argument = argv[index];
    if (argument === "setup") {
      if (command !== "serve" || index !== 0) {
        throw new Error("'setup' must be the first and only positional command");
      }
      command = "setup";
      continue;
    }
    if (argument === "--help" || argument === "-h") {
      command = "help";
      continue;
    }
    switch (argument) {
      case "--game":
        game = requiredValue(argv, ++index, "--game");
        break;
      case "--install":
        install = requiredValue(argv, ++index, "--install");
        break;
      case "--runtime-file":
        runtimeFile = requiredValue(argv, ++index, "--runtime-file");
        break;
      case "--config":
        config = requiredValue(argv, ++index, "--config");
        break;
      default:
        throw new Error(`unknown argument '${String(argument)}'`);
    }
  }
  return {
    command,
    ...(game ? { game } : {}),
    ...(install ? { install: resolve(install) } : {}),
    ...(runtimeFile ? { runtimeFile: resolve(runtimeFile) } : {}),
    ...(config ? { config: resolve(config) } : {}),
  };
}

function requiredValue(
  argv: string[],
  index: number,
  option: string,
): string {
  const value = argv[index];
  if (!value || value.startsWith("--")) {
    throw new Error(`${option} requires a value`);
  }
  return value;
}

function bridgeInvocation(): { command: string; scriptArgs: string[] } {
  if (isCompiledExecutable()) {
    return { command: process.execPath, scriptArgs: [] };
  }
  const script = process.argv[1];
  if (!script) throw new Error("cannot determine the bridge entry script");
  return { command: process.execPath, scriptArgs: [resolve(script)] };
}

function isCompiledExecutable(): boolean {
  return process.argv[1]?.includes("~BUN") ?? false;
}

function printHelp(): void {
  console.log(`devbench-bridge [setup] --game <id> [options]

Restart-surviving stdio MCP bridge for DevBench.

Options:
  --game <id>           Supported games: ${GAME_PROFILES.map((profile) => profile.id).join(", ")}.
  --install <folder>     Constrain discovery to the selected game's executable and runtime.json.
  --runtime-file <path>  Add an explicit runtime.json candidate.
  --config <path>        Session controller machine config. Explicit invalid files fail startup.
  --help, -h             Print this reference and exit.

The default session config is %LOCALAPPDATA%\\devbench\\<game-id>\\session.json.
It is never created or edited automatically. "setup" prints only an MCP JSON snippet.`);
}

main().catch((error: unknown) => {
  console.error(`devbench-bridge: ${errorMessage(error)}`);
  process.exitCode = 1;
});
