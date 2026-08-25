export class ApiError extends Error {
  status: number;
  code: string;

  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

export async function api<T>(path: string, init?: RequestInit): Promise<T> {
  const headers = new Headers(init?.headers);
  if (init?.body && typeof init.body === "string" && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  const response = await fetch(path, { ...init, headers, credentials: "same-origin" });
  if (!response.ok) {
    const body = await response.json().catch(() => ({}));
    throw new ApiError(response.status, body?.error?.code ?? "http_error", body?.error?.message ?? response.statusText);
  }
  return (await response.json()) as T;
}

export function jsonBody(value: unknown): RequestInit {
  return { body: JSON.stringify(value), headers: { "Content-Type": "application/json" } };
}

export function formatBytes(value?: number): string {
  if (value == null) return "—";
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / 1024 / 1024).toFixed(1)} MiB`;
}

export function formatDuration(start?: number, end?: number): string {
  if (!start) return "—";
  const value = ((end ?? Date.now() * 1e6) - start) / 1e9;
  return value < 1 ? `${Math.max(value * 1000, 0).toFixed(0)} ms` : `${value.toFixed(2)} s`;
}

export function formatTime(ns?: number): string {
  if (!ns) return "—";
  return new Date(ns / 1e6).toLocaleTimeString([], { hour12: false, hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

export function formatDateTime(ns?: number): string {
  if (!ns) return "—";
  const value = new Date(ns / 1e6);
  const pad = (part: number) => String(part).padStart(2, "0");
  return `${value.getFullYear()}-${pad(value.getMonth() + 1)}-${pad(value.getDate())} ${pad(value.getHours())}:${pad(value.getMinutes())}:${pad(value.getSeconds())}`;
}
