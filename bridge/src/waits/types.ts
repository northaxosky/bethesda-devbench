import type { JsonObject, JsonValue } from "../json.js";

export type WaitFailureReason =
  | "exited"
  | "crashed"
  | "stalled"
  | "modal"
  | "loadFailed"
  | "operationFailed"
  | "actionsDisabled"
  | "eventHistoryLost"
  | "uncorrelated"
  | "unavailable"
  | "cancelled"
  | "ceiling";

export interface WaitFailure {
  reason: WaitFailureReason;
  detail: JsonValue;
}

export interface WaitSatisfied {
  ok: true;
  until: string;
  satisfied: JsonObject;
  cursor: number;
  elapsedMs: number;
}

export interface WaitFailed extends WaitFailure {
  ok: false;
  until: string;
  cursor: number;
  elapsedMs: number;
}

export type WaitResult = WaitSatisfied | WaitFailed;

export type ProgressSink = (message: string) => void;

export interface CallContext {
  signal?: AbortSignal | undefined;
  progress?: ProgressSink | undefined;
}
