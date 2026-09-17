import { readFile } from "node:fs/promises";
import { join, resolve, win32 } from "node:path";

import {
  errorMessage,
  requireInteger,
  requireObject,
  requireString,
} from "./json.js";

export interface RuntimeIdentity {
  runtimeFile: string;
  port: number;
  pid: number;
  instanceId: string;
  exePath: string;
  runtime: string;
  dllPath: string;
}

export interface RuntimeTarget {
  game: "fo4";
  label: string;
  runtimeFiles: string[];
  install?: string;
}

export interface RuntimeTargetOptions {
  game?: string;
  install?: string;
  runtimeFile?: string;
  localAppData?: string;
}

export interface CandidateFailure {
  runtimeFile: string;
  reason: string;
}

export function createRuntimeTarget(
  options: RuntimeTargetOptions,
): RuntimeTarget {
  if (options.game !== undefined && options.game !== "fo4") {
    throw new Error(
      `unsupported --game ${options.game}; this bridge supports only --game fo4`,
    );
  }
  if (
    options.game === undefined &&
    options.install === undefined &&
    options.runtimeFile === undefined
  ) {
    throw new Error(
      "devbench-bridge requires --game fo4, --install <Fallout 4 folder>, or --runtime-file <path>",
    );
  }

  const files: string[] = [];
  if (options.runtimeFile) files.push(resolve(options.runtimeFile));
  if (options.install) {
    files.push(
      resolve(
        options.install,
        "Data",
        "F4SE",
        "Plugins",
        "devbench",
        "runtime.json",
      ),
    );
  }
  const localAppData = options.localAppData ?? process.env.LOCALAPPDATA;
  if (localAppData) {
    files.push(resolve(localAppData, "devbench", "fo4", "runtime.json"));
  }

  return {
    game: "fo4",
    label: options.install ?? "FO4",
    runtimeFiles: [...new Set(files.map((file) => file.toLowerCase()))].map(
      (lower) => files.find((file) => file.toLowerCase() === lower) as string,
    ),
    ...(options.install ? { install: resolve(options.install) } : {}),
  };
}

export async function readRuntimeIdentity(
  runtimeFile: string,
): Promise<RuntimeIdentity> {
  let raw: string;
  try {
    raw = await readFile(runtimeFile, "utf8");
  } catch (error) {
    throw new Error(`cannot read ${runtimeFile}: ${errorMessage(error)}`, {
      cause: error,
    });
  }

  let value: unknown;
  try {
    value = JSON.parse(raw) as unknown;
  } catch (error) {
    throw new Error(`invalid JSON in ${runtimeFile}: ${errorMessage(error)}`, {
      cause: error,
    });
  }
  const object = requireObject(value, `runtime.json at ${runtimeFile}`);
  const port = requireInteger(object, "port", "runtime.json");
  const pid = requireInteger(object, "pid", "runtime.json");
  if (port < 1 || port > 65535) {
    throw new Error(`runtime.json at ${runtimeFile} has invalid port ${port}`);
  }
  if (pid < 1) {
    throw new Error(`runtime.json at ${runtimeFile} has invalid pid ${pid}`);
  }

  let instanceId: string;
  try {
    instanceId = requireString(object, "instanceId", "runtime.json");
  } catch {
    throw new Error(
      `runtime.json at ${runtimeFile} has no instanceId; upgrade the DevBench plugin before using the external bridge`,
    );
  }
  const instanceMatch = /^([1-9]\d*)-([0-9A-F]{16})$/.exec(instanceId);
  if (!instanceMatch || Number(instanceMatch[1]) !== pid) {
    throw new Error(
      `runtime.json at ${runtimeFile} has invalid instanceId '${instanceId}'; expected <pid>-<16 uppercase hex creation FILETIME ticks>`,
    );
  }

  const exePath = requireString(object, "exePath", "runtime.json");
  if (win32.basename(exePath).toLowerCase() !== "fallout4.exe") {
    throw new Error(
      `runtime.json at ${runtimeFile} identifies unsupported executable ${exePath}; expected Fallout4.exe`,
    );
  }
  const dllPath = requireString(object, "dllPath", "runtime.json");
  if (win32.basename(dllPath).toLowerCase() !== "devbench.dll") {
    throw new Error(
      `runtime.json at ${runtimeFile} identifies unsupported plugin ${dllPath}; expected devbench.dll`,
    );
  }

  return {
    runtimeFile,
    port,
    pid,
    instanceId,
    exePath,
    runtime: requireString(object, "runtime", "runtime.json"),
    dllPath,
  };
}

export function validateInstallIdentity(
  target: RuntimeTarget,
  identity: RuntimeIdentity,
): void {
  if (!target.install) return;
  const expected = join(target.install, "Fallout4.exe");
  if (!sameWindowsPath(identity.exePath, expected)) {
    throw new Error(
      `runtime identity exePath ${identity.exePath} is outside the selected install; expected ${expected}`,
    );
  }
}

export function sameWindowsPath(left: string, right: string): boolean {
  return normalizeWindowsPath(left) === normalizeWindowsPath(right);
}

function normalizeWindowsPath(value: string): string {
  return win32.normalize(value).replace(/[\\/]+$/, "").toLowerCase();
}

export function sameIdentity(
  left: RuntimeIdentity,
  right: RuntimeIdentity,
): boolean {
  return (
    left.pid === right.pid &&
    left.port === right.port &&
    left.instanceId === right.instanceId &&
    sameWindowsPath(left.exePath, right.exePath) &&
    left.runtime === right.runtime &&
    sameWindowsPath(left.dllPath, right.dllPath)
  );
}
