import { FormEvent, useState } from "react";
import { api } from "./api";

type Props = {
  configured: boolean;
  onAuthenticated: () => Promise<unknown>;
};

export default function Login({ configured, onAuthenticated }: Props) {
  const [password, setPassword] = useState("espressif");
  const [confirm, setConfirm] = useState("espressif");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  async function submit(event: FormEvent) {
    event.preventDefault();
    if (!configured && password !== confirm) {
      setError("两次口令不一致");
      return;
    }
    setBusy(true);
    setError("");
    try {
      await api(configured ? "/v1/auth/login" : "/v1/auth/setup", {
        method: "POST",
        body: JSON.stringify({ password }),
        headers: { "Content-Type": "application/json" },
      });
      await onAuthenticated();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="login-shell">
      <section className="login-panel">
        <div className="brand-mark" aria-hidden="true">IR</div>
        <p className="eyebrow">ESP-IRIS / LOCAL GATEWAY</p>
        <h1>开发者工作台</h1>
        <p className="muted">设备状态、Agent 操作、日志与恢复证据的唯一入口</p>
        <form onSubmit={submit}>
          <label>
            开发口令
            <input autoFocus autoComplete="current-password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} minLength={8} required />
          </label>
          {!configured && (
            <label>
              确认开发口令
              <input autoComplete="new-password" type="password" value={confirm} onChange={(event) => setConfirm(event.target.value)} minLength={8} required />
            </label>
          )}
          {error && <p className="form-error">{error}</p>}
          <button className="primary-button" disabled={busy}>{busy ? "连接中…" : configured ? "进入工作台" : "初始化网关"}</button>
        </form>
        <div className="login-foot"><span className="status-dot online" /> {location.protocol === "https:" ? "已启用 HTTPS · 局域网开发环境" : "HTTP · 仅限受信任局域网"}</div>
      </section>
    </main>
  );
}
