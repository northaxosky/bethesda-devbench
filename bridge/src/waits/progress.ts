import type { DevBenchEvent } from "../remote.js";
import type { ProgressSink } from "./types.js";

export interface ProgressContext {
  _meta?: { progressToken?: string | number | undefined } | undefined;
  sendNotification(notification: {
    method: "notifications/progress";
    params: { progressToken: string | number; progress: number; message: string };
  }): Promise<void>;
}

export function createProgressSink(context: ProgressContext): ProgressSink | undefined {
  const token = context._meta?.progressToken;
  if (token === undefined) return undefined;
  let progress = 0;
  return (message) => {
    // A dropped progress line must never fail the wait it describes.
    context
      .sendNotification({
        method: "notifications/progress",
        params: { progressToken: token, progress: ++progress, message },
      })
      .catch(() => undefined);
  };
}

export function describeEvent(event: DevBenchEvent): string {
  const { topic, data } = event;
  if (topic === "lifecycle" && typeof data.event === "string") return `lifecycle ${data.event}`;
  if (topic === "menu" && typeof data.name === "string") {
    return `menu ${data.name} ${data.opening === false ? "closed" : "opened"}`;
  }
  if (topic === "health.stalled" && typeof data.stalledForMs === "number") {
    const seconds = Math.round(data.stalledForMs / 1000);
    return `stalled ${String(seconds)} s${data.foreground === false ? " (background)" : ""}`;
  }
  return topic;
}
