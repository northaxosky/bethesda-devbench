# Bethesda DevBench

A game-agnostic mod development and testing framework based on
[alandtse/devbench](https://github.com/alandtse/devbench).
One MCP/REST host and tool layer serve separate native adapters:

| Build adapter | Game | Extender |
|---|---|---|
| `fallout4` | Fallout 4 1.11.240 | F4SE |
| `skyrimse` | Skyrim SE/AE/VR | SKSE |

`game\starfield` contains only `.gitkeep`; Starfield is not implemented.

The [companion bridge](bridge/README.md) keeps MCP connected across game
restarts and manages controller-owned MO2 sessions. `fallout4-utils` remains
a separate, user-facing plugin.

## Tools

The implementation targets upstream **v1.18.2**:
`input`, `menu`, `console`, `scenario`, `inspect`, `game`, `camera`, `papyrus`,
`capture`, `record`, `recordings`, `wait`, `sleep`, `mcp_bridge_setup`, and `ping`.
Use MCP tool discovery or `GET /api/tools` for arguments and capabilities.

Capture uses optional providers or an explicitly selected native screenshot
fallback, which is inconclusive for visual assertions. No RenderDoc provider
is bundled. Skyrim retains VR controls, hotkeys/autorun, and optional SMF/FUCK menus.

## Build

Requires Windows, MSVC with C++26 support, xmake, and Node.js 22+.
From this repository in PowerShell:

```powershell
git submodule update --init --recursive
Remove-Item Env:\FO4_DEV_MODS, Env:\XSE_FO4_MODS_PATH, Env:\XSE_FO4_GAME_PATH -ErrorAction SilentlyContinue
Remove-Item Env:\SkyrimPluginTargets, Env:\XSE_TES5_MODS_PATH, Env:\XSE_TES5_GAME_PATH -ErrorAction SilentlyContinue
Push-Location bridge
npm ci
Pop-Location
xmake config --game=fallout4 --mode=releasedbg -y
xmake build devbench-package
xmake build devbench-tests
xmake run devbench-tests
```

Use `--game=skyrimse` for the universal Skyrim build. Outputs and PCHs are
isolated under `build\<adapter>`; keep each `devbench.dll` with its matching PDB.
Ship `releasedbg`, not `release`. Build targets do not deploy; keep deployment
variables cleared in every fresh shell. `devbench-package` also builds the companion.

Run `npm run validate` in `bridge` for companion checks without launching the
game. PowerShell 7 and an MO2 launcher entry are required for session control;
see [bridge setup](bridge/README.md). Interop targets are test-only.

## Configuration and connection

Startup reads `Data\<F4SE|SKSE>\Plugins\devbench\config.json` under the game directory.
Missing settings receive defaults; invalid configuration fails startup.
The mutation opt-ins default to off and require a game restart:

```json
{
  "allowConsoleCommands": false,
  "allowGameActions": false,
  "allowControlActions": false,
  "allowPapyrusCalls": false
}
```

Use the companion for restart-surviving MCP, or connect directly to
`http://127.0.0.1:<port>/mcp` (defaults: FO4 8930, Skyrim 8920, VR 8921).
REST exposes `/api/health`, `/api/tools`,
`/api/events`, and `POST /api/tool/{name}`. The selected port and instance
identity are mirrored at `%LOCALAPPDATA%\devbench\<fo4|se|vr>\runtime.json`.

## Safety

- Keep the listener local. Mutation opt-ins are not authentication or a sandbox.
- Use disposable saves: native wait/sleep preserves game rules and autosaves;
  named-save overwrite protection does not protect rolling saves.
- Check completion and gameplay readiness, not just HTTP health or queue
  acknowledgements. Timeouts do not cancel accepted mutations; never blindly retry.
- Use `game` for save/load; raw console save/load is blocked. Replay uses the
  same permission gates and does not substitute a missing named fixture.
- Never close MO2 before the game and its launch shim exit and RootBuilder
  cleanup completes.

## Source

`shared` retains the upstream v1.17.0 (`afd0f3d`) shared sources unchanged.
The shared host and tools live in `src`, native implementations in
`game\fallout4` and `game\skyrimse`, and the external controller in `bridge`.
The plugin is [GPL-3.0](COPYING) with upstream's [modding and linking exceptions](EXCEPTIONS).
The [cross-plugin API glue](include/DevBenchAPI.LICENSE.txt) has a separate MIT license.
