# Bethesda DevBench

A Fallout 4 port of [alandtse/devbench](https://github.com/alandtse/devbench)
for mod development and testing through local MCP and REST tools.
Targets **Fallout 4 1.11.240 with F4SE**; other runtimes and game adapters
are not supported by this port.

The [companion bridge](bridge/README.md) keeps MCP connected across game
restarts and manages controller-owned MO2 sessions. `fallout4-utils` remains
a separate, user-facing plugin.

## Tools

The implementation targets upstream **v1.18.2**:
`input`, `menu`, `console`, `scenario`, `inspect`, `game`, `camera`, `papyrus`,
`capture`, `record`, `recordings`, `wait`, `sleep`, `mcp_bridge_setup`, and `ping`.
Use MCP tool discovery or `GET /api/tools` for arguments and capabilities.

Capture uses optional providers or an explicitly selected native screenshot
fallback, which is inconclusive for visual assertions. No RenderDoc provider,
VR input, or hotkey/autorun frontend is bundled.

## Build

Requires Windows, MSVC with C++26 support, xmake, and Node.js 22+.
From this repository in PowerShell:

```powershell
git submodule update --init --recursive
Remove-Item Env:\FO4_DEV_MODS, Env:\XSE_FO4_MODS_PATH, Env:\XSE_FO4_GAME_PATH -ErrorAction SilentlyContinue
Push-Location bridge
npm ci
Pop-Location
xmake config --mode=releasedbg -y
xmake build
xmake build devbench-tests
xmake run devbench-tests
```

Clear those three variables in **every fresh build shell**: CommonLib can
deploy during a build, even without `xmake install`. Ship `releasedbg`, not
`release`, and keep `build\windows\x64\releasedbg\devbench.dll` with its matching
PDB. The build also packages the standalone companion and its `platform` files.

Run `npm run validate` in `bridge` for companion checks without launching the
game. PowerShell 7 and an MO2 launcher entry are required for session control;
see [bridge setup](bridge/README.md). `devbench-interop` is test-only.

## Configuration and connection

Startup reads `Data\F4SE\Plugins\devbench\config.json` under the game directory.
Missing settings receive defaults; invalid configuration fails startup.
The mutation opt-ins default to off and require a game restart:

```json
{
  "enabled": true,
  "port": 8930,
  "logLevel": "info",
  "allowConsoleCommands": false,
  "allowGameActions": false,
  "allowControlActions": false,
  "allowPapyrusCalls": false
}
```

Use the companion for restart-surviving MCP, or connect directly to
`http://127.0.0.1:8930/mcp`. REST exposes `/api/health`, `/api/tools`,
`/api/events`, and `POST /api/tool/{name}`. The selected port and instance
identity are mirrored at `%LOCALAPPDATA%\devbench\fo4\runtime.json`.

## Safety

- Keep the listener local. Mutation opt-ins are not authentication or a sandbox.
- Use disposable saves: native wait/sleep preserves game rules and autosaves;
  named-save overwrite protection does not protect rolling saves.
- Check completion and gameplay readiness, not just HTTP health or queue
  acknowledgements. Timeouts do not cancel accepted mutations; never blindly retry.
- Use `game` for save/load; raw console save/load is blocked. Replay uses the
  same permission gates and does not substitute a missing named fixture.
- Never close MO2 before Fallout and its launch shim exit and RootBuilder
  cleanup completes.

## Source

`shared` retains the upstream v1.17.0 (`afd0f3d`) shared sources unchanged.
Engine-independent tools live in `src\tools`, the native adapter in
`src\game\fallout4`, and the external controller in `bridge`.
The plugin is [GPL-3.0](COPYING) with upstream's [modding and linking exceptions](EXCEPTIONS).
The [cross-plugin API glue](include/DevBenchAPI.LICENSE.txt) has a separate MIT license.
