import { formatTime } from "./api";
import type { Device, GatewayEvent, Operation } from "./types";

type Props = { devices: Device[]; operations: Operation[]; events: GatewayEvent[]; onOpen: (deviceId: string) => void };

export default function Overview({ devices, operations, events, onOpen }: Props) {
  const connected = devices.filter((device) => device.connected).length;
  const recovery = devices.filter((device) => device.firmware_mode === "recovery").length;
  const running = operations.filter((operation) => !["succeeded", "failed", "cancelled", "interrupted", "outcome_unknown"].includes(operation.status)).length;
  const failed = operations.filter((operation) => ["failed", "outcome_unknown"].includes(operation.status)).length;
  return (
    <main className="content-page overview-page">
      <PageHeading eyebrow="MULTI-DEVICE / CURRENT STATE" title="设备概览" copy="当前连接、固件与操作状态；V1 不提供批量写入。" />
      <section className="overview-summary">
        <Summary label="已发现设备" value={String(devices.length)} detail={`${connected} 在线`} tone="cyan" />
        <Summary label="恢复固件" value={String(recovery)} detail="NORMAL / RECOVERY 统一历史" tone={recovery ? "amber" : "green"} />
        <Summary label="进行中操作" value={String(running)} detail="跨设备并行，单设备串行" tone={running ? "amber" : "cyan"} />
        <Summary label="失败 / 未知" value={String(failed)} detail="需要开发者检查" tone={failed ? "red" : "green"} />
      </section>
      <section className="table-pane">
        <div className="panel-title"><span>设备清单</span><small>LIVE + CACHED</small></div>
        <table>
          <thead><tr><th>连接</th><th>设备 / ID</th><th>固件模式</th><th>项目 / 版本</th><th>传输</th><th>Boot ID</th><th>最近行为</th><th /></tr></thead>
          <tbody>{devices.map((device) => {
            const operation = operations.find((item) => item.device_id === device.device_id);
            const event = [...events].reverse().find((item) => item.device_id === device.device_id);
            return <tr key={device.device_id}><td><span className={`connection-cell ${device.connected ? "online" : "offline"}`}><i className="status-dot" />{device.connected ? "在线" : "离线"}</span></td><td><strong>{device.alias || device.suggested_alias || "未命名设备"}</strong><code>{device.device_id}</code></td><td><span className={`firmware-pill ${device.firmware_mode}`}>{device.firmware_mode || "unknown"}</span></td><td><strong>{device.project_name || "—"}</strong><small>{device.app_version || "—"}</small></td><td>{device.transport_name || device.endpoint || "—"}</td><td className="mono-id">{device.boot_id ?? "—"}</td><td>{operation?.action || event?.connection_state || event?.event_name || "—"}<small>{formatTime(operation?.created_ns || event?.host_receive_ns)}</small></td><td><button onClick={() => onOpen(device.device_id)}>打开工作区 →</button></td></tr>;
          })}</tbody>
        </table>
      </section>
      <section className="capability-pane"><div className="panel-title"><span>能力矩阵</span><small>设备声明</small></div><div className="capability-grid">{["rpc", "jobs", "ota", "restart", "screen", "image", "audio", "input", "crash"].map((name) => <div key={name}><strong>{name.toUpperCase()}</strong><span>{devices.filter((device) => device.capability_names?.includes(name)).length}/{devices.length}</span></div>)}</div></section>
    </main>
  );
}

function PageHeading({ eyebrow, title, copy }: { eyebrow: string; title: string; copy: string }) {
  return <header className="page-heading"><div><p className="eyebrow">{eyebrow}</p><h1>{title}</h1><p>{copy}</p></div><span>最后刷新 {new Date().toLocaleTimeString([], { hour12: false })}</span></header>;
}

function Summary({ label, value, detail, tone }: { label: string; value: string; detail: string; tone: string }) {
  return <div className={`summary-cell ${tone}`}><span>{label}</span><strong>{value}</strong><small>{detail}</small></div>;
}

export { PageHeading };
