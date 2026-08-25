export type Device = {
  device_id: string;
  alias?: string;
  suggested_alias?: string;
  connected: boolean;
  cached?: boolean;
  firmware_mode?: "normal" | "recovery";
  app_version?: string;
  project_name?: string;
  endpoint?: string;
  transport_name?: string;
  boot_id?: number;
  capability_names?: string[];
  demo?: boolean;
};

export type DeviceStatus = Device & {
  stale: boolean;
  mode: "develop" | "observe";
  uptime_us?: number;
  free_internal?: number;
  min_free_internal?: number;
  heap_used?: number;
  heap_total?: number;
  log_dropped_bytes?: number;
  lifecycle_state?: string;
  clock_uncertainty_us?: number;
  queue?: { running: string[]; queued: string[] };
};

export type Operation = {
  operation_id: string;
  device_id: string;
  actor_type: "agent" | "developer" | string;
  actor_name: string;
  action: string;
  status: string;
  params: Record<string, unknown>;
  result?: unknown;
  error?: string;
  created_ns: number;
  started_ns?: number;
  finished_ns?: number;
  queue_position?: number;
  progress?: {
    stage: string;
    progress_permille: number;
    device_progress_permille?: number;
    job_id?: number;
    bytes_received?: number;
    bytes_total?: number;
    partition?: string;
    updated_ns?: number;
  } | null;
};

export type GatewayEvent = {
  event_id?: number;
  category?: string;
  kind?: string;
  device_id?: string;
  text?: string;
  host_receive_ns?: number;
  host_receive_wall_ns?: number;
  parsed?: { level?: string; stamp?: string; tag?: string; message?: string };
  operation?: Operation;
  connection_state?: string;
  event_name?: string;
};

export type Audit = {
  audit_id: number;
  actor_type: string;
  actor_name: string;
  action: string;
  details: Record<string, unknown>;
  created_ns: number;
};
