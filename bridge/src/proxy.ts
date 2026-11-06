import type { Tool } from "@modelcontextprotocol/sdk/types.js";

import { coreTools } from "./catalog.js";
import { asJsonValue, errorMessage, isObject, type JsonObject, type JsonValue } from "./json.js";
import {
  GameUnavailableError,
  IdentityChangedError,
  RemoteClient,
  RemoteDomainError,
  type RemoteToolDescriptor,
} from "./remote.js";
import type { RuntimeIdentity } from "./runtime.js";
import { DEFAULT_SESSION_TIMINGS, callTimeoutFor, type WaitTimings } from "./timeouts.js";
import {
  AWAIT_SCHEMA,
  awaitsOutcome,
  outcomeFor,
  splitAwait,
  type AwaitArgument,
} from "./waits/outcomes.js";
import type { OwnedSignals } from "./waits/signals.js";
import type { CallContext } from "./waits/types.js";
import { awaitCondition } from "./waits/waiter.js";

export interface ProxyCallResult {
  value: JsonValue;
  isError: boolean;
}

export interface ProxyStatus {
  connected: boolean;
  identity?: RuntimeIdentity;
  remoteTools: string[];
  unavailableReason?: string;
}

/** What a proxy wait may use beyond the remote: timings and owned-game signals. */
export interface WaitEnvironment {
  timings: WaitTimings;
  owned?: OwnedSignals | undefined;
}

const DEFAULT_ENVIRONMENT: WaitEnvironment = { timings: DEFAULT_SESSION_TIMINGS };

export interface RemoteProxyOptions {
  pollIntervalMs?: number;
  // Resolves the wait environment for the game instance a call reached.
  environment?: (instanceId: string) => Promise<WaitEnvironment>;
  onListChanged?: () => Promise<void>;
  logger?: (message: string) => void;
}

export class RemoteProxy {
  private liveTools: RemoteToolDescriptor[] = [];
  private identity: RuntimeIdentity | undefined;
  private unavailableReason: string | undefined;
  private fingerprint = "";
  private polling = false;
  private timer: NodeJS.Timeout | undefined;
  private readonly onListChanged: (() => Promise<void>) | undefined;
  private readonly environment: (instanceId: string) => Promise<WaitEnvironment>;
  private readonly logger: (message: string) => void;

  constructor(
    readonly remote: RemoteClient,
    options: RemoteProxyOptions = {},
  ) {
    this.onListChanged = options.onListChanged;
    this.environment = options.environment ?? (() => Promise.resolve(DEFAULT_ENVIRONMENT));
    this.logger = options.logger ?? ((message) => console.error(message));
    const interval = options.pollIntervalMs ?? 1_000;
    if (interval > 0) {
      this.timer = setInterval(() => {
        void this.poll();
      }, interval);
      this.timer.unref();
    }
  }

  async listRemoteTools(): Promise<Tool[]> {
    await this.refresh();
    if (this.identity) return this.liveTools.map((tool) => toMcpTool(tool));
    return coreTools(this.remote.target.game).map(
      (tool) => toMcpTool(tool, this.remote.target.profile.displayName),
    );
  }

  async callTool(
    name: string,
    args: JsonObject,
    context: CallContext = {},
  ): Promise<ProxyCallResult> {
    let split: ReturnType<typeof splitAwait>;
    try {
      split = splitAwait(args);
    } catch (error) {
      return { value: { ok: false, reason: errorMessage(error) }, isError: true };
    }
    const { forwarded, awaitArg } = split;
    const descriptor = this.liveTools.find((tool) => tool.name === name);
    const mutation = descriptor?.readOnly !== true;
    if (!mutation && typeof awaitArg === "object") {
      return {
        value: { ok: false, reason: `await {event, correlate} needs a mutating tool; ${name} is read-only` },
        isError: true,
      };
    }
    const waiting = mutation && awaitsOutcome(name, forwarded, awaitArg);
    const timeoutMs = callTimeoutFor(name, forwarded, this.remote.callTimeoutMs);
    try {
      // Events from this call's effects must come after the cursor.
      const since = waiting ? await this.remote.headSeq() : undefined;
      const result = await this.remote.callTool(name, forwarded, {
        mutation,
        ...(timeoutMs !== undefined ? { timeoutMs } : {}),
      });
      this.identity = result.identity;
      if (since === undefined || !isObject(result.value)) {
        return { value: result.value, isError: false };
      }
      return await this.awaitOutcome(name, forwarded, awaitArg, {
        receipt: result.value,
        instanceId: result.identity.instanceId,
        since,
        context,
      });
    } catch (error) {
      if (error instanceof RemoteDomainError) {
        return { value: error.body, isError: true };
      }
      if (
        error instanceof GameUnavailableError ||
        error instanceof IdentityChangedError
      ) {
        await this.markUnavailable(errorMessage(error));
      }
      return {
        value: {
          ok: false,
          reason: errorMessage(error),
          availability: "offline",
        },
        isError: true,
      };
    }
  }

  // A failed wait reports in `outcome`; it never re-sends the mutation.
  private async awaitOutcome(
    name: string,
    args: JsonObject,
    awaitArg: AwaitArgument | undefined,
    call: { receipt: JsonObject; instanceId: string; since: number; context: CallContext },
  ): Promise<ProxyCallResult> {
    const { receipt, instanceId, since, context } = call;
    const outcome = outcomeFor(name, args, awaitArg, receipt);
    if (!outcome) return { value: receipt, isError: false };
    if ("failure" in outcome) {
      return {
        value: { ...receipt, outcome: { ok: false, ...asJsonValue(outcome.failure) as JsonObject } },
        isError: true,
      };
    }
    const environment = await this.environment(instanceId);
    const waited = await awaitCondition({
      remote: this.remote,
      condition: outcome.condition,
      since,
      timings: environment.timings,
      signal: context.signal,
      progress: context.progress,
      owned: environment.owned,
    });
    return {
      value: { ...receipt, outcome: asJsonValue(waited) },
      isError: !waited.ok,
    };
  }

  status(): ProxyStatus {
    return {
      connected: this.identity !== undefined,
      ...(this.identity ? { identity: this.identity } : {}),
      remoteTools: this.liveTools.map((tool) => tool.name),
      ...(this.unavailableReason
        ? { unavailableReason: this.unavailableReason }
        : {}),
    };
  }

  async refresh(): Promise<void> {
    const previous = this.fingerprint;
    try {
      const live = await this.remote.listTools();
      this.identity = live.identity;
      this.liveTools = live.tools.filter(
        (tool) => tool.name !== "devbench.session",
      );
      this.unavailableReason = undefined;
      this.fingerprint = JSON.stringify({
        instanceId: live.identity.instanceId,
        tools: this.liveTools,
      });
    } catch (error) {
      this.identity = undefined;
      this.liveTools = [];
      this.unavailableReason = errorMessage(error);
      this.fingerprint = "offline";
    }
    if (previous !== "" && previous !== this.fingerprint) {
      await this.notifyListChanged();
    }
  }

  close(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = undefined;
  }

  private async poll(): Promise<void> {
    if (this.polling) return;
    this.polling = true;
    try {
      await this.refresh();
    } finally {
      this.polling = false;
    }
  }

  private async markUnavailable(reason: string): Promise<void> {
    const changed = this.fingerprint !== "offline";
    this.identity = undefined;
    this.liveTools = [];
    this.unavailableReason = reason;
    this.fingerprint = "offline";
    if (changed) await this.notifyListChanged();
  }

  private async notifyListChanged(): Promise<void> {
    if (!this.onListChanged) return;
    try {
      await this.onListChanged();
    } catch (error) {
      this.logger(
        `devbench-bridge: tools/list_changed notification failed: ${errorMessage(error)}`,
      );
    }
  }
}

// Mutating tools may block on their outcome; see waits/outcomes.ts.
function withAwait(schema: JsonObject): JsonObject {
  const properties = isObject(schema.properties) ? schema.properties : {};
  return {
    ...schema,
    properties: { ...properties, await: AWAIT_SCHEMA as JsonValue },
  };
}

function toMcpTool(tool: RemoteToolDescriptor, offlineGame?: string): Tool {
  return {
    name: tool.name,
    description: offlineGame
      ? `${tool.description}\nOffline schema: this tool requires a running ${offlineGame} DevBench instance.`
      : tool.description,
    inputSchema: asJsonValue(
      tool.readOnly ? tool.inputSchema : withAwait(tool.inputSchema),
    ) as Tool["inputSchema"],
    ...(tool.readOnly ? { annotations: { readOnlyHint: true } } : {}),
  };
}
