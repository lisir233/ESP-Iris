import { useCallback, useEffect, useRef, useState } from "react";
import { api } from "./api";
import { appendGatewayEvent, nextReconnectDelay } from "./eventState";
import type { Audit, Device, DeviceStatus, GatewayEvent, Operation } from "./types";

type AuthState = { required: boolean; configured: boolean; authenticated: boolean; actor?: { type: string; name: string } };
type ModeState = { mode: "develop" | "observe"; transitioning: boolean };

export function useGateway() {
  const [auth, setAuth] = useState<AuthState | null>(null);
  const [mode, setModeState] = useState<ModeState>({ mode: "develop", transitioning: false });
  const [devices, setDevices] = useState<Device[]>([]);
  const [selectedId, setSelectedId] = useState<string>("");
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [operations, setOperations] = useState<Operation[]>([]);
  const [audits, setAudits] = useState<Audit[]>([]);
  const [events, setEvents] = useState<GatewayEvent[]>([]);
  const [demo, setDemo] = useState(false);
  const [error, setError] = useState<string>("");
  const cursor = useRef(0);

  const refreshAuth = useCallback(async () => {
    const value = await api<AuthState>("/v1/auth/state");
    setAuth(value);
    return value;
  }, []);

  const refresh = useCallback(async () => {
    try {
      const [modeData, deviceData, operationData, auditData] = await Promise.all([
        api<ModeState>("/v1/mode"),
        api<{ devices: Device[]; demo: boolean }>("/v1/devices"),
        api<{ operations: Operation[] }>("/v1/operations"),
        api<{ audits: Audit[] }>("/v1/system-audit"),
      ]);
      setModeState(modeData);
      setDevices(deviceData.devices);
      setDemo(deviceData.demo);
      setOperations(operationData.operations);
      setAudits(auditData.audits);
      setSelectedId((current) => current || deviceData.devices[0]?.device_id || "");
      setError("");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  }, []);

  const refreshStatus = useCallback(async () => {
    if (!selectedId) {
      setStatus(null);
      return;
    }
    try {
      setStatus(await api<DeviceStatus>(`/v1/devices/${encodeURIComponent(selectedId)}`));
    } catch (reason) {
      setStatus(null);
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  }, [selectedId]);

  useEffect(() => {
    refreshAuth().catch(() => setAuth({ required: true, configured: true, authenticated: false }));
  }, [refreshAuth]);

  useEffect(() => {
    if (!auth?.authenticated) return;
    refresh();
    const timer = window.setInterval(refresh, 3000);
    return () => window.clearInterval(timer);
  }, [auth?.authenticated, refresh]);

  useEffect(() => {
    if (!auth?.authenticated || !selectedId) return;
    refreshStatus();
    const timer = window.setInterval(refreshStatus, 2500);
    return () => window.clearInterval(timer);
  }, [auth?.authenticated, refreshStatus, selectedId]);

  useEffect(() => {
    if (!auth?.authenticated) return;
    let closed = false;
    let socket: WebSocket | null = null;
    let refreshTimer: number | null = null;
    let retry = 500;

    const connect = () => {
      const protocol = location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${location.host}/v1/events/ws?cursor=${cursor.current}`);
      socket.onopen = () => { retry = 500; };
      socket.onmessage = (message) => {
        const item = JSON.parse(message.data) as GatewayEvent;
        if (item.event_id) cursor.current = Math.max(cursor.current, item.event_id);
        setEvents((current) => appendGatewayEvent(current, item));
        if (item.category === "operation") {
          if (refreshTimer != null) window.clearTimeout(refreshTimer);
          refreshTimer = window.setTimeout(() => { refreshTimer = null; void refresh(); }, 100);
        }
      };
      socket.onclose = () => {
        if (!closed) {
          window.setTimeout(connect, retry);
          retry = nextReconnectDelay(retry);
        }
      };
    };
    connect();
    return () => {
      closed = true;
      if (refreshTimer != null) window.clearTimeout(refreshTimer);
      socket?.close();
    };
  }, [auth?.authenticated, refresh]);

  const setMode = useCallback(async (value: "develop" | "observe") => {
    setModeState((current) => ({ ...current, transitioning: true }));
    const result = await api<ModeState>("/v1/mode", { method: "PUT", body: JSON.stringify({ mode: value }), headers: { "Content-Type": "application/json" } });
    setModeState(result);
    await refresh();
  }, [refresh]);

  return {
    auth,
    setAuth,
    refreshAuth,
    mode,
    setMode,
    devices,
    selectedId,
    setSelectedId,
    status,
    operations,
    audits,
    events,
    demo,
    error,
    refresh,
    refreshStatus,
  };
}
