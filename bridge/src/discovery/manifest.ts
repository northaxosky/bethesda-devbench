import { win32 } from "node:path";

import manifest from "../generated/core-tools.json" with { type: "json" };
import { requireObject, requireString } from "../json.js";

export interface GameProfile {
  id: string;
  displayName: string;
  extender: string;
  executable: string;
  aliases: string[];
}

export interface PackagedCatalog {
  profile: GameProfile;
  tools: unknown[];
}

function readManifest(value: unknown): PackagedCatalog[] {
  const root = requireObject(value, "packaged tool manifest");
  if (root.format !== "devbench.core-tools-2" || !Array.isArray(root.catalogs)) {
    throw new Error("The packaged game tool manifest is invalid");
  }
  const names = new Set<string>();
  const executables = new Set<string>();
  const catalogs = root.catalogs.map((entry) => {
    const catalog = requireObject(entry, "packaged game catalog");
    const profile = requireObject(catalog.profile, "packaged game profile");
    const id = requireString(profile, "id", "game profile");
    const extender = requireString(profile, "extender", "game profile");
    const executable = requireString(profile, "executable", "game profile");
    if (
      catalog.format !== "devbench.core-tools-1" ||
      catalog.game !== id ||
      !/^[a-z][a-z0-9-]*$/.test(id) ||
      !/^[A-Za-z][A-Za-z0-9]*$/.test(extender) ||
      win32.basename(executable) !== executable ||
      !executable.toLowerCase().endsWith(".exe") ||
      !Array.isArray(catalog.tools) ||
      !Array.isArray(profile.aliases)
    ) {
      throw new Error(`The packaged ${id} catalog or profile is invalid`);
    }
    const aliases = profile.aliases.map((alias) => {
      if (typeof alias !== "string" || !/^[a-z][a-z0-9-]*$/.test(alias)) {
        throw new Error(`The packaged ${id} game alias is invalid`);
      }
      return alias;
    });
    for (const name of [id, ...aliases]) {
      if (names.has(name)) throw new Error(`Duplicate packaged game name: ${name}`);
      names.add(name);
    }
    const executableKey = executable.toLowerCase();
    if (executables.has(executableKey)) {
      throw new Error(`Duplicate packaged game executable: ${executable}`);
    }
    executables.add(executableKey);
    return {
      profile: {
        id,
        displayName: requireString(profile, "displayName", "game profile"),
        extender,
        executable,
        aliases,
      },
      tools: catalog.tools,
    };
  });
  if (catalogs.length === 0) throw new Error("The packaged game tool manifest is empty");
  return catalogs;
}

export const PACKAGED_CATALOGS = readManifest(manifest);
export const GAME_PROFILES = PACKAGED_CATALOGS.map((catalog) => catalog.profile);

export function resolveGameProfile(game: string): GameProfile {
  const name = game.toLowerCase();
  const profile = GAME_PROFILES.find(
    (candidate) => candidate.id === name || candidate.aliases.includes(name),
  );
  if (!profile) {
    throw new Error(
      `unsupported --game ${game}; supported games: ${GAME_PROFILES.map((candidate) => candidate.id).join(", ")}`,
    );
  }
  return profile;
}

export function profileForExecutable(path: string): GameProfile | undefined {
  const name = win32.basename(path).toLowerCase();
  return GAME_PROFILES.find((profile) => profile.executable.toLowerCase() === name);
}
