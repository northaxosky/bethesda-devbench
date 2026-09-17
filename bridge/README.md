# Fallout 4 DevBench external bridge

This package is the restart-surviving stdio MCP companion for the Fallout 4
DevBench plugin. Its small proxy/discovery shape follows the upstream DevBench
bridge at `726fa8691db22a9e2f42c15406cf4bc504815b36` (v1.18.2), adapted to the
Fallout 4 runtime identity contract and a thin, ownership-safe session
controller.

The bridge:

- keeps one MCP stdio process alive while Fallout 4 stops and restarts;
- starts successfully when no game or `runtime.json` exists;
- always exposes the local `devbench.session` tool;
- exposes a generated FO4 core-tool catalog while offline, without claiming the
  game is ready, and replaces it with live DLL discovery when connected;
- sends `notifications/tools/list_changed` on connection, disconnection,
  instance replacement, or descriptor changes;
- never includes Skyrim or consumer-added tools in its offline catalog;
- connects only to `http://127.0.0.1:<validated-port>`;
- verifies `pid`, `instanceId`, `exePath`, runtime, and DLL identity;
- sends `X-DevBench-Instance` on every health, discovery, event, and tool call;
- never retries a remote POST automatically.

State and protocol diagnostics go to stderr. Stdout is reserved for MCP framing,
except for the explicit `setup` and `--help` commands.

## Commands

```powershell
npm ci
npm run validate
node dist/index.js --game fo4
node dist/index.js setup --game fo4
```

Building the companion uses the repository's Windows C++/xmake toolchain to
generate `src\generated\core-tools.json` from the same descriptor factories as
the plugin. The `devbench-bridge` xmake target builds that dependency before
packaging. Offline calls still return an unavailable error; catalog presence
does not imply a game instance or provider is ready.

Discovery options:

```text
--game fo4
--install C:\Path\To\Fallout 4
--runtime-file C:\Path\To\runtime.json
--config C:\Path\To\session.json
```

`--install` constrains a responding identity to that folder's `Fallout4.exe`.
The normal out-of-VFS candidate is
`%LOCALAPPDATA%\devbench\fo4\runtime.json`. An explicit runtime file is also
searched when provided. A legacy runtime file without `instanceId` is rejected
with an upgrade-required message rather than treated as a controllable host.

`setup` prints only the JSON MCP snippet; it never edits a client config.
Unknown options and missing option values are fatal.

## Session controller

The default machine config path is
`%LOCALAPPDATA%\devbench\fo4\session.json`. It is not created automatically.
The simple proxy remains available when the default file is absent. An explicit
invalid `--config` fails bridge startup. Copy `config.example.json` and replace
every placeholder with an absolute Windows path.

The config names the portable MO2 executable, actual profiles directory and profile,
MO2 shim executable label, game and F4SE loader executables, physical deployed DLL, evidence
directory, and only the log paths that may be collected. It may pin the runtime
version and DLL SHA-256. The controller validates the profile directory and
deployed DLL before launch; it never edits `modlist.txt`, INIs, saves, or any
MO2/game setting.

### One-time MO2 launcher entry

Keep the configured MO2 instance open. The controller will not start, close,
or restart MO2; stopping a game waits for both Fallout and its native shim to exit.
Launch requests use MO2's single-instance pipe and remain connected until the
shim acknowledges them, avoiding early-disconnect races in CLI forwarding.

No elevation or system-wide process tracing is needed. Add an executable to
your portable MO2 instance with:

- **Title:** `DevBench Launch` (matches `executable` in the session config).
- **Binary:** the physical packaged `platform\devbench-launch.exe`.
- **Arguments:** `-GameExe "C:\Path\To\Fallout 4\Fallout4.exe"`
- **Start in:** the Fallout 4 installation directory.

Keep all packaged `platform` files together. Use the physical packaged
paths, not the game's virtual Data directory. The helper validates this entry
in the adjacent `ModOrganizer.ini` before asking MO2 to run it. The config's
`loaderExe` points to `f4se_loader.exe` beside `gameExe`; RootBuilder may supply
it only after MO2 starts the shim.

PowerShell 7 runs only the external broker, outside MO2's VFS. MO2 launches
the native shim instead; the Store PowerShell host crashes under this MO2
installation's injection and must not be used as its executable entry.

The broker creates a current-user-only launch pipe. The MO2-launched native shim
authenticates that pending launch, starts F4SE inside MO2's VFS, retains the
loader handle, and transfers an actual game handle to the broker. The shim
waits for game exit so MO2/RootBuilder keeps its normal cleanup lifetime even
if the bridge disconnects. Starting the shim without a pending controller
launch fails without starting F4SE.

`devbench.session` actions:

- `status`: local controller/config/job/ownership status.
- `start`: asynchronously launch a new game through the Windows helper and wait
  for matching runtime, process-creation, executable, loaded DLL path/hash, and
  runtime-version evidence.
- `run`: asynchronously start or reuse only the already-owned game, load the
  required named `fixture`, wait for the same native `operationId` to succeed
  and for `inspect{kind:"state"}.playerLoaded`, launch a server-side scenario
  with `async:true`, poll status and current-instance progress events, collect
  evidence, then stop or release ownership.
- `stop`: graceful close of the retained owned game only. `force:true` permits
  the helper's retained-handle terminate operation after graceful timeout.
- `cancel`: cooperative cancellation of the active controller job.

Example run call:

```json
{
  "action": "run",
  "fixture": "DevBenchFixture01",
  "steps": [
    {
      "assert": {
        "tool": "inspect",
        "args": { "kind": "state" },
        "path": "/playerLoaded",
        "eq": true
      }
    }
  ],
  "finish": "stop",
  "forceStop": false
}
```

Exactly one of `steps` or `scenarioFile` is required. `finish` defaults to
`stop`; `leave-running` calls the helper's `release` operation, after which this
controller refuses to adopt or stop that process. If a mutation response is
lost or times out, the report says the outcome is uncertain, the mutation is
not retried, and the controller performs no further automatic writes or stop.

## Evidence

Each job creates a unique folder under `resultsDir` and atomically writes
`result.json`. Reports distinguish `launchfailed`, `identitymismatch`,
`loadfailed`, `scenariofailed`, `cancelled`, `cleanupfailed`, and `complete`.
They include the controller run ID, remote instance ID, PID/creation time,
runtime, DLL SHA-256, source paths, expected/actual identity, transcript, and
cleanup result.

Configured logs are snapshotted before the job by size, mtime, file identity,
and a content prefix. Only files created, replaced, truncated, or changed during that job
are copied, up to the configured per-file bound. Larger captures remain
referenced by source path; bounded text captures are marked truncated. Dumps, PDBs, and arbitrary
directories are not copied. No automatic retention or recursive root deletion
is performed.

## Packaging

- Node entry: `dist/index.js`
- Node helper: `dist/platform/windows-session.ps1`
- Broker implementation: `dist/platform/LaunchShim.cs`
- Native launcher: `dist/platform/devbench-launch.exe`
- Standalone entry after `npm run compile`:
  `standalone/devbench-bridge.exe`
- Standalone helper:
  `standalone/platform/windows-session.ps1`
- Standalone broker:
  `standalone/platform/LaunchShim.cs`
- Standalone launcher:
  `standalone/platform/devbench-launch.exe`

The helper is spawned attached with:

```text
<absolute-path-to-pwsh.exe> -NoProfile -NonInteractive -File <path>
```

The Node client resolves PowerShell 7's `pwsh.exe` to an absolute path from
`PATH` or the standard `%ProgramFiles%\PowerShell\7` installation. It does not
fall back to Windows PowerShell 5.1 and never uses `-ExecutionPolicy Bypass`.

EOF and bridge shutdown release native handles/lease; they do not kill Fallout
4 or MO2. The standalone build uses the project-local Bun package only.

## Plugin contract required by the bridge

The plugin's mirrored and Data-relative `runtime.json` must contain:

```json
{
  "pid": 1234,
  "port": 8930,
  "instanceId": "1234-01DC000000000000",
  "exePath": "C:\\Path\\To\\Fallout 4\\Fallout4.exe",
  "runtime": "1.11.240.0",
  "dllPath": "C:\\Physical\\Path\\To\\devbench.dll"
}
```

`instanceId` is `<pid>-<16 uppercase hexadecimal FILETIME creation ticks>`.
`/api/health` must return the matching full identity. Before dispatching
`/api/*` and `/mcp*`, the plugin must reject empty, duplicate, or mismatched
`X-DevBench-Instance` headers with HTTP 409. The header remains optional for
direct clients, but the bridge always sends exactly one. The bridge assumes no
stronger authentication and must remain loopback only.

Proxy/controller tests use fake HTTP servers and injected platforms. Native
tests use synthetic MO2/loader/game processes, including forwarding through an
existing launcher, rapid loader exit, ownership transfer, and bridge EOF.
They do not launch Fallout 4 or MO2. Real MO2 forwarding, RootBuilder behavior,
game window close, and the complete game session still require supervised
runtime validation on the configured profile.
