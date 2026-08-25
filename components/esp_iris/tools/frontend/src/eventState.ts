import type { GatewayEvent } from "./types";

export const MAX_RETAINED_EVENTS = 3000;

export function appendGatewayEvent(
  current: GatewayEvent[],
  item: GatewayEvent,
  limit = MAX_RETAINED_EVENTS,
): GatewayEvent[] {
  if (!Number.isInteger(limit) || limit < 1) throw new RangeError("event limit must be positive");
  return [...current.slice(-(limit - 1)), item];
}

export function nextReconnectDelay(current: number, maximum = 5000): number {
  if (current < 0 || maximum < 1) throw new RangeError("reconnect delays must be positive");
  return Math.min(Math.max(1, current * 2), maximum);
}
