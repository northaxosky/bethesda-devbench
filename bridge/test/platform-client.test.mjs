import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
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

  assert.equal(
    resolvePwshExecutable({
      PATH: `${first}${delimiter}${second}`,
    }),
    executable,
  );
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
