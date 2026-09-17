import { mkdir, rm } from "node:fs/promises";
import { spawn, spawnSync } from "node:child_process";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const bridgeDirectory = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const source = resolve(
  bridgeDirectory,
  "platform",
  "native-launch-shim.cpp",
);
const outputDirectory = resolve(bridgeDirectory, "build", "native-shim");
const temporaryDirectory = resolve(outputDirectory, "obj");
const executable = resolve(outputDirectory, "devbench-launch.exe");
const programDatabase = resolve(outputDirectory, "devbench-launch.pdb");
const visualStudioInstaller = resolve(
  process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
  "Microsoft Visual Studio",
  "Installer",
);

const vcvars = findVcVars();
await mkdir(temporaryDirectory, { recursive: true });
await Promise.all([
  rm(executable, { force: true }),
  rm(programDatabase, { force: true }),
]);

const compilerArguments = [
  "/nologo",
  "/std:c++20",
  "/EHsc",
  "/permissive-",
  "/utf-8",
  "/MT",
  "/Zi",
  "/O2",
  "/W4",
  "/WX",
  "/DUNICODE",
  "/D_UNICODE",
  `/Fo${join(temporaryDirectory, "native-launch-shim.obj")}`,
  `/Fd${join(temporaryDirectory, "native-launch-shim.pdb")}`,
  source,
  "/link",
  "/DEBUG:FULL",
  "/OPT:REF",
  "/OPT:ICF",
  `/OUT:${executable}`,
  `/PDB:${programDatabase}`,
];

await run(
  process.env.ComSpec ?? "cmd.exe",
  [
    "/d",
    "/c",
    `set "PATH=${visualStudioInstaller};%PATH%" && call "${vcvars}" >nul && cl ${compilerArguments.map(quoteCmdArgument).join(" ")}`,
  ],
);
await rm(temporaryDirectory, { recursive: true, force: true });

console.log(executable);
console.log(programDatabase);

function findVcVars() {
  const vswhere = join(visualStudioInstaller, "vswhere.exe");
  const query = spawnSync(
    vswhere,
    [
      "-latest",
      "-products",
      "*",
      "-requires",
      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
      "-property",
      "installationPath",
    ],
    { encoding: "utf8", windowsHide: true },
  );
  if (query.status === 0 && query.stdout.trim() !== "") {
    return join(
      query.stdout.trim(),
      "VC",
      "Auxiliary",
      "Build",
      "vcvars64.bat",
    );
  }

  const programFiles = process.env.ProgramFiles ?? "C:\\Program Files";
  return join(
    programFiles,
    "Microsoft Visual Studio",
    "18",
    "Community",
    "VC",
    "Auxiliary",
    "Build",
    "vcvars64.bat",
  );
}

function quoteCmdArgument(value) {
  return `"${value.replaceAll('"', '""')}"`;
}

function run(command, args) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, {
      cwd: bridgeDirectory,
      stdio: "inherit",
      windowsHide: true,
      windowsVerbatimArguments: true,
    });
    child.once("error", reject);
    child.once("exit", (code) => {
      if (code === 0) {
        resolvePromise();
      } else {
        reject(new Error(`${command} exited with code ${String(code)}`));
      }
    });
  });
}
