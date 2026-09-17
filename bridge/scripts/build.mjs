import { cp, mkdir, rm } from "node:fs/promises";
import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

if (!process.argv.includes("--catalog-ready")) {
  await run("xmake", ["build", "devbench-catalog"], resolve(root, ".."));
  await run("xmake", [
    "run", "devbench-catalog", resolve(root, "src", "generated", "core-tools.json"),
  ], resolve(root, ".."));
}
await rm(resolve(root, "dist"), { recursive: true, force: true });
await run(process.execPath, [resolve(root, "scripts", "build-native-shim.mjs")]);
await run(process.execPath, [
  resolve(root, "node_modules", "typescript", "bin", "tsc"),
  "-p",
  root,
]);
await mkdir(resolve(root, "dist", "platform"), { recursive: true });
await cp(
  resolve(root, "platform"),
  resolve(root, "dist", "platform"),
  { recursive: true },
);
await cp(
  resolve(root, "build", "native-shim", "devbench-launch.exe"),
  resolve(root, "dist", "platform", "devbench-launch.exe"),
);

function run(command, args, cwd = root) {
  return new Promise((resolvePromise, reject) => {
    const env = { ...process.env };
    delete env.FO4_DEV_MODS;
    delete env.XSE_FO4_MODS_PATH;
    delete env.XSE_FO4_GAME_PATH;
    delete env.SkyrimPluginTargets;
    delete env.XSE_TES5_MODS_PATH;
    delete env.XSE_TES5_GAME_PATH;
    const child = spawn(command, args, { cwd, env, stdio: "inherit" });
    child.once("error", reject);
    child.once("exit", (code) => {
      if (code === 0) resolvePromise();
      else reject(new Error(`${command} exited with code ${String(code)}`));
    });
  });
}
