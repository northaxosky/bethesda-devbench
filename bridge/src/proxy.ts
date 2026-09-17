import type { Tool } from "@modelcontextprotocol/sdk/types.js";

import { CORE_TOOLS } from "./catalog.js";
import { asJsonValue, errorMessage, type JsonObject, type JsonValue } from "./json.js";
import {
  GameUnavailableError,
  IdentityChangedError,
  RemoteClient,
  RemoteDomainError,
  type RemoteToolDescriptor,
} from "./remote.js";
import type { RuntimeIdentity } from "./runtime.js";

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

export interface RemoteProxyOptions {
  pollIntervalMs?: number;
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
  private readonly logger: (message: string) => void;

  constructor(
    readonly remote: RemoteClient,
    options: RemoteProxyOptions = {},
  ) {
    this.onListChanged = options.onListChanged;
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
    return CORE_TOOLS.map((tool) => toMcpTool(tool, true));
  }

  async callTool(name: string, args: JsonObject): Promise<ProxyCallResult> {
    const descriptor = this.liveTools.find((tool) => tool.name === name);
    try {
      const result = await this.remote.callTool(name, args, {
        mutation: descriptor?.readOnly !== true,
      });
      this.identity = result.identity;
      return { value: result.value, isError: false };
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

function toMcpTool(tool: RemoteToolDescriptor, offline = false): Tool {
  return {
    name: tool.name,
    description: offline
      ? `${tool.description}\nOffline schema: this tool requires a running Fallout 4 DevBench instance.`
      : tool.description,
    inputSchema: asJsonValue(tool.inputSchema) as Tool["inputSchema"],
    ...(tool.readOnly ? { annotations: { readOnlyHint: true } } : {}),
  };
}
