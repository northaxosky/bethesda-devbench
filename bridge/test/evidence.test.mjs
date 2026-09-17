import assert from "node:assert/strict";
import { appendFile, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { collectEvidence, snapshotEvidence } from "../dist/evidence.js";

test("log truncation followed by growth does not lose the new startup banner", async (t) => {
  const root = await mkdtemp(join(tmpdir(), "devbench-log-recreation-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const log = join(root, "devbench.log");
  const append = join(root, "append.log");
  const capture = join(root, "frame.rdc");
  const results = join(root, "results");
  await mkdir(results);
  await writeFile(log, "OLD-RUN\nfinished\n");
  await writeFile(append, "PREVIOUS\n");
  const config = {
    resultsDir: results,
    logs: [log, append, capture].map((path) => ({ path, maxBytes: 1024 })),
  };
  const before = await snapshotEvidence(config);
  const newText = "NEW-RUN\nnew startup identity\nlonger new log after truncation\n";
  await writeFile(log, newText);
  await appendFile(append, "NEW-RECEIPT\n");
  await writeFile(capture, Buffer.alloc(4096, 1));
  const evidence = await collectEvidence(config, "recreated", before, { ok: false });
  const recreated = evidence.logs.find((item) => item.source === log);
  assert.equal(recreated.startOffset, 0);
  assert.equal(recreated.replacedOrTruncated, true);
  assert.equal(await readFile(recreated.captured, "utf8"), newText);
  const appended = evidence.logs.find((item) => item.source === append);
  assert.equal(appended.startOffset, 9);
  assert.equal(await readFile(appended.captured, "utf8"), "NEW-RECEIPT\n");
  const binary = evidence.logs.find((item) => item.source === capture);
  assert.equal(binary.referenced, true);
  assert.equal(binary.captured, undefined);
});
