import { PACKAGED_CATALOGS, resolveGameProfile } from "./discovery/manifest.js";
import { parseToolDescriptor, type RemoteToolDescriptor } from "./remote.js";

const catalogs = new Map(PACKAGED_CATALOGS.map(({ profile, tools: entries }) => {
  const tools = entries.map((tool, index) => parseToolDescriptor(tool, index));
  if (tools.length === 0 || new Set(tools.map((tool) => tool.name)).size !== tools.length) {
    throw new Error(`The packaged ${profile.displayName} tool catalog is empty or has duplicate names`);
  }
  return [profile.id, tools];
}));

export function coreTools(game: string): readonly RemoteToolDescriptor[] {
  const profile = resolveGameProfile(game);
  const tools = catalogs.get(profile.id);
  if (!tools) throw new Error(`The packaged ${profile.displayName} tool catalog is missing`);
  return tools;
}
