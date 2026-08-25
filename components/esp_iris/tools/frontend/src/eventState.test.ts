import { describe, expect, it } from "vitest";
import { appendGatewayEvent, nextReconnectDelay } from "./eventState";
import type { GatewayEvent } from "./types";

describe("gateway event state", () => {
  it("retains the newest bounded history", () => {
    const events = [1, 2, 3].map((event_id) => ({ event_id } as GatewayEvent));
    expect(appendGatewayEvent(events, { event_id: 4 } as GatewayEvent, 3).map((item) => item.event_id)).toEqual([2, 3, 4]);
  });

  it("bounds reconnect backoff", () => {
    expect(nextReconnectDelay(500)).toBe(1000);
    expect(nextReconnectDelay(4000)).toBe(5000);
  });
});
