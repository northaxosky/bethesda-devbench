import { cp, mkdir, rm } from "node:fs/promises";
import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const outputDir = resolve(root, "standalone");
const bun = resolve(
  root,
  "node_modules",
  "@oven",
  "bun-windows-x64",
  "bin",
  "bun.exe",
);

await rm(outputDir, { recursive: true, force: true });
await mkdir(resolve(outputDir, "platform"), { recursive: true });
await run(process.execPath, [resolve(root, "scripts", "build-native-shim.mjs")]);
await run(bun, [
  "build",
  "--compile",
  "--outfile",
  resolve(outputDir, "devbench-bridge.exe"),
  resolve(root, "src", "index.ts"),
]);
await cp(
  resolve(root, "platform"),
  resolve(outputDir, "platform"),
  { recursive: true },
);
await cp(
  resolve(root, "build", "native-shim", "devbench-launch.exe"),
  resolve(outputDir, "platform", "devbench-launch.exe"),
);

function run(command, args) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, { cwd: root, stdio: "inherit" });
    child.once("error", reject);
    child.once("exit", (code) => {
      if (code === 0) resolvePromise();
      else reject(new Error(`${command} exited with code ${String(code)}`));
    });
  });
}
