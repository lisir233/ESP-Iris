import { useMemo, useState } from "react";
import { formatDateTime, formatDuration, formatTime } from "./api";
import PageHeading from "./PageHeading";
import { actionLabel } from "./Workspace";
import type { Audit, Device, Operation } from "./types";

type Props = { operations: Operation[]; audits: Audit[]; devices: Device[]; initialDeviceId?: string };

export default function Records({ operations, audits, devices, initialDeviceId = "" }: Props) {
  const [kind, setKind] = useState<"operations" | "system">("operations");
  return <main className="content-page records-page">
    <PageHeading title="记录" copy="查看设备操作及网关系统事件。" />
    <div className="record-tabs" role="tablist">
      <button role="tab" aria-selected={kind === "operations"} className={kind === "operations" ? "active" : ""} onClick={() => setKind("operations")}>设备操作</button>
      <button role="tab" aria-selected={kind === "system"} className={kind === "system" ? "active" : ""} onClick={() => setKind("system")}>系统事件</button>
    </div>
    {kind === "operations" ? <OperationRecords operations={operations} devices={devices} initialDeviceId={initialDeviceId} /> : <SystemRecords audits={audits} />}
  </main>;
}

function OperationRecords({ operations, devices, initialDeviceId }: { operations: Operation[]; devices: Device[]; initialDeviceId: string }) {
  const [actor, setActor] = useState("all");
  const [status, setStatus] = useState("all");
  const [deviceId, setDeviceId] = useState(initialDeviceId || "all");
  const [search, setSearch] = useState("");
  const [selected, setSelected] = useState<Operation | null>(null);
  const filtered = useMemo(() => operations.filter((item) => (actor === "all" || item.actor_type === actor) && (status === "all" || item.status === status) && (deviceId === "all" || item.device_id === deviceId) && (!search || JSON.stringify(item).toLowerCase().includes(search.toLowerCase()))), [operations, actor, status, deviceId, search]);

  return <>
    <section className="records-toolbar">
      <select aria-label="设备" value={deviceId} onChange={(event) => setDeviceId(event.target.value)}><option value="all">全部设备</option>{devices.map((device) => <option key={device.device_id} value={device.device_id}>{deviceName(devices, device.device_id)}</option>)}</select>
      <select aria-label="操作者" value={actor} onChange={(event) => setActor(event.target.value)}><option value="all">全部操作者</option><option value="agent">Agent</option><option value="developer">Developer</option></select>
      <select aria-label="状态" value={status} onChange={(event) => setStatus(event.target.value)}><option value="all">全部状态</option>{["queued", "running", "waiting_device", "reconnecting", "succeeded", "failed", "cancelled", "interrupted", "outcome_unknown"].map((value) => <option key={value}>{value}</option>)}</select>
      <input aria-label="搜索设备操作" placeholder="搜索动作、参数或 ID" value={search} onChange={(event) => setSearch(event.target.value)} />
      <span>{filtered.length} 条</span>
    </section>
    <section className="records-layout">
      <div className="table-pane records-table"><table><thead><tr><th>时间</th><th>操作者</th><th>设备</th><th>动作</th><th>状态</th><th>耗时</th><th>队列</th></tr></thead><tbody>{filtered.map((item) => <tr key={item.operation_id} className={selected?.operation_id === item.operation_id ? "selected-row" : ""} onClick={() => setSelected(item)}><td>{formatTime(item.created_ns)}</td><td><span className={`actor ${item.actor_type}`}>{item.actor_type === "agent" ? "AGENT" : "DEV"}</span><small>{item.actor_name}</small></td><td><code>{deviceName(devices, item.device_id)}</code></td><td><strong>{actionLabel(item.action)}</strong><small>{item.action}</small></td><td><span className={`op-status ${item.status}`}>{item.status}</span></td><td>{formatDuration(item.started_ns, item.finished_ns)}</td><td>{item.queue_position || "—"}</td></tr>)}</tbody></table>{!filtered.length && <p className="empty-state">没有符合筛选条件的操作</p>}</div>
      <aside className="record-detail"><div className="panel-title"><span>操作详情</span></div>{selected ? <><dl><dt>Operation ID</dt><dd>{selected.operation_id}</dd><dt>设备 ID</dt><dd>{selected.device_id}</dd><dt>操作者</dt><dd>{selected.actor_type} / {selected.actor_name}</dd><dt>状态</dt><dd><span className={`op-status ${selected.status}`}>{selected.status}</span></dd><dt>创建时间</dt><dd>{formatDateTime(selected.created_ns)}</dd><dt>开始</dt><dd>{formatDateTime(selected.started_ns)}</dd><dt>结束</dt><dd>{formatDateTime(selected.finished_ns)}</dd><dt>耗时</dt><dd>{formatDuration(selected.started_ns, selected.finished_ns)}</dd></dl><h3>参数</h3><pre>{JSON.stringify(selected.params, null, 2)}</pre><h3>结果</h3><pre className={selected.error ? "error-text" : ""}>{selected.error || JSON.stringify(selected.result, null, 2) || "—"}</pre></> : <p className="empty-state">选择一条操作查看详情</p>}</aside>
    </section>
  </>;
}

function SystemRecords({ audits }: { audits: Audit[] }) {
  const [search, setSearch] = useState("");
  const [selected, setSelected] = useState<Audit | null>(null);
  const filtered = useMemo(() => audits.filter((audit) => !search || JSON.stringify(audit).toLowerCase().includes(search.toLowerCase())), [audits, search]);
  return <>
    <section className="records-toolbar system-toolbar"><input aria-label="搜索系统事件" placeholder="搜索操作者、动作或详情" value={search} onChange={(event) => setSearch(event.target.value)} /><span>{filtered.length} 条</span></section>
    <section className="records-layout"><div className="table-pane records-table"><table><thead><tr><th>时间</th><th>操作者</th><th>动作</th><th>详情</th></tr></thead><tbody>{filtered.map((audit) => <tr key={audit.audit_id} className={selected?.audit_id === audit.audit_id ? "selected-row" : ""} onClick={() => setSelected(audit)}><td>{formatTime(audit.created_ns)}</td><td><span className={`actor ${audit.actor_type}`}>{audit.actor_type}</span><small>{audit.actor_name}</small></td><td><strong>{audit.action}</strong></td><td><code>{JSON.stringify(audit.details)}</code></td></tr>)}</tbody></table>{!filtered.length && <p className="empty-state">没有符合条件的系统事件</p>}</div><aside className="record-detail"><div className="panel-title"><span>事件详情</span></div>{selected ? <><dl><dt>Audit ID</dt><dd>{selected.audit_id}</dd><dt>操作者</dt><dd>{selected.actor_type} / {selected.actor_name}</dd><dt>时间</dt><dd>{formatTime(selected.created_ns)}</dd><dt>动作</dt><dd>{selected.action}</dd></dl><h3>详情</h3><pre>{JSON.stringify(selected.details, null, 2)}</pre></> : <p className="empty-state">选择一条系统事件查看详情</p>}</aside></section>
  </>;
}

function deviceName(devices: Device[], id: string) {
  const item = devices.find((device) => device.device_id === id);
  return item?.alias || item?.suggested_alias || id.slice(0, 12);
}
