import {
  asJsonValue,
  errorMessage,
  isObject,
  type JsonObject,
  type JsonValue,
} from "./json.js";
import {
  readRuntimeIdentity,
  sameIdentity,
  sameWindowsPath,
  validateTargetIdentity,
  type CandidateFailure,
  type RuntimeIdentity,
  type RuntimeTarget,
} from "./runtime.js";

export interface RemoteToolDescriptor {
  name: string;
  description: string;
  inputSchema: JsonObject;
  readOnly: boolean;
}

export interface ResolvedRemote {
  identity: RuntimeIdentity;
  health: JsonObject;
}

export interface RemoteEvents {
  headSeq: number;
  events: JsonValue[];
}

export class GameUnavailableError extends Error {
  constructor(
    message: string,
    readonly failures: CandidateFailure[] = [],
  ) {
    super(message);
  }
}

export class RemoteDomainError extends Error {
  constructor(
    readonly status: number,
    readonly body: JsonValue,
  ) {
    super(`DevBench returned HTTP ${status}`);
  }
}

export class UncertainMutationError extends Error {
  constructor(message: string) {
    super(message);
  }
}

export class IdentityChangedError extends Error {}

export interface RemoteClientOptions {
  discoveryTimeoutMs?: number;
  callTimeoutMs?: number;
  fetchImplementation?: typeof fetch;
}

interface FetchJsonOptions {
  method?: "GET" | "POST";
  body?: JsonObject;
  timeoutMs: number;
  mutation: boolean;
}

export class RemoteClient {
  private pinnedIdentity?: RuntimeIdentity;
  private readonly discoveryTimeoutMs: number;
  private readonly callTimeoutMs: number;
  private readonly fetchImplementation: typeof fetch;

  constructor(
    readonly target: RuntimeTarget,
    options: RemoteClientOptions = {},
  ) {
    this.discoveryTimeoutMs = options.discoveryTimeoutMs ?? 2_000;
    this.callTimeoutMs = options.callTimeoutMs ?? 30_000;
    this.fetchImplementation = options.fetchImplementation ?? fetch;
  }

  pin(identity: RuntimeIdentity): void {
    if (this.pinnedIdentity && !sameIdentity(this.pinnedIdentity, identity)) {
      throw new IdentityChangedError("A controlled session cannot change its bound game instance");
    }
    this.pinnedIdentity = { ...identity };
  }

  async resolve(): Promise<ResolvedRemote> {
    const failures: CandidateFailure[] = [];
    for (const runtimeFile of this.target.runtimeFiles) {
      try {
        const identity = await readRuntimeIdentity(runtimeFile);
        if (this.pinnedIdentity && !sameIdentity(this.pinnedIdentity, identity)) {
          throw new IdentityChangedError(
            `Owned session instance changed from ${this.pinnedIdentity.instanceId} to ${identity.instanceId}; no command was dispatched`,
          );
        }
        validateTargetIdentity(this.target, identity);
        const health = await this.fetchJson(identity, "/api/health", {
          timeoutMs: this.discoveryTimeoutMs,
          mutation: false,
        });
        const object = requireHealth(health);
        verifyHealthIdentity(identity, object);
        await this.verifyRuntimeFile(identity);
        return { identity, health: asJsonValue(object) as JsonObject };
      } catch (error) {
        if (error instanceof IdentityChangedError && this.pinnedIdentity) throw error;
        failures.push({ runtimeFile, reason: errorMessage(error) });
      }
    }
    const summary =
      failures.length === 0
        ? "no runtime.json candidates are configured"
        : failures.map((failure) => failure.reason).join("; ");
    throw new GameUnavailableError(
      `${this.target.profile.displayName} DevBench is unavailable: ${summary}`,
      failures,
    );
  }

  async listTools(): Promise<{
    identity: RuntimeIdentity;
    tools: RemoteToolDescriptor[];
  }> {
    const resolved = await this.resolve();
    const body = await this.fetchJson(resolved.identity, "/api/tools", {
      timeoutMs: this.callTimeoutMs,
      mutation: false,
    });
    const object = requireJsonObject(body, "/api/tools response");
    if (!Array.isArray(object.tools)) {
      throw new Error('DevBench /api/tools response is missing a "tools" array');
    }
    const tools = object.tools.map((entry, index) =>
      parseToolDescriptor(entry, index),
    );
    await this.verifyRuntimeFile(resolved.identity);
    return { identity: resolved.identity, tools };
  }

  async callTool(
    name: string,
    args: JsonObject,
    options: { mutation: boolean; timeoutMs?: number },
  ): Promise<{ identity: RuntimeIdentity; value: JsonValue }> {
    const resolved = await this.resolve();
    const value = await this.fetchJson(
      resolved.identity,
      `/api/tool/${encodeURIComponent(name)}`,
      {
        method: "POST",
        body: args,
        timeoutMs: options.timeoutMs ?? this.callTimeoutMs,
        mutation: options.mutation,
      },
    );
    try {
      await this.verifyRuntimeFile(resolved.identity);
    } catch (error) {
      if (options.mutation) {
        throw new UncertainMutationError(
          `DevBench mutation returned but ${errorMessage(error)}; it was not retried and the outcome is uncertain`,
        );
      }
      throw error;
    }
    return { identity: resolved.identity, value };
  }

  async events(since: number): Promise<{
    identity: RuntimeIdentity;
    value: RemoteEvents;
  }> {
    const resolved = await this.resolve();
    const body = await this.fetchJson(
      resolved.identity,
      `/api/events?since=${String(since)}`,
      { timeoutMs: this.callTimeoutMs, mutation: false },
    );
    const object = requireJsonObject(body, "/api/events response");
    if (!Number.isSafeInteger(object.headSeq) || !Array.isArray(object.events)) {
      throw new Error("DevBench /api/events response is malformed");
    }
    await this.verifyRuntimeFile(resolved.identity);
    return {
      identity: resolved.identity,
      value: {
        headSeq: object.headSeq as number,
        events: object.events.map(asJsonValue),
      },
    };
  }

  private async verifyRuntimeFile(identity: RuntimeIdentity): Promise<void> {
    let current: RuntimeIdentity;
    try {
      current = await readRuntimeIdentity(identity.runtimeFile);
    } catch (error) {
      throw new IdentityChangedError(
        `runtime identity became unavailable during the call: ${errorMessage(error)}`,
      );
    }
    if (!sameIdentity(identity, current)) {
      throw new IdentityChangedError(
        `DevBench instance changed during the call (expected ${identity.instanceId}, found ${current.instanceId})`,
      );
    }
  }

  private async fetchJson(
    identity: RuntimeIdentity,
    path: string,
    options: FetchJsonOptions,
  ): Promise<JsonValue> {
    const url = `http://127.0.0.1:${String(identity.port)}${path}`;
    let response: Response;
    try {
      response = await this.fetchImplementation(url, {
        method: options.method ?? "GET",
        redirect: "error",
        headers: {
          Accept: "application/json",
          "X-DevBench-Instance": identity.instanceId,
          ...(options.body ? { "Content-Type": "application/json" } : {}),
        },
        ...(options.body ? { body: JSON.stringify(options.body) } : {}),
        signal: AbortSignal.timeout(options.timeoutMs),
      });
    } catch (error) {
      const detail = `${url}: ${errorMessage(error)}`;
      if (options.mutation) {
        throw new UncertainMutationError(
          `DevBench mutation response was lost or timed out and was not retried; outcome is uncertain (${detail})`,
        );
      }
      throw new GameUnavailableError(`DevBench is not reachable at ${detail}`);
    }

    let text: string;
    try {
      text = await response.text();
    } catch (error) {
      if (options.mutation) {
        throw new UncertainMutationError(
          `DevBench mutation response body was interrupted and was not retried; outcome is uncertain (${errorMessage(error)})`,
        );
      }
      throw new GameUnavailableError(`DevBench response body was interrupted: ${errorMessage(error)}`);
    }
    let parsed: JsonValue;
    try {
      parsed = asJsonValue(JSON.parse(text) as unknown);
    } catch (error) {
      if (options.mutation) {
        throw new UncertainMutationError(
          `DevBench mutation returned malformed JSON and was not retried; outcome is uncertain (${errorMessage(error)})`,
        );
      }
      throw new Error(
        `DevBench returned malformed JSON from ${path}: ${errorMessage(error)}`,
        { cause: error },
      );
    }
    if (!response.ok) throw new RemoteDomainError(response.status, parsed);
    return parsed;
  }
}

function requireJsonObject(value: JsonValue, context: string): JsonObject {
  if (!isObject(value)) throw new Error(`${context} must be a JSON object`);
  return asJsonValue(value) as JsonObject;
}

function requireHealth(value: JsonValue): Record<string, unknown> {
  const object = requireJsonObject(value, "DevBench health response");
  if (object.ok !== true) throw new Error("DevBench health response is not ok");
  return object;
}

function verifyHealthIdentity(
  identity: RuntimeIdentity,
  health: Record<string, unknown>,
): void {
  if (
    health.pid !== identity.pid ||
    health.port !== identity.port ||
    health.instanceId !== identity.instanceId ||
    typeof health.exePath !== "string" ||
    !sameWindowsPath(health.exePath, identity.exePath) ||
    health.runtime !== identity.runtime ||
    typeof health.dllPath !== "string" ||
    !sameWindowsPath(health.dllPath, identity.dllPath)
  ) {
    throw new Error(
      `health identity does not match runtime.json for instance ${identity.instanceId}`,
    );
  }
}

export function parseToolDescriptor(
  value: unknown,
  index: number,
): RemoteToolDescriptor {
  if (!isObject(value)) {
    throw new Error(`DevBench tool descriptor ${String(index)} is not an object`);
  }
  if (
    typeof value.name !== "string" ||
    value.name.length === 0 ||
    typeof value.description !== "string" ||
    !isObject(value.inputSchema)
  ) {
    throw new Error(`DevBench tool descriptor ${String(index)} is malformed`);
  }
  return {
    name: value.name,
    description: value.description,
    inputSchema: asJsonValue(value.inputSchema) as JsonObject,
    readOnly: value.readOnly === true,
  };
}
