import { isObject, type JsonObject, type JsonValue } from "../json.js";
import {
  eventOccurred,
  menuState,
  operationTerminal,
  playerReady,
  scenarioFinished,
  type WaitCondition,
} from "./conditions.js";
import type { WaitFailure } from "./types.js";

/** The `await` argument of a proxied mutation. */
export type AwaitArgument =
  | boolean
  | { event: string; correlate: string };

export type Outcome =
  | { condition: WaitCondition }
  | { failure: WaitFailure };

interface OutcomeRule {
  when(args: JsonObject): boolean;
  // Undefined when the receipt gives nothing to correlate against.
  condition(args: JsonObject, receipt: JsonObject): WaitCondition | undefined;
}

const isOperation = (receipt: JsonObject): receipt is JsonObject & { operationId: number } =>
  receipt.queued === true && Number.isSafeInteger(receipt.operationId);

/** Which proxied mutations await what by default, keyed by tool name. */
const OUTCOMES: Readonly<Record<string, readonly OutcomeRule[]>> = {
  game: [
    {
      when: (args) => args.action === "load",
      condition: (_args, receipt) =>
        isOperation(receipt)
          ? playerReady({
              operationId: receipt.operationId,
              ...(Number.isSafeInteger(receipt.actionCursor)
                ? { actionCursor: receipt.actionCursor as number }
                : {}),
            })
          : undefined,
    },
    {
      when: (args) => args.action === "save",
      condition: (_args, receipt) =>
        isOperation(receipt) ? operationTerminal(receipt.operationId) : undefined,
    },
  ],
  menu: [
    {
      when: (args) => args.action === "open" || args.action === "close",
      condition: (args) =>
        typeof args.name === "string"
          ? menuState({ name: args.name, open: args.action === "open" })
          : undefined,
    },
  ],
  scenario: [
    {
      when: (args) => args.action === "run" && args.async === true,
      condition: (_args, receipt) =>
        Number.isSafeInteger(receipt.runId)
          ? scenarioFinished(receipt.runId as number)
          : undefined,
    },
  ],
};

export const AWAIT_SCHEMA = {
  description:
    "Block until this call's real outcome and return it as `outcome`; no polling needed. " +
    "Default (true) for game load/save, menu open/close, and async scenario run. " +
    "false returns the bare receipt. {event, correlate} instead waits for the named event " +
    "whose data[correlate] equals the receipt's [correlate] (for example " +
    "{event:\"communityshaders.capture.completed\", correlate:\"requestId\"}). " +
    "Failures (game exit, crash, stall, modal, load failure) end the wait at once.",
  anyOf: [
    { type: "boolean" },
    {
      type: "object",
      additionalProperties: false,
      required: ["event", "correlate"],
      properties: { event: { type: "string" }, correlate: { type: "string" } },
    },
  ],
} as const satisfies JsonValue;

/** Splits `await` off the arguments forwarded to the game. */
export function splitAwait(args: JsonObject): {
  forwarded: JsonObject;
  awaitArg: AwaitArgument | undefined;
} {
  if (!("await" in args)) return { forwarded: args, awaitArg: undefined };
  const { await: value, ...forwarded } = args;
  if (typeof value === "boolean") return { forwarded, awaitArg: value };
  if (
    isObject(value) &&
    typeof value.event === "string" &&
    value.event.length > 0 &&
    typeof value.correlate === "string" &&
    value.correlate.length > 0
  ) {
    return { forwarded, awaitArg: { event: value.event, correlate: value.correlate } };
  }
  throw new Error("await must be a boolean or {event, correlate} strings");
}

/** Whether the call awaits anything; decided before dispatch to take the cursor. */
export function awaitsOutcome(
  tool: string,
  args: JsonObject,
  awaitArg: AwaitArgument | undefined,
): boolean {
  if (awaitArg === false) return false;
  if (typeof awaitArg === "object") return true;
  return OUTCOMES[tool]?.some((rule) => rule.when(args)) === true;
}

/** The condition (or uncorrelated failure) for a dispatched call's receipt. */
export function outcomeFor(
  tool: string,
  args: JsonObject,
  awaitArg: AwaitArgument | undefined,
  receipt: JsonObject,
): Outcome | undefined {
  if (typeof awaitArg === "object") {
    const id = receipt[awaitArg.correlate];
    if (id === undefined) {
      return {
        failure: {
          reason: "uncorrelated",
          detail: `the receipt has no '${awaitArg.correlate}' to correlate ${awaitArg.event} against`,
        },
      };
    }
    return {
      condition: eventOccurred({ topic: awaitArg.event, match: { [awaitArg.correlate]: id } }),
    };
  }
  const condition = OUTCOMES[tool]?.find((rule) => rule.when(args))?.condition(args, receipt);
  return condition ? { condition } : undefined;
}
