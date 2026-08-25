import { describe, expect, it } from "vitest";
import { formatDateTime } from "./api";

describe("formatDateTime", () => {
  it("includes the local calendar date for cross-day operation details", () => {
    const beforeMidnight = new Date(2026, 7, 24, 23, 59, 55).getTime() * 1e6;
    const afterMidnight = new Date(2026, 7, 25, 0, 0, 5).getTime() * 1e6;

    expect(formatDateTime(beforeMidnight)).toBe("2026-08-24 23:59:55");
    expect(formatDateTime(afterMidnight)).toBe("2026-08-25 00:00:05");
    expect(formatDateTime()).toBe("—");
  });
});
