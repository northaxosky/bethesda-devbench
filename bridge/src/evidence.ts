import {
  mkdir,
  open,
  readdir,
  rename,
  stat,
  writeFile,
} from "node:fs/promises";
import { basename, join } from "node:path";
import { createHash } from "node:crypto";

import type { SessionConfig, SessionLogSpec } from "./config.js";
import { asJsonValue, errorMessage, type JsonObject, type JsonValue } from "./json.js";

interface FileSnapshot {
  path: string;
  size: number;
  mtimeMs: number;
  identity: string;
  prefixBytes: number;
  prefixHash: string;
  spec: SessionLogSpec;
}

export interface EvidenceSnapshot {
  startedAt: string;
  files: Map<string, FileSnapshot>;
  unavailable: JsonObject[];
}

export interface CollectedEvidence {
  directory: string;
  logs: JsonObject[];
  unavailable: JsonObject[];
  resultPath: string;
}

export async function snapshotEvidence(
  config: SessionConfig,
): Promise<EvidenceSnapshot> {
  const files = new Map<string, FileSnapshot>();
  const unavailable: JsonObject[] = [];
  for (const spec of config.logs) {
    try {
      for (const path of await expandSpec(spec)) {
        try {
          const value = await stat(path);
          if (!value.isFile()) continue;
          files.set(path.toLowerCase(), {
            path,
            size: value.size,
            mtimeMs: value.mtimeMs,
            identity: fileIdentity(value),
            prefixBytes: Math.min(value.size, 4096),
            prefixHash: await hashPrefix(path, Math.min(value.size, 4096)),
            spec,
          });
        } catch (error) {
          unavailable.push({
            source: path,
            reason: errorMessage(error),
          });
        }
      }
    } catch (error) {
      unavailable.push({
        source: spec.path,
        reason: errorMessage(error),
      });
    }
  }
  return { startedAt: new Date().toISOString(), files, unavailable };
}

export async function collectEvidence(
  config: SessionConfig,
  runId: string,
  snapshot: EvidenceSnapshot,
  result: JsonObject,
): Promise<CollectedEvidence> {
  const safeRunId = runId.replace(/[^a-zA-Z0-9_.-]/g, "_");
  const directory = join(
    config.resultsDir,
    `${new Date().toISOString().replace(/[:.]/g, "-")}-${safeRunId}`,
  );
  await mkdir(directory, { recursive: true });
  const logs: JsonObject[] = [];
  const unavailable = [...snapshot.unavailable];
  let index = 0;

  for (const spec of config.logs) {
    let paths: string[];
    try {
      paths = await expandSpec(spec);
    } catch (error) {
      unavailable.push({ source: spec.path, reason: errorMessage(error) });
      continue;
    }
    for (const path of paths) {
      try {
        const current = await stat(path);
        if (!current.isFile()) continue;
        const before = snapshot.files.get(path.toLowerCase());
        const identity = fileIdentity(current);
        const fresh =
          before === undefined ||
          before.identity !== identity ||
          before.size !== current.size ||
          before.mtimeMs !== current.mtimeMs;
        if (!fresh) continue;

        if (/\.(rdc|dmp|pdb|dll|exe)$/i.test(path)) {
          logs.push({
            source: path,
            referenced: true,
            bytes: current.size,
            reason: "binary evidence is referenced in place, never copied into run logs",
          });
          continue;
        }
        const append = before !== undefined && before.identity === identity &&
          current.size >= before.size &&
          await hashPrefix(path, before.prefixBytes) === before.prefixHash;
        const start = append ? before.size : 0;
        const available = Math.max(0, current.size - start);
        const bytes = Math.min(available, spec.maxBytes);
        const outputName = `${String(++index).padStart(2, "0")}-${basename(path)}`;
        const outputPath = join(directory, outputName);
        const handle = await open(path, "r");
        let captured = 0;
        try {
          const buffer = Buffer.alloc(bytes);
          while (captured < bytes) {
            const read = await handle.read(buffer, captured, bytes - captured, start + captured);
            if (read.bytesRead === 0) break;
            captured += read.bytesRead;
          }
          await writeFile(outputPath, buffer.subarray(0, captured));
        } finally {
          await handle.close();
        }
        logs.push({
          source: path,
          captured: outputPath,
          startOffset: start,
          capturedBytes: captured,
          availableBytes: available,
          truncated: captured < available,
          replacedOrTruncated:
            before !== undefined &&
            !append,
          mtime: current.mtime.toISOString(),
        });
      } catch (error) {
        unavailable.push({ source: path, reason: errorMessage(error) });
      }
    }
  }
  if (logs.length === 0) {
    unavailable.push({
      source: "configured logs",
      reason: "no configured log changed during this run",
    });
  }

  const resultPath = join(directory, "result.json");
  const payload = {
    ...result,
    evidence: {
      directory,
      startedAt: snapshot.startedAt,
      logs,
      unavailable,
    },
  };
  await atomicJson(resultPath, payload);
  return { directory, logs, unavailable, resultPath };
}

async function hashPrefix(path: string, count: number): Promise<string> {
  const handle = await open(path, "r");
  try {
    const data = Buffer.alloc(count);
    let length = 0;
    while (length < count) {
      const read = await handle.read(data, length, count - length, length);
      if (read.bytesRead === 0) break;
      length += read.bytesRead;
    }
    return createHash("sha256").update(data.subarray(0, length)).digest("hex");
  } finally {
    await handle.close();
  }
}

async function expandSpec(spec: SessionLogSpec): Promise<string[]> {
  if (!spec.pattern) return [spec.path];
  const entries = await readdir(spec.path, { withFileTypes: true });
  const expression = wildcardExpression(spec.pattern);
  return entries
    .filter((entry) => entry.isFile() && expression.test(entry.name))
    .map((entry) => join(spec.path, entry.name));
}

function wildcardExpression(pattern: string): RegExp {
  const escaped = pattern.replace(/[.+^${}()|[\]\\]/g, "\\$&");
  return new RegExp(
    `^${escaped.replaceAll("*", ".*").replaceAll("?", ".")}$`,
    "i",
  );
}

function fileIdentity(value: {
  dev: number | bigint;
  ino: number | bigint;
}): string {
  return `${String(value.dev)}:${String(value.ino)}`;
}

async function atomicJson(path: string, value: JsonValue | JsonObject): Promise<void> {
  const temporary = `${path}.tmp-${String(process.pid)}-${String(Date.now())}`;
  await writeFile(temporary, `${JSON.stringify(asJsonValue(value), null, 2)}\n`);
  await rename(temporary, path);
}
