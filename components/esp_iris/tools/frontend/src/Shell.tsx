import type { Device } from "./types";

export type Page = "workspace" | "overview" | "records" | "settings" | "docs";

type HeaderProps = {
  page: Page;
  setPage: (value: Page) => void;
  mode: "develop" | "observe";
  transitioning: boolean;
  setMode: (value: "develop" | "observe") => Promise<void>;
  demo: boolean;
  language: "zh" | "en";
  setLanguage: (value: "zh" | "en") => void;
};

const labels = {
  zh: { workspace: "设备工作区", overview: "设备概览", records: "操作记录", settings: "系统设置", docs: "API 文档", develop: "开发模式", observe: "观察模式" },
  en: { workspace: "Workspace", overview: "Overview", records: "Operations", settings: "Settings", docs: "API Docs", develop: "Develop", observe: "Observe" },
};

export function Header({ page, setPage, mode, transitioning, setMode, demo, language, setLanguage }: HeaderProps) {
  const t = labels[language];
  return (
    <header className="app-header">
      <div className="brand-lockup"><span className="brand-mark small">IR</span><span>ESP-IRIS</span><em>DEVELOPER GATEWAY</em></div>
      <nav aria-label="主导航">
        {(["workspace", "overview", "records", "settings", "docs"] as Page[]).map((item) => (
          <button key={item} className={page === item ? "active" : ""} onClick={() => setPage(item)}>{t[item]}</button>
        ))}
      </nav>
      <div className="header-actions">
        {demo && <span className="demo-badge">DEMO</span>}
        <button className={`mode-switch ${mode}`} disabled={transitioning} onClick={() => setMode(mode === "develop" ? "observe" : "develop") }>
          <span className="status-dot" /> {transitioning ? "切换中…" : t[mode]}
        </button>
        <button className="language-button" onClick={() => setLanguage(language === "zh" ? "en" : "zh")}>{language === "zh" ? "EN" : "中"}</button>
      </div>
    </header>
  );
}

type RailProps = {
  devices: Device[];
  selectedId: string;
  onSelect: (deviceId: string) => void;
};

export function DeviceRail({ devices, selectedId, onSelect }: RailProps) {
  return (
    <aside className="device-rail">
      <div className="rail-heading"><span>设备</span><small>{devices.filter((device) => device.connected).length}/{devices.length}</small></div>
      <div className="rail-search"><span>⌕</span><input aria-label="筛选设备" placeholder="ID / 别名" /></div>
      <div className="device-list">
        {devices.map((device) => (
          <button key={device.device_id} className={`device-row ${selectedId === device.device_id ? "selected" : ""}`} onClick={() => onSelect(device.device_id)}>
            <span className={`device-icon ${device.firmware_mode === "recovery" ? "recovery" : ""}`}>{device.firmware_mode === "recovery" ? "R" : "M"}</span>
            <span className="device-copy">
              <strong>{device.alias || device.suggested_alias || device.device_id.slice(0, 12)}</strong>
              <small>{device.device_id}</small>
              <span><i className={`status-dot ${device.connected ? "online" : "offline"}`} />{device.connected ? device.firmware_mode === "recovery" ? "恢复固件" : "在线" : "离线"}</span>
            </span>
            <em>{device.app_version || "—"}</em>
          </button>
        ))}
        {!devices.length && <p className="empty-state">尚未发现 ESP-Iris 设备</p>}
      </div>
      <div className="rail-footer"><span>USB HIGHSPEED</span><i className="status-dot online" /></div>
    </aside>
  );
}
