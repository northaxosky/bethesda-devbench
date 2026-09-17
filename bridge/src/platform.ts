import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync, statSync } from "node:fs";
import { createInterface } from "node:readline";
import { delimiter, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
  errorMessage,
  isObject,
  requireInteger,
  requireObject,
  requireString,
} from "./json.js";

export interface OwnedProcessStatus {
  owned: boolean;
  running: boolean;
  pid?: number;
  creationTime?: string;
  exe?: string;
  mo2Pid?: number;
}

export interface StartRequest {
  mo2Exe: string;
  profilesDir: string;
  profile: string;
  executable: string;
  gameExe: string;
  loaderExe: string;
  timeoutMs: number;
}

export interface StartedProcess extends OwnedProcessStatus {
  owned: true;
  running: true;
  pid: number;
  creationTime: string;
  exe: string;
  mo2Pid: number;
}

export interface InspectedProcess {
  pid: number;
  creationTime: string;
  exe: string;
  running: boolean;
  runtimeVersion: string;
  plugin: { path: string; sha256: string } | null;
}

export interface CloseResult {
  exited: boolean;
  reason?: string;
  owned?: boolean;
}

export interface NativePlatform {
  start(request: StartRequest): Promise<StartedProcess>;
  status(): Promise<OwnedProcessStatus>;
  inspect(request: {
    pid: number;
    gameExe: string;
    pluginName?: "devbench.dll";
  }): Promise<InspectedProcess>;
  close(timeoutMs: number): Promise<CloseResult>;
  terminate(): Promise<CloseResult>;
  release(): Promise<void>;
  shutdown(): Promise<void>;
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timeout: NodeJS.Timeout;
}

export class PlatformCallError extends Error {
  constructor(
    readonly code: string,
    message: string,
  ) {
    super(message);
  }
}

export class WindowsSessionPlatform implements NativePlatform {
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly pending = new Map<number, PendingCall>();
  private nextId = 1;
  private closed = false;

  constructor(
    scriptPath = resolvePlatformScript(),
    private readonly defaultTimeoutMs = 30_000,
    pwshPath = resolvePwshExecutable(),
  ) {
    this.child = spawn(
      pwshPath,
      [
        "-NoProfile",
        "-NonInteractive",
        "-File",
        scriptPath,
      ],
      { stdio: ["pipe", "pipe", "pipe"], windowsHide: true },
    );
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      this.handleLine(line);
    });
    this.child.stderr.on("data", (chunk: Buffer) => {
      process.stderr.write(chunk);
    });
    this.child.once("error", (error) => this.failAll(error));
    this.child.once("exit", (code, signal) => {
      this.failAll(
        new Error(
          `Windows session helper exited (code=${String(code)}, signal=${String(signal)})`,
        ),
      );
    });
  }

  async start(request: StartRequest): Promise<StartedProcess> {
    return parseStarted(
      await this.call(
        "start",
        { ...request },
        Math.max(request.timeoutMs, this.defaultTimeoutMs) + 15_000,
      ),
    );
  }

  async status(): Promise<OwnedProcessStatus> {
    return parseStatus(await this.call("status", {}, this.defaultTimeoutMs));
  }

  async inspect(request: {
    pid: number;
    gameExe: string;
    pluginName?: "devbench.dll";
  }): Promise<InspectedProcess> {
    return parseInspect(
      await this.call("inspect", request, this.defaultTimeoutMs),
    );
  }

  async close(timeoutMs: number): Promise<CloseResult> {
    return parseClose(
      await this.call("close", { timeoutMs }, timeoutMs + 5_000),
    );
  }

  async terminate(): Promise<CloseResult> {
    return parseClose(await this.call("terminate", {}, this.defaultTimeoutMs));
  }

  async release(): Promise<void> {
    if (this.closed) return;
    await this.call("release", {}, this.defaultTimeoutMs);
  }

  async shutdown(): Promise<void> {
    if (this.closed) return;
    try {
      await this.release();
    } finally {
      this.closed = true;
      this.child.stdin.end();
    }
  }

  private call(
    op: string,
    fields: Record<string, unknown>,
    timeoutMs: number,
  ): Promise<unknown> {
    if (this.closed) {
      return Promise.reject(new Error("Windows session helper is closed"));
    }
    const id = this.nextId++;
    return new Promise((resolvePromise, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(id);
        reject(
          new Error(
            `Windows session helper ${op} call timed out after ${String(timeoutMs)} ms`,
          ),
        );
      }, timeoutMs);
      this.pending.set(id, { resolve: resolvePromise, reject, timeout });
      this.child.stdin.write(`${JSON.stringify({ id, op, ...fields })}\n`, (error) => {
        if (!error) return;
        clearTimeout(timeout);
        this.pending.delete(id);
        reject(error);
      });
    });
  }

  private handleLine(line: string): void {
    let value: unknown;
    try {
      value = JSON.parse(line) as unknown;
    } catch (error) {
      this.failAll(
        new Error(`Windows session helper emitted invalid JSON: ${errorMessage(error)}`),
      );
      return;
    }
    if (!isObject(value) || !Number.isSafeInteger(value.id)) {
      this.failAll(new Error("Windows session helper emitted an invalid envelope"));
      return;
    }
    const id = value.id as number;
    const pending = this.pending.get(id);
    if (!pending) return;
    clearTimeout(pending.timeout);
    this.pending.delete(id);
    if (value.ok === true) {
      pending.resolve(value.result);
      return;
    }
    if (value.ok === false && isObject(value.error)) {
      pending.reject(
        new PlatformCallError(
          requireString(value.error, "code", "platform error"),
          requireString(value.error, "message", "platform error"),
        ),
      );
      return;
    }
    pending.reject(new Error("Windows session helper returned an invalid response"));
  }

  private failAll(error: Error): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timeout);
      pending.reject(error);
    }
    this.pending.clear();
  }
}

export function resolvePlatformScript(): string {
  const moduleDir = dirname(fileURLToPath(import.meta.url));
  if (process.argv[1]?.includes("~BUN")) {
    return resolve(dirname(process.execPath), "platform", "windows-session.ps1");
  }
  return resolve(moduleDir, "platform", "windows-session.ps1");
}

export function resolvePwshExecutable(
  environment: NodeJS.ProcessEnv = process.env,
): string {
  const candidates: string[] = [];
  const pathValue = environment.Path ?? environment.PATH;
  if (pathValue) {
    for (const entry of pathValue.split(delimiter)) {
      const directory = entry.trim().replace(/^"(.*)"$/, "$1");
      if (directory) candidates.push(resolve(directory, "pwsh.exe"));
    }
  }
  for (const root of [environment.ProgramW6432, environment.ProgramFiles]) {
    if (root) candidates.push(resolve(root, "PowerShell", "7", "pwsh.exe"));
  }
  const found = [...new Set(candidates.map((candidate) => candidate.toLowerCase()))]
    .map((lower) => candidates.find((candidate) => candidate.toLowerCase() === lower))
    .find((candidate): candidate is string => {
      if (!candidate || !existsSync(candidate)) return false;
      try {
        return statSync(candidate).isFile();
      } catch {
        return false;
      }
    });
  if (found) return found;
  throw new Error(
    "PowerShell 7 pwsh.exe is required for the Windows session helper; Windows PowerShell 5.1 is not supported and execution-policy bypass is never used",
  );
}

function parseStatus(value: unknown): OwnedProcessStatus {
  const object = requireObject(value, "platform status");
  if (typeof object.owned !== "boolean" || typeof object.running !== "boolean") {
    throw new Error("platform status must contain owned/running booleans");
  }
  return {
    owned: object.owned,
    running: object.running,
    ...(Number.isSafeInteger(object.pid) ? { pid: object.pid as number } : {}),
    ...(typeof object.creationTime === "string"
      ? { creationTime: object.creationTime }
      : {}),
    ...(typeof object.exe === "string" ? { exe: object.exe } : {}),
    ...(Number.isSafeInteger(object.mo2Pid)
      ? { mo2Pid: object.mo2Pid as number }
      : {}),
  };
}

function parseStarted(value: unknown): StartedProcess {
  const status = parseStatus(value);
  const object = requireObject(value, "platform start");
  if (
    status.owned !== true ||
    status.running !== true ||
    status.pid === undefined ||
    status.creationTime === undefined ||
    status.exe === undefined
  ) {
    throw new Error("platform start did not return a retained owned process");
  }
  return {
    ...status,
    owned: true,
    running: true,
    pid: status.pid,
    creationTime: status.creationTime,
    exe: status.exe,
    mo2Pid: requireInteger(object, "mo2Pid", "platform start"),
  };
}

function parseInspect(value: unknown): InspectedProcess {
  const object = requireObject(value, "platform inspect");
  const plugin = object.plugin;
  let parsedPlugin: InspectedProcess["plugin"] = null;
  if (plugin !== null) {
    const pluginObject = requireObject(plugin, "platform inspect.plugin");
    parsedPlugin = {
      path: requireString(pluginObject, "path", "platform inspect.plugin"),
      sha256: requireString(pluginObject, "sha256", "platform inspect.plugin"),
    };
  }
  if (typeof object.running !== "boolean") {
    throw new Error("platform inspect.running must be boolean");
  }
  return {
    pid: requireInteger(object, "pid", "platform inspect"),
    creationTime: requireString(object, "creationTime", "platform inspect"),
    exe: requireString(object, "exe", "platform inspect"),
    running: object.running,
    runtimeVersion: requireString(
      object,
      "runtimeVersion",
      "platform inspect",
    ),
    plugin: parsedPlugin,
  };
}

function parseClose(value: unknown): CloseResult {
  const object = requireObject(value, "platform close");
  if (typeof object.exited !== "boolean") {
    throw new Error("platform close.exited must be boolean");
  }
  return {
    exited: object.exited,
    ...(typeof object.reason === "string" ? { reason: object.reason } : {}),
    ...(typeof object.owned === "boolean" ? { owned: object.owned } : {}),
  };
}
