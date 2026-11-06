import type { NativePlatform } from "../platform.js";

export interface GameExit {
  pid: number;
  exitCode: number | null;
}

/**
 * Sticky record of one owned process's exit events. It subscribes at
 * construction, so an exit that precedes any wait is still observed.
 */
export class ExitTracker {
  readonly gameExited: Promise<GameExit>;
  readonly shimExited: Promise<void>;
  private resolveGameExited!: (exit: GameExit) => void;
  private markShimExited!: () => void;
  private exit: GameExit | undefined;
  private readonly listeners = new Set<(exit: GameExit) => void>();
  private readonly unsubscribe: () => void;

  constructor(
    private readonly platform: Pick<NativePlatform, "onEvent" | "status">,
    private readonly process: { pid: number },
  ) {
    this.gameExited = new Promise((resolve) => {
      this.resolveGameExited = resolve;
    });
    this.shimExited = new Promise((resolve) => {
      this.markShimExited = resolve;
    });
    this.unsubscribe = platform.onEvent((event) => {
      if (event.event === "gameExited" && event.pid === process.pid) {
        this.markGameExited({ pid: event.pid, exitCode: event.exitCode });
      } else if (event.event === "shimExited") {
        this.markShimExited();
      }
    });
  }

  /** Calls `listener` once the game has exited; returns the unsubscribe. */
  onGameExit(listener: (exit: GameExit) => void): () => void {
    const known = this.exit;
    if (known) {
      queueMicrotask(() => {
        listener(known);
      });
      return () => undefined;
    }
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /** Status check after subscribing, for an exit that beat the subscription. */
  async confirm(): Promise<void> {
    const status = await this.platform.status();
    if (!status.running) this.markGameExited({ pid: this.process.pid, exitCode: null });
  }

  /** Settles shimExited when the helper no longer tracks a live shim. */
  async confirmShim(): Promise<void> {
    const status = await this.platform.status();
    if (!status.owned || status.shimRunning === false) this.markShimExited();
  }

  dispose(): void {
    this.unsubscribe();
    this.listeners.clear();
  }

  private markGameExited(exit: GameExit): void {
    if (this.exit) return;
    this.exit = exit;
    this.resolveGameExited(exit);
    for (const listener of this.listeners) listener(exit);
  }
}
