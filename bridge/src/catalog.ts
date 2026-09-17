import catalog from "./generated/core-tools.json" with { type: "json" };

import { isObject } from "./json.js";
import { parseToolDescriptor, type RemoteToolDescriptor } from "./remote.js";

function readCoreTools(value: unknown): RemoteToolDescriptor[] {
  if (
    !isObject(value) ||
    value.format !== "devbench.core-tools-1" ||
    value.game !== "fo4" ||
    !Array.isArray(value.tools)
  ) {
    throw new Error("The packaged Fallout 4 tool catalog is invalid");
  }
  const tools = value.tools.map((tool, index) => parseToolDescriptor(tool, index));
  if (tools.length === 0 || new Set(tools.map((tool) => tool.name)).size !== tools.length) {
    throw new Error("The packaged Fallout 4 tool catalog is empty or has duplicate names");
  }
  return tools;
}

export const CORE_TOOLS = readCoreTools(catalog);
