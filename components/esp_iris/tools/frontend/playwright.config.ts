import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./tests",
  timeout: 30_000,
  use: {
    baseURL: process.env.ESP_IRIS_TEST_URL || "http://127.0.0.1:8878",
    viewport: { width: 1440, height: 900 },
    colorScheme: "light",
    locale: "zh-CN",
  },
});
