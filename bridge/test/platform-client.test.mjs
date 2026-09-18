import assert from "node:assert/strict";
import fs from "node:fs";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { syncBuiltinESMExports } from "node:module";
import { tmpdir } from "node:os";
import { delimiter, join } from "node:path";
import test from "node:test";

import { resolvePwshExecutable } from "../dist/platform.js";

test("resolves an explicit PowerShell 7 executable from PATH", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-pwsh-path-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const first = join(root, "missing");
  const second = join(root, "PowerShell");
  await mkdir(second, { recursive: true });
  const executable = join(second, "pwsh.exe");
  await writeFile(executable, "synthetic");
  const standard = join(root, "PowerShell", "7");
  await mkdir(standard);
  await writeFile(join(standard, "pwsh.exe"), "synthetic");

  assert.equal(
    resolvePwshExecutable({
      PATH: `${first}${delimiter}"${second}"${delimiter}${standard}`,
      ProgramFiles: root,
    }),
    executable,
  );
});

test("resolves a Store app-execution alias that stat cannot follow without accepting missing targets", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-pwsh-alias-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const aliasDirectory = join(root, "aliases");
  const packageDirectory = join(root, "package");
  const standardDirectory = join(root, "PowerShell", "7");
  await Promise.all(
    [aliasDirectory, packageDirectory, standardDirectory]
      .map((directory) => mkdir(directory, { recursive: true })),
  );
  const alias = join(aliasDirectory, "pwsh.exe");
  const target = join(packageDirectory, "pwsh.exe");
  const standard = join(standardDirectory, "pwsh.exe");
  await Promise.all([
    writeFile(alias, ""),
    writeFile(target, "synthetic"),
    writeFile(standard, "synthetic"),
  ]);

  const { statSync, readlinkSync } = fs;
  t.mock.method(fs, "statSync", (candidate, ...args) => {
    if (candidate === alias) {
      throw Object.assign(new Error("App-execution alias cannot be stat'ed"), {
        code: "EACCES",
      });
    }
    return statSync(candidate, ...args);
  });
  t.mock.method(fs, "readlinkSync", (candidate, ...args) =>
    candidate === alias ? target : readlinkSync(candidate, ...args));
  syncBuiltinESMExports();
  t.after(() => {
    t.mock.restoreAll();
    syncBuiltinESMExports();
  });

  const environment = { Path: aliasDirectory, ProgramFiles: root };
  assert.equal(resolvePwshExecutable(environment), alias);
  await rm(target);
  assert.equal(resolvePwshExecutable(environment), standard);
  await mkdir(target);
  assert.equal(resolvePwshExecutable(environment), standard);
});

test("resolves the standard PowerShell 7 installation without Windows PowerShell fallback", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-pwsh-programfiles-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const directory = join(root, "PowerShell", "7");
  await mkdir(directory, { recursive: true });
  const executable = join(directory, "pwsh.exe");
  await writeFile(executable, "synthetic");

  assert.equal(
    resolvePwshExecutable({ PATH: "", ProgramFiles: root }),
    executable,
  );
  assert.throws(
    () => resolvePwshExecutable({ PATH: "", ProgramFiles: join(root, "absent") }),
    /PowerShell 7 pwsh\.exe is required/,
  );
});
