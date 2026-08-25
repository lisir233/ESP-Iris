import { useEffect, useMemo, useState } from "react";
import Docs from "./Docs";
import Login from "./Login";
import Overview from "./Overview";
import Records from "./Records";
import Settings from "./Settings";
import { DeviceRail, Header, type Page } from "./Shell";
import Workspace from "./Workspace";
import { useUiTranslation } from "./i18n";
import { useGateway } from "./useGateway";

export default function App() {
  const gateway = useGateway();
  const [page, setPageState] = useState<Page>(location.pathname === "/docs" ? "docs" : "workspace");
  const [language, setLanguage] = useState<"zh" | "en">((localStorage.getItem("esp-iris-language") as "zh" | "en") || "zh");
  const selected = useMemo(() => gateway.devices.find((device) => device.device_id === gateway.selectedId), [gateway.devices, gateway.selectedId]);
  useUiTranslation(language);

  function setPage(value: Page) {
    setPageState(value);
    history.replaceState(null, "", value === "docs" ? "/docs" : "/");
  }

  useEffect(() => { localStorage.setItem("esp-iris-language", language); document.documentElement.lang = language === "zh" ? "zh-CN" : "en"; }, [language]);

  if (!gateway.auth) return <div className="boot-screen"><span className="brand-mark">IR</span><p>连接本地网关…</p></div>;
  if (!gateway.auth.authenticated) return <Login configured={gateway.auth.configured} onAuthenticated={gateway.refreshAuth} />;

  return <div className="app-shell"><Header page={page} setPage={setPage} mode={gateway.mode.mode} transitioning={gateway.mode.transitioning} setMode={gateway.setMode} demo={gateway.demo} language={language} setLanguage={setLanguage} />{gateway.error && <div className="global-error">{gateway.error}</div>}{page === "workspace" ? <div className="workspace-shell"><DeviceRail devices={gateway.devices} selectedId={gateway.selectedId} onSelect={gateway.setSelectedId} /><Workspace device={selected} status={gateway.status} mode={gateway.mode.mode} operations={gateway.operations} events={gateway.events} refresh={gateway.refresh} /></div> : page === "overview" ? <Overview devices={gateway.devices} operations={gateway.operations} events={gateway.events} onOpen={(deviceId) => { gateway.setSelectedId(deviceId); setPage("workspace"); }} /> : page === "records" ? <Records operations={gateway.operations} devices={gateway.devices} /> : page === "settings" ? <Settings mode={gateway.mode.mode} transitioning={gateway.mode.transitioning} setMode={gateway.setMode} audits={gateway.audits} demo={gateway.demo} localAuthRequired={gateway.auth.required} /> : <Docs mode={gateway.mode.mode} />}</div>;
}
