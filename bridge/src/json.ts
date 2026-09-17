export type JsonPrimitive = string | number | boolean | null;
export type JsonValue = JsonPrimitive | JsonObject | JsonValue[];
export interface JsonObject {
  [key: string]: JsonValue;
}

export function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function requireObject(
  value: unknown,
  context: string,
): Record<string, unknown> {
  if (!isObject(value)) throw new Error(`${context} must be a JSON object`);
  return value;
}

export function requireString(
  object: Record<string, unknown>,
  key: string,
  context: string,
): string {
  const value = object[key];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${context}.${key} must be a non-empty string`);
  }
  return value;
}

export function optionalString(
  object: Record<string, unknown>,
  key: string,
  context: string,
): string | undefined {
  const value = object[key];
  if (value === undefined) return undefined;
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${context}.${key} must be a non-empty string when present`);
  }
  return value;
}

export function requireInteger(
  object: Record<string, unknown>,
  key: string,
  context: string,
): number {
  const value = object[key];
  if (!Number.isSafeInteger(value)) {
    throw new Error(`${context}.${key} must be an integer`);
  }
  return value as number;
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export function asJsonValue(value: unknown): JsonValue {
  if (
    value === null ||
    typeof value === "string" ||
    typeof value === "boolean"
  ) {
    return value;
  }
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (Array.isArray(value)) return value.map(asJsonValue);
  if (isObject(value)) {
    return Object.fromEntries(
      Object.entries(value).map(([key, entry]) => [key, asJsonValue(entry)]),
    );
  }
  throw new Error("value is not JSON-compatible");
}
