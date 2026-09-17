import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { once } from "node:events";
import {
  copyFile,
  mkdir,
  readFile,
  rm,
  unlink,
  writeFile,
} from "node:fs/promises";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import path from "node:path";
import test from "node:test";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const bridgeDirectory = path.dirname(testDirectory);
const packagedPlatform = path.join(bridgeDirectory, "dist", "platform");
const fixtureSource = path.join(
  testDirectory,
  "fixtures",
  "SyntheticProcessTree.cs",
);
const temporaryDirectory = path.join(
  bridgeDirectory,
  "build",
  `windows-platform-test-${process.pid}`,
);
const fixtureAssembly = path.join(temporaryDirectory, "fixture.exe");
const helperPath = path.join(temporaryDirectory, "platform", "windows-session.ps1");
const nativeShimPath = path.join(temporaryDirectory, "platform", "devbench-launch.exe");
const mo2PipeName = `DevBenchSyntheticMo2-${process.pid}`;
const mo2Exe = path.join(temporaryDirectory, "SyntheticMo2.exe");
const loaderExe = path.join(temporaryDirectory, "SyntheticLoader.exe");
const gameExe = path.join(temporaryDirectory, "SyntheticGame.exe");
const profilesDir = path.join(temporaryDirectory, "profiles");
const profile = "Synthetic Profile";
const delayFile = path.join(temporaryDirectory, "delay-ms.txt");

function availablePowerShell() {
  const candidates = [
    process.env.PWSH_EXE,
    "pwsh.exe",
    process.env.SystemRoot &&
      path.join(
        process.env.SystemRoot,
        "System32",
        "WindowsPowerShell",
        "v1.0",
        "powershell.exe",
      ),
  ].filter(Boolean);

  for (const candidate of candidates) {
    const probe = spawnSync(
      candidate,
      [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "$PSVersionTable.PSVersion.ToString()",
      ],
      { encoding: "utf8", windowsHide: true },
    );
    if (probe.status === 0) {
      return candidate;
    }
  }
  throw new Error("No usable PowerShell host was found.");
}

const powershell = availablePowerShell();
function shimArgs() {
  return `-GameExe "${gameExe}"`;
}
const fixtureCompiler = path.join(
  process.env.SystemRoot,
  "System32",
  "WindowsPowerShell",
  "v1.0",
  "powershell.exe",
);

function runPowerShell(command, environment = {}, executable = powershell) {
  const encoded = Buffer.from(command, "utf16le").toString("base64");
  const result = spawnSync(
    executable,
    ["-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
    {
      encoding: "utf8",
      env: { ...process.env, ...environment },
      windowsHide: true,
    },
  );
  if (result.status !== 0) {
    throw new Error(
      `PowerShell command failed (${result.status}):\n${result.stderr}\n${result.stdout}`,
    );
  }
  return result.stdout;
}

function windowsSessionCommand(lines) {
  return [
    "$text = Get-Content -LiteralPath $env:HELPER_PATH -Raw",
    "$prefix = '$source = @'''",
    "$start = $text.IndexOf($prefix)",
    "if ($start -lt 0) { throw 'C# source start not found' }",
    "$start += $prefix.Length",
    "$end = $text.IndexOf(\"'@\", $start)",
    "if ($end -lt 0) { throw 'C# source end not found' }",
    "$source = $text.Substring($start, $end - $start)",
    "$source += [Environment]::NewLine + (Get-Content -LiteralPath (Join-Path (Split-Path $env:HELPER_PATH) 'LaunchShim.cs') -Raw)",
    "Add-Type -TypeDefinition $source -Language CSharp",
    ...lines,
  ].join("\n");
}

class HelperClient {
  constructor() {
    this.child = spawn(
      powershell,
      ["-NoProfile", "-NonInteractive", "-File", helperPath],
      {
        stdio: ["pipe", "pipe", "pipe"],
        windowsHide: true,
      },
    );
    this.stderr = "";
    this.lines = [];
    this.waiters = [];
    this.exited = false;
    this.exitPromise = once(this.child, "exit").then(([code, signal]) => {
      this.exited = true;
      while (this.waiters.length !== 0) {
        this.waiters.shift().reject(
          new Error(
            `Helper exited before replying (code=${code}, signal=${signal}).\n${this.stderr}`,
          ),
        );
      }
      return { code, signal };
    });
    this.child.stderr.setEncoding("utf8");
    this.child.stderr.on("data", (chunk) => {
      this.stderr += chunk;
    });
    this.reader = createInterface({ input: this.child.stdout });
    this.reader.on("line", (line) => {
      const waiter = this.waiters.shift();
      if (waiter) {
        waiter.resolve(line);
      } else {
        this.lines.push(line);
      }
    });
  }

  nextLine(timeoutMs = 20_000) {
    if (this.lines.length !== 0) {
      return Promise.resolve(this.lines.shift());
    }
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        const index = this.waiters.findIndex((entry) => entry.resolve === resolve);
        if (index >= 0) {
          this.waiters.splice(index, 1);
        }
        reject(
          new Error(`Timed out waiting for helper response.\n${this.stderr}`),
        );
      }, timeoutMs);
      this.waiters.push({
        resolve: (value) => {
          clearTimeout(timeout);
          resolve(value);
        },
        reject: (error) => {
          clearTimeout(timeout);
          reject(error);
        },
      });
    });
  }

  async raw(line, timeoutMs) {
    this.child.stdin.write(`${line}\n`);
    return JSON.parse(await this.nextLine(timeoutMs));
  }

  request(message, timeoutMs) {
    return this.raw(JSON.stringify(message), timeoutMs);
  }

  async closeInput() {
    if (!this.child.stdin.destroyed) {
      this.child.stdin.end();
    }
    return this.exitPromise;
  }

  async dispose() {
    if (!this.exited) {
      this.child.stdin.end();
      const timer = setTimeout(() => {
        if (!this.exited) {
          this.child.kill();
        }
      }, 5_000);
      await this.exitPromise;
      clearTimeout(timer);
    }
    this.reader.close();
  }
}

function startRequest(id, timeoutMs = 8_000) {
  return {
    id,
    op: "start",
    mo2Exe,
    profilesDir,
    profile,
    executable: "Synthetic",
    gameExe,
    loaderExe,
    timeoutMs,
  };
}

async function removeIfPresent(file) {
  try {
    await unlink(file);
  } catch (error) {
    if (error.code !== "ENOENT") {
      throw error;
    }
  }
}

async function waitForFile(file, timeoutMs = 5_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      return await readFile(file, "utf8");
    } catch (error) {
      if (error.code !== "ENOENT") {
        throw error;
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error(`Timed out waiting for ${file}`);
}

async function stopMarkedSyntheticGame() {
  const marker = path.join(temporaryDirectory, "game.pid");
  let rawPid;
  try {
    rawPid = await readFile(marker, "utf8");
  } catch (error) {
    if (error.code === "ENOENT") {
      return;
    }
    throw error;
  }

  const pid = Number.parseInt(rawPid, 10);
  if (!Number.isInteger(pid) || pid <= 0) {
    return;
  }
  try {
    runPowerShell(
      [
        "$pidValue = [int]$env:SYNTHETIC_PID",
        "$expected = [IO.Path]::GetFullPath($env:SYNTHETIC_IMAGE)",
        "$process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue",
        "if ($null -ne $process) {",
        "  $actual = [IO.Path]::GetFullPath($process.MainModule.FileName)",
        "  if (-not [string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) { throw 'PID no longer belongs to the synthetic game' }",
        "  Stop-Process -Id $pidValue -Force",
        "}",
        "exit 0",
      ].join("; "),
      { SYNTHETIC_PID: String(pid), SYNTHETIC_IMAGE: gameExe },
    );
  } catch (error) {
    throw error;
  } finally {
    await removeIfPresent(marker);
  }
}

async function resetFixture(delayMs = 0) {
  await Promise.all([
    removeIfPresent(path.join(temporaryDirectory, "mo2.pid")),
    removeIfPresent(path.join(temporaryDirectory, "loader.pid")),
    removeIfPresent(path.join(temporaryDirectory, "game.pid")),
    removeIfPresent(path.join(temporaryDirectory, "forward.pipe")),
  ]);
  await writeFile(delayFile, String(delayMs), "utf8");
}

test.before(async () => {
  await rm(temporaryDirectory, { recursive: true, force: true });
  await mkdir(path.join(profilesDir, profile), { recursive: true });
  await mkdir(path.dirname(helperPath), { recursive: true });
  await copyFile(path.join(packagedPlatform, "windows-session.ps1"), helperPath);
  await copyFile(path.join(packagedPlatform, "devbench-launch.exe"), nativeShimPath);
  const broker = await readFile(path.join(packagedPlatform, "LaunchShim.cs"), "utf8");
  assert.equal(broker.split("mo-43d1a3ad-eeb0-4818-97c9-eda5216c29b5").length, 2);
  await writeFile(path.join(temporaryDirectory, "platform", "LaunchShim.cs"),
    broker.replace("mo-43d1a3ad-eeb0-4818-97c9-eda5216c29b5", mo2PipeName));
  await writeFile(path.join(temporaryDirectory, "ipc-name.txt"), mo2PipeName);

  runPowerShell(
    [
      "$source = Get-Content -LiteralPath $env:FIXTURE_SOURCE -Raw",
      "Add-Type -TypeDefinition $source -Language CSharp -OutputAssembly $env:FIXTURE_OUTPUT -OutputType ConsoleApplication",
    ].join("; "),
    {
      FIXTURE_SOURCE: fixtureSource,
      FIXTURE_OUTPUT: fixtureAssembly,
    },
    fixtureCompiler,
  );

  await Promise.all([
    copyFile(fixtureAssembly, mo2Exe),
    copyFile(fixtureAssembly, loaderExe),
    copyFile(fixtureAssembly, gameExe),
    writeFile(
      path.join(temporaryDirectory, "ModOrganizer.ini"),
      `[customExecutables]\n1\\title=Synthetic\n1\\binary=${nativeShimPath}\n1\\arguments=${shimArgs()}\n`,
      "utf8",
    ),
    writeFile(
      path.join(temporaryDirectory, "shim-config.txt"),
      `${nativeShimPath}\n${shimArgs()}\n`,
      "utf8",
    ),
  ]);
});

test.afterEach(async () => {
  await stopMarkedSyntheticGame();
});

test.after(async () => {
  await stopMarkedSyntheticGame();
  await rm(temporaryDirectory, { recursive: true, force: true });
});

test("keeps JSONL alive after malformed input and rejects unowned close", async () => {
  const helper = new HelperClient();
  try {
    const malformed = await helper.raw("{not-json");
    assert.equal(malformed.id, null);
    assert.equal(malformed.ok, false);
    assert.equal(malformed.error.code, "INVALID_JSON");

    const oversized = await helper.raw("x".repeat(1024 * 1024 + 1));
    assert.equal(oversized.id, null);
    assert.equal(oversized.ok, false);
    assert.equal(oversized.error.code, "LINE_TOO_LONG");

    const status = await helper.request({ id: 1, op: "status" });
    assert.deepEqual(status, {
      id: 1,
      ok: true,
      result: { owned: false, running: false },
    });

    const close = await helper.request({ id: 2, op: "close", timeoutMs: 50 });
    assert.equal(close.id, 2);
    assert.equal(close.ok, false);
    assert.equal(close.error.code, "NO_OWNED_GAME");

    const exit = await helper.closeInput();
    assert.equal(exit.code, 0);
  } finally {
    await helper.dispose();
  }
});

test("inspects exact process identity and only hashes the requested module", async () => {
  const child = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
    stdio: "ignore",
    windowsHide: true,
  });
  const helper = new HelperClient();
  try {
    const inspected = await helper.request({
      id: 10,
      op: "inspect",
      pid: child.pid,
      gameExe: process.execPath,
      pluginName: "kernel32.dll",
    });
    assert.equal(inspected.ok, true, JSON.stringify(inspected));
    assert.equal(inspected.result.pid, child.pid);
    assert.equal(inspected.result.running, true);
    assert.match(inspected.result.creationTime, /Z$/);
    assert.equal(
      path.resolve(inspected.result.exe).toLowerCase(),
      path.resolve(process.execPath).toLowerCase(),
    );
    assert.equal(path.basename(inspected.result.plugin.path).toLowerCase(), "kernel32.dll");
    assert.match(inspected.result.plugin.sha256, /^[0-9a-f]{64}$/);

    const expectedHash = createHash("sha256")
      .update(await readFile(inspected.result.plugin.path))
      .digest("hex");
    assert.equal(inspected.result.plugin.sha256, expectedHash);

    const mismatch = await helper.request({
      id: 11,
      op: "inspect",
      pid: child.pid,
      gameExe,
    });
    assert.equal(mismatch.ok, false);
    assert.equal(mismatch.error.code, "PROCESS_PATH_MISMATCH");
  } finally {
    child.kill();
    await once(child, "exit");
    await helper.dispose();
  }
});

test("rejects profile traversal before launching anything", async () => {
  await resetFixture();
  const helper = new HelperClient();
  try {
    const response = await helper.request({
      ...startRequest(20, 2_000),
      profile: `..${path.sep}${profile}`,
    });
    assert.equal(response.ok, false);
    assert.equal(response.error.code, "INVALID_PROFILE");
    await assert.rejects(
      readFile(path.join(temporaryDirectory, "mo2.pid"), "utf8"),
      { code: "ENOENT" },
    );
  } finally {
    await helper.dispose();
  }
});

test("non-admin shim proves synthetic MO2-loader-game ownership despite rapid loader exit", async () => {
  await resetFixture();
  const existing = spawn(mo2Exe, ["--serve"], { stdio: "ignore", windowsHide: true });
  const existingExit = once(existing, "exit");
  await waitForFile(path.join(temporaryDirectory, "forward.pipe"));
  const helper = new HelperClient();
  try {
    const started = await helper.request(startRequest(30), 20_000);
    assert.equal(started.ok, true, JSON.stringify(started));
    assert.equal(started.result.owned, true);
    assert.equal(started.result.running, true);
    assert.equal(
      path.resolve(started.result.exe).toLowerCase(),
      path.resolve(gameExe).toLowerCase(),
    );
    assert.equal(
      started.result.pid,
      Number.parseInt(await waitForFile(path.join(temporaryDirectory, "game.pid")), 10),
    );
    assert.equal(
      started.result.mo2Pid,
      Number.parseInt(await readFile(path.join(temporaryDirectory, "mo2.pid"), "utf8"), 10),
    );

    const close = await helper.request({ id: 31, op: "close", timeoutMs: 50 });
    assert.equal(close.ok, true);
    assert.equal(close.result.exited, false);
    assert.equal(close.result.reason, "timeout");
    assert.equal(close.result.owned, true);

    const terminated = await helper.request({ id: 32, op: "terminate" });
    assert.equal(terminated.ok, true, JSON.stringify(terminated));
    assert.equal(terminated.result.exited, true);
    assert.equal(terminated.result.pid, started.result.pid);
  } finally {
    await helper.dispose();
    await stopMarkedSyntheticGame();
    if (existing.exitCode === null) existing.kill();
    await existingExit;
  }
});

test("does not start MO2 when the configured instance is closed", async () => {
  await resetFixture();
  const helper = new HelperClient();
  try {
    const result = await helper.request(startRequest(35));
    assert.equal(result.ok, false);
    assert.equal(result.error.code, "MO2_NOT_RUNNING");
    await assert.rejects(readFile(path.join(temporaryDirectory, "mo2.pid")), { code: "ENOENT" });
  } finally {
    await helper.dispose();
  }
});

test("blocks a second controller with the canonical-image lease", async () => {
  await resetFixture();
  const command = windowsSessionCommand([
    "$assembly = [BethesdaDevBench.WindowsSession.Controller].Assembly",
    "$processType = $assembly.GetType('BethesdaDevBench.WindowsSession.WindowsProcess', $true)",
    "$flags = [Reflection.BindingFlags]::Static -bor [Reflection.BindingFlags]::NonPublic",
    "$canonicalize = $processType.GetMethod('CanonicalizeExistingPath', $flags)",
    "$canonical = $canonicalize.Invoke($null, @($env:GAME_EXE, $false))",
    "$controller = New-Object BethesdaDevBench.WindowsSession.Controller",
    "$instanceFlags = [Reflection.BindingFlags]::Instance -bor [Reflection.BindingFlags]::NonPublic",
    "$acquire = $controller.GetType().GetMethod('AcquireLease', $instanceFlags)",
    "[void]$acquire.Invoke($controller, @($canonical))",
    "[Console]::Out.WriteLine('ready')",
    "[Console]::Out.Flush()",
    "[void][Console]::In.ReadLine()",
    "$controller.Dispose()",
  ]);
  const holder = spawn(
    powershell,
    [
      "-NoProfile",
      "-NonInteractive",
      "-EncodedCommand",
      Buffer.from(command, "utf16le").toString("base64"),
    ],
    {
      env: { ...process.env, HELPER_PATH: helperPath, GAME_EXE: gameExe },
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
    },
  );
  let holderStderr = "";
  holder.stderr.setEncoding("utf8");
  holder.stderr.on("data", (chunk) => {
    holderStderr += chunk;
  });
  const holderReader = createInterface({ input: holder.stdout });
  const holderReady = once(holderReader, "line");
  const helper = new HelperClient();
  try {
    const [ready] = await Promise.race([
      holderReady,
      once(holder, "exit").then(([code]) => {
        throw new Error(`Lease holder exited (${code}).\n${holderStderr}`);
      }),
    ]);
    assert.equal(ready, "ready");

    const conflict = await helper.request(startRequest(41, 2_000), 10_000);
    assert.equal(conflict.ok, false, JSON.stringify(conflict));
    assert.equal(conflict.error.code, "LEASE_BUSY");
  } finally {
    holder.stdin.end("\n");
    await once(holder, "exit");
    holderReader.close();
    await helper.dispose();
  }
});

test("reconciles exited identity-query races only from the signaled retained handle", async () => {
  await resetFixture();
  const command = windowsSessionCommand([
    "$assembly = [BethesdaDevBench.WindowsSession.Controller].Assembly",
    "$processType = $assembly.GetType('BethesdaDevBench.WindowsSession.WindowsProcess', $true)",
    "$ownedType = $assembly.GetType('BethesdaDevBench.WindowsSession.OwnedSession', $true)",
    "$staticFlags = [Reflection.BindingFlags]::Static -bor [Reflection.BindingFlags]::NonPublic",
    "$fieldFlags = [Reflection.BindingFlags]::Instance -bor [Reflection.BindingFlags]::Public",
    "$open = $processType.GetMethod('OpenProcessHandle', $staticFlags)",
    "$queryImage = $processType.GetMethod('QueryImagePath', $staticFlags)",
    "$queryCreation = $processType.GetMethod('QueryCreationFileTime', $staticFlags)",
    "$isRunning = $processType.GetMethod('IsRunning', $staticFlags)",
    "$revalidate = $processType.GetMethod('RevalidateOwnedAfterRunningObserved', $staticFlags)",
    "if ($null -eq $revalidate) { throw 'Race revalidation method not found' }",
    "$handleField = $ownedType.GetField('Handle', $fieldFlags)",
    "$imageField = $ownedType.GetField('ImagePath', $fieldFlags)",
    "$creationField = $ownedType.GetField('CreationFileTime', $fieldFlags)",
    "function New-OwnedSession([object]$handle, [string]$image, [long]$creation) {",
    "  $session = [Activator]::CreateInstance($ownedType, $true)",
    "  $handleField.SetValue($session, $handle)",
    "  $imageField.SetValue($session, $image)",
    "  $creationField.SetValue($session, $creation)",
    "  return $session",
    "}",
    "$fullAccess = [uint32](0x0001 -bor 0x1000 -bor 0x00100000)",
    "$exitedProcess = Start-Process -FilePath $env:SYNTHETIC_IMAGE -PassThru",
    "$exitedSession = $null",
    "$liveProcess = $null",
    "$liveQueryHandle = $null",
    "$liveSession = $null",
    "try {",
    "  $exitedHandle = $open.Invoke($null, @($exitedProcess.Id, $fullAccess, 'TEST_OPEN_FAILED'))",
    "  $exitedImage = $queryImage.Invoke($null, @($exitedHandle))",
    "  $exitedCreation = $queryCreation.Invoke($null, @($exitedHandle))",
    "  if (-not $isRunning.Invoke($null, @($exitedHandle))) { throw 'Synthetic process was not initially live' }",
    "  $exitedSession = New-OwnedSession $exitedHandle $exitedImage $exitedCreation",
    "  Stop-Process -Id $exitedProcess.Id -Force",
    "  if (-not $exitedProcess.WaitForExit(5000)) { throw 'Synthetic process did not exit' }",
    "  $exitedResult = $revalidate.Invoke($null, @($exitedSession))",
    "",
    "  $liveProcess = Start-Process -FilePath $env:SYNTHETIC_IMAGE -PassThru",
    "  $liveQueryHandle = $open.Invoke($null, @($liveProcess.Id, $fullAccess, 'TEST_OPEN_FAILED'))",
    "  $liveImage = $queryImage.Invoke($null, @($liveQueryHandle))",
    "  $liveCreation = $queryCreation.Invoke($null, @($liveQueryHandle))",
    "  $syncHandle = $open.Invoke($null, @($liveProcess.Id, [uint32]0x00100000, 'TEST_OPEN_FAILED'))",
    "  $liveSession = New-OwnedSession $syncHandle $liveImage $liveCreation",
    "  $liveFailure = $null",
    "  try {",
    "    [void]$revalidate.Invoke($null, @($liveSession))",
    "  } catch {",
    "    $failure = $_.Exception.InnerException",
    "    if ($null -eq $failure) { throw }",
    "    $liveFailure = $failure.Code",
    "  }",
    "  [pscustomobject]@{",
    "    exitedResult = [bool]$exitedResult",
    "    liveFailure = $liveFailure",
    "    liveStillRunning = [bool]$isRunning.Invoke($null, @($syncHandle))",
    "  } | ConvertTo-Json -Compress",
    "} finally {",
    "  if ($null -ne $exitedSession) { $exitedSession.Dispose() }",
    "  if ($null -ne $liveSession) { $liveSession.Dispose() }",
    "  if ($null -ne $liveQueryHandle) { $liveQueryHandle.Dispose() }",
    "  if ($null -ne $exitedProcess -and -not $exitedProcess.HasExited) {",
    "    Stop-Process -Id $exitedProcess.Id -Force",
    "    [void]$exitedProcess.WaitForExit(5000)",
    "  }",
    "  if ($null -ne $liveProcess -and -not $liveProcess.HasExited) {",
    "    Stop-Process -Id $liveProcess.Id -Force",
    "    [void]$liveProcess.WaitForExit(5000)",
    "  }",
    "}",
  ]);

  const result = JSON.parse(
    runPowerShell(command, {
      HELPER_PATH: helperPath,
      SYNTHETIC_IMAGE: gameExe,
    }).trim(),
  );
  assert.equal(result.exitedResult, false);
  assert.equal(result.liveFailure, "PROCESS_QUERY_FAILED");
  assert.equal(result.liveStillRunning, true);
});

test("the shim supports forwarding through an already-running synthetic MO2", async () => {
  await resetFixture(250);
  const existing = spawn(mo2Exe, ["--serve"], { stdio: "ignore", windowsHide: true });
  const existingExit = once(existing, "exit");
  const helper = new HelperClient();
  try {
    await waitForFile(path.join(temporaryDirectory, "forward.pipe"));
    const started = await helper.request(startRequest(45), 20_000);
    assert.equal(started.ok, true, JSON.stringify(started));
    assert.equal(started.result.mo2Pid, existing.pid);
    const forwarded = await readFile(path.join(temporaryDirectory, "forwarded-command.txt"), "utf8");
    assert.match(forwarded, /-p "Synthetic Profile" run -e Synthetic$/);
    const stopped = await helper.request({ id: 46, op: "terminate" });
    assert.equal(stopped.ok, true, JSON.stringify(stopped));
    assert.equal(stopped.result.exited, true);
    await existingExit;
  } finally {
    await helper.dispose();
    if (existing.exitCode === null) {
      existing.kill();
      await existingExit;
    }
  }
});

test("invalid shim entry fails before MO2 or its loader can start", async () => {
  await resetFixture();
  const ini = path.join(temporaryDirectory, "ModOrganizer.ini");
  const original = await readFile(ini, "utf8");
  const helper = new HelperClient();
  try {
    await writeFile(ini, "[customExecutables]\n1\\title=Synthetic\n1\\binary=not-pwsh.exe\n1\\arguments=\n");
    const result = await helper.request(startRequest(47, 2000));
    assert.equal(result.ok, false);
    assert.equal(result.error.code, "INVALID_SHIM_CONFIG");
    await assert.rejects(readFile(path.join(temporaryDirectory, "mo2.pid")), { code: "ENOENT" });
    await assert.rejects(readFile(path.join(temporaryDirectory, "loader.pid")), { code: "ENOENT" });
  } finally {
    await writeFile(ini, original);
    await helper.dispose();
  }
});

test("EOF releases broker handles while the shim keeps MO2 and the synthetic game alive", async () => {
  await resetFixture();
  const existing = spawn(mo2Exe, ["--serve"], { stdio: "ignore", windowsHide: true });
  const existingExit = once(existing, "exit");
  await waitForFile(path.join(temporaryDirectory, "forward.pipe"));
  const helper = new HelperClient();
  const inspector = new HelperClient();
  try {
    const started = await helper.request(startRequest(50), 20_000);
    assert.equal(started.ok, true, JSON.stringify(started));
    const exit = await helper.closeInput();
    assert.equal(exit.code, 0);

    const inspected = await inspector.request({
      id: 51,
      op: "inspect",
      pid: started.result.pid,
      gameExe,
    });
    assert.equal(inspected.ok, true, JSON.stringify(inspected));
    assert.equal(inspected.result.running, true);

    const mo2Status = await inspector.request({
      id: 52, op: "inspect", pid: started.result.mo2Pid, gameExe: mo2Exe,
    });
    assert.equal(mo2Status.ok, true, JSON.stringify(mo2Status));
    assert.equal(mo2Status.result.running, true);

    process.kill(started.result.pid);
  } finally {
    await inspector.dispose();
    await helper.dispose();
    await stopMarkedSyntheticGame();
    if (existing.exitCode === null) existing.kill();
    await existingExit;
  }
});
