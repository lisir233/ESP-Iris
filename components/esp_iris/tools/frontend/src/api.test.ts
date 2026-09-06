import { describe, expect, it } from "vitest";
import { formatBootId, formatDateTime, formatRecordJson } from "./api";

describe("formatDateTime", () => {
  it("includes the local calendar date for cross-day operation details", () => {
    const beforeMidnight = new Date(2026, 7, 24, 23, 59, 55).getTime() * 1e6;
    const afterMidnight = new Date(2026, 7, 25, 0, 0, 5).getTime() * 1e6;

    expect(formatDateTime(beforeMidnight)).toBe("2026-08-24 23:59:55");
    expect(formatDateTime(afterMidnight)).toBe("2026-08-25 00:00:05");
    expect(formatDateTime()).toBe("—");
  });
});


describe("Boot ID display", () => {
  it.each(["9007199254740993", "12238782771570883527", "18446744073709551615"])("preserves %s through JSON parsing", (exact) => {
    const device = JSON.parse(`{"boot_id":${exact},"boot_id_text":"${exact}"}`);
    expect(formatBootId(device)).toBe(exact);
    expect(JSON.parse(formatRecordJson({ verification: device })!).verification.boot_id).toBe(exact);
    expect(typeof device.boot_id).toBe("number");
  });
  it("keeps old API fallback and missing identity readable", () => {
    expect(formatBootId({ boot_id: 42 })).toBe("42");
    expect(formatBootId({})).toBe("—");
    expect(formatRecordJson({ boot_id: 42 })).toBe('{"boot_id":42}');
  });
});
