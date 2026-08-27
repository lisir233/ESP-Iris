import { defineConfig } from "@playwright/test";
import { tmpdir } from "node:os";
import { join } from "node:path";

const externalBaseUrl = process.env.ESP_IRIS_TEST_URL;
const baseURL = externalBaseUrl || "http://127.0.0.1:8878";
const python = process.env.PYTHON || (process.platform === "win32" ? "python" : "python3");
const stateDir = join(tmpdir(), `esp-iris-playwright-${process.pid}`);

export default defineConfig({
  testDir: "./tests",
  timeout: 30_000,
  webServer: externalBaseUrl ? undefined : {
    command: `"${python}" ../esp_iris.py web --demo --listen 127.0.0.1 --port 8878 --state-dir "${stateDir}" --no-tls`,
    url: `${baseURL}/v1/health`,
    reuseExistingServer: false,
    timeout: 30_000,
  },
  use: {
    baseURL,
    viewport: { width: 1440, height: 900 },
    colorScheme: "light",
    locale: "zh-CN",
    timezoneId: "Asia/Shanghai",
  },
});
