import { access, readFile, stat } from "node:fs/promises";
import { constants } from "node:fs";
import { join, resolve, win32 } from "node:path";

import {
  errorMessage,
  isObject,
  optionalString,
  requireObject,
  requireString,
} from "./json.js";

export interface SessionLogSpec {
  path: string;
  pattern?: string;
  maxBytes: number;
}

export interface SessionConfig {
  sourcePath: string;
  mo2Exe: string;
  profilesDir: string;
  profile: string;
  executable: string;
  gameExe: string;
  loaderExe: string;
  devbenchDll: string;
  runtimeFile?: string;
  expectedRuntime?: string;
  expectedDllSha256?: string;
  resultsDir: string;
  logs: SessionLogSpec[];
}

export interface SessionConfigProviderOptions {
  configPath?: string;
  explicit: boolean;
  localAppData?: string;
}

export class SessionConfigProvider {
  readonly configPath: string;
  readonly explicit: boolean;
  private cached?: SessionConfig;

  constructor(options: SessionConfigProviderOptions) {
    const localAppData = options.localAppData ?? process.env.LOCALAPPDATA;
    this.configPath = options.configPath
      ? resolve(options.configPath)
      : localAppData
        ? resolve(localAppData, "devbench", "fo4", "session.json")
        : resolve("session.json");
    this.explicit = options.explicit;
  }

  async validateExplicit(): Promise<void> {
    if (this.explicit) await this.load();
  }

  async load(): Promise<SessionConfig> {
    if (this.cached) return this.cached;
    let raw: string;
    try {
      raw = await readFile(this.configPath, "utf8");
    } catch (error) {
      throw new Error(
        `session config is unavailable at ${this.configPath}: ${errorMessage(error)}`,
        { cause: error },
      );
    }
    let value: unknown;
    try {
      value = JSON.parse(raw) as unknown;
    } catch (error) {
      throw new Error(
        `invalid session config JSON at ${this.configPath}: ${errorMessage(error)}`,
        { cause: error },
      );
    }
    this.cached = parseSessionConfig(value, this.configPath);
    return this.cached;
  }

  describe(): { path: string; loaded: boolean; explicit: boolean } {
    return {
      path: this.configPath,
      loaded: this.cached !== undefined,
      explicit: this.explicit,
    };
  }
}

export function parseSessionConfig(
  value: unknown,
  sourcePath: string,
): SessionConfig {
  const object = requireObject(value, "session config");
  const config: SessionConfig = {
    sourcePath,
    mo2Exe: absoluteWindowsPath(object, "mo2Exe"),
    profilesDir: absoluteWindowsPath(object, "profilesDir"),
    profile: requireString(object, "profile", "session config"),
    executable: requireString(object, "executable", "session config"),
    gameExe: absoluteWindowsPath(object, "gameExe"),
    loaderExe: absoluteWindowsPath(object, "loaderExe"),
    devbenchDll: absoluteWindowsPath(object, "devbenchDll"),
    resultsDir: absoluteWindowsPath(object, "resultsDir"),
    logs: parseLogs(object.logs),
  };
  const runtimeFile = optionalString(object, "runtimeFile", "session config");
  const expectedRuntime = optionalString(
    object,
    "expectedRuntime",
    "session config",
  );
  const expectedDllSha256 = optionalString(
    object,
    "expectedDllSha256",
    "session config",
  );
  if (runtimeFile) config.runtimeFile = validateAbsolute(runtimeFile, "runtimeFile");
  if (expectedRuntime) config.expectedRuntime = expectedRuntime;
  if (expectedDllSha256) {
    if (!/^[a-f0-9]{64}$/i.test(expectedDllSha256)) {
      throw new Error(
        "session config.expectedDllSha256 must contain 64 hexadecimal characters",
      );
    }
    config.expectedDllSha256 = expectedDllSha256.toLowerCase();
  }
  return config;
}

export async function validateLaunchInputs(config: SessionConfig): Promise<void> {
  await requireFile(config.mo2Exe, "Mod Organizer executable");
  await requireFile(config.gameExe, "Fallout4 executable");
  await requireFile(config.devbenchDll, "deployed DevBench DLL");
  const profileDir = join(config.profilesDir, config.profile);
  let profileStat;
  try {
    profileStat = await stat(profileDir);
  } catch (error) {
    throw new Error(
      `MO2 profile directory is unavailable at ${profileDir}: ${errorMessage(error)}`,
      { cause: error },
    );
  }
  if (!profileStat.isDirectory()) {
    throw new Error(`MO2 profile path is not a directory: ${profileDir}`);
  }
}

function parseLogs(value: unknown): SessionLogSpec[] {
  if (!Array.isArray(value)) {
    throw new Error("session config.logs must be an array");
  }
  return value.map((entry, index) => {
    if (typeof entry === "string") {
      return { path: validateAbsolute(entry, `logs[${String(index)}]`), maxBytes: 1_048_576 };
    }
    if (!isObject(entry)) {
      throw new Error(`session config.logs[${String(index)}] must be a path or object`);
    }
    const path = validateAbsolute(
      requireString(entry, "path", `session config.logs[${String(index)}]`),
      `logs[${String(index)}].path`,
    );
    const pattern = optionalString(
      entry,
      "pattern",
      `session config.logs[${String(index)}]`,
    );
    const maxBytesValue = entry.maxBytes;
    const maxBytes =
      maxBytesValue === undefined ? 1_048_576 : Number(maxBytesValue);
    if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || maxBytes > 16_777_216) {
      throw new Error(
        `session config.logs[${String(index)}].maxBytes must be between 1 and 16777216`,
      );
    }
    return {
      path,
      maxBytes,
      ...(pattern ? { pattern: validatePattern(pattern, index) } : {}),
    };
  });
}

function absoluteWindowsPath(
  object: Record<string, unknown>,
  key: string,
): string {
  return validateAbsolute(requireString(object, key, "session config"), key);
}

function validateAbsolute(value: string, label: string): string {
  if (!win32.isAbsolute(value)) {
    throw new Error(`session config.${label} must be an absolute Windows path`);
  }
  return win32.normalize(value);
}

function validatePattern(pattern: string, index: number): string {
  if (
    pattern.includes("/") ||
    pattern.includes("\\") ||
    pattern === "." ||
    pattern === ".."
  ) {
    throw new Error(
      `session config.logs[${String(index)}].pattern must be a filename wildcard, not a path`,
    );
  }
  return pattern;
}

async function requireFile(path: string, label: string): Promise<void> {
  try {
    await access(path, constants.R_OK);
    const value = await stat(path);
    if (!value.isFile()) throw new Error("not a file");
  } catch (error) {
    throw new Error(`${label} is unavailable at ${path}: ${errorMessage(error)}`, {
      cause: error,
    });
  }
}
