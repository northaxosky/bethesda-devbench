import { existsSync, watch, type FSWatcher } from "node:fs";
import { open, stat } from "node:fs/promises";
import { basename, dirname } from "node:path";

export interface WatchTarget {
  dir: string;
  accept?: (name: string) => boolean;
}

const MAX_ANCESTOR_CLIMB = 4;

/**
 * Resolves with the first defined `probe()` result. The probe runs once after
 * the directory watchers are armed and again after every matching change, so
 * no change can fall between the check and the watch. A rejected probe or a
 * watcher error rejects the wait.
 */
export function awaitFileProbe<T>(options: {
  targets: readonly WatchTarget[];
  probe: () => Promise<T | undefined>;
  signal: AbortSignal;
  recheckMs?: number;
}): Promise<T> {
  const { targets, probe, signal, recheckMs } = options;
  return new Promise<T>((resolve, reject) => {
    const watchers: FSWatcher[] = [];
    let recheck: NodeJS.Timeout | undefined;
    let settled = false;
    let running = false;
    let dirty = false;

    const finish = (settle: () => void): void => {
      if (settled) return;
      settled = true;
      signal.removeEventListener("abort", onAbort);
      for (const watcher of watchers) watcher.close();
      clearInterval(recheck);
      settle();
    };
    const onAbort = (): void => {
      finish(() => reject(abortReason(signal)));
    };
    const run = async (): Promise<void> => {
      if (running) {
        dirty = true;
        return;
      }
      running = true;
      try {
        do {
          dirty = false;
          const value = await probe();
          if (value !== undefined) {
            finish(() => resolve(value));
            return;
          }
        } while (dirty && !settled);
      } catch (error) {
        finish(() => reject(asError(error)));
      } finally {
        running = false;
      }
    };

    if (signal.aborted) {
      onAbort();
      return;
    }
    signal.addEventListener("abort", onAbort, { once: true });
    try {
      for (const target of targets) {
        watchers.push(watchTarget(target, () => void run(), (error) => {
          finish(() => reject(error));
        }));
      }
    } catch (error) {
      finish(() => reject(asError(error)));
      return;
    }
    if (recheckMs !== undefined) recheck = setInterval(() => void run(), recheckMs);
    void run();
  });
}

// A missing directory is watched through its nearest existing ancestor.
function watchTarget(
  target: WatchTarget,
  onChange: () => void,
  onError: (error: Error) => void,
): FSWatcher {
  let dir = target.dir;
  let accept = target.accept;
  let recursive = false;
  for (let climbed = 0; !existsSync(dir); climbed++) {
    if (climbed >= MAX_ANCESTOR_CLIMB) {
      throw new Error(`cannot watch ${target.dir}: no existing ancestor nearby`);
    }
    dir = dirname(dir);
    recursive = true;
    accept = undefined;
  }
  const watcher = watch(dir, { persistent: false, recursive }, (_event, name) => {
    if (!accept || name === null || accept(name.toString())) onChange();
  });
  watcher.on("error", onError);
  return watcher;
}

/** Watch target for one file, accepted by case-insensitive base name. */
export function fileTarget(path: string): WatchTarget {
  const name = basename(path).toLowerCase();
  return { dir: dirname(path), accept: (changed) => changed.toLowerCase() === name };
}

export async function fileSize(path: string): Promise<number> {
  try {
    return (await stat(path)).size;
  } catch {
    return 0;
  }
}

export interface MarkerSearch {
  path: string;
  match: string;
  // Only bytes appended past this offset count.
  offset: number;
}

// Windows defers change notifications for a file another process keeps open
// (MO2 holds its log), so appended lines are also re-read on this interval.
const HELD_OPEN_RECHECK_MS = 250;

/** Resolves with the first line containing `match` appended after `offset`. */
export function awaitMarkerLine(
  search: MarkerSearch,
  signal: AbortSignal,
): Promise<string> {
  let position = search.offset;
  let carry = "";
  return awaitFileProbe({
    targets: [fileTarget(search.path)],
    signal,
    recheckMs: HELD_OPEN_RECHECK_MS,
    probe: async () => {
      let handle;
      try {
        handle = await open(search.path, "r");
      } catch {
        return undefined;
      }
      try {
        const { size } = await handle.stat();
        // A shrunken file was truncated or rotated; its content is new.
        if (size < position) {
          position = 0;
          carry = "";
        }
        if (size === position) return undefined;
        const buffer = Buffer.alloc(size - position);
        const { bytesRead } = await handle.read(buffer, 0, buffer.length, position);
        position += bytesRead;
        const lines = (carry + buffer.toString("utf8", 0, bytesRead)).split(/\r?\n/);
        carry = lines.pop() ?? "";
        return [...lines, carry].find((line) => line.includes(search.match));
      } finally {
        await handle.close();
      }
    },
  });
}

/** Abortable delay for the bounded startup-race retry only. */
export function abortableDelay(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(abortReason(signal));
      return;
    }
    const timer = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, ms);
    const onAbort = (): void => {
      clearTimeout(timer);
      reject(abortReason(signal));
    };
    signal.addEventListener("abort", onAbort, { once: true });
  });
}

function abortReason(signal: AbortSignal): Error {
  return signal.reason instanceof Error ? signal.reason : new Error(String(signal.reason));
}

function asError(error: unknown): Error {
  return error instanceof Error ? error : new Error(String(error));
}
