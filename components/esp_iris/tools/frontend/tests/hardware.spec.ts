import { expect, test } from "@playwright/test";

const baseURL = process.env.ESP_IRIS_E2E_BASE_URL;
const deviceId = process.env.ESP_IRIS_E2E_DEVICE_ID;

test("real Gateway drives the hardware workbench", async ({ page }) => {
  test.skip(!baseURL || !deviceId, "requires the explicit local HIL runner");
  const browserErrors: string[] = [];
  const apiErrors: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") browserErrors.push(message.text());
  });
  page.on("response", async (response) => {
    const url = new URL(response.url());
    if (!url.pathname.startsWith("/v1/")) return;
    const contentType = response.headers()["content-type"] || "";
    if (!response.ok() || contentType.includes("text/html")) {
      apiErrors.push(`${response.status()} ${url.pathname} ${contentType}`);
    }
  });

  await page.goto(baseURL!);
  const password = page.getByLabel("开发口令");
  if (await password.isVisible().catch(() => false)) {
    await password.fill("iris-e2e-developer-password");
    await page.getByRole("button", { name: "进入工作台" }).click();
  }

  await expect(page.getByText(deviceId!.slice(0, 12)).first()).toBeVisible();
  await expect(page.getByText("已连接", { exact: true }).first()).toBeVisible();
  await page.getByText("更多操作", { exact: true }).click();
  await page.getByRole("button", { name: "原始 RPC", exact: true }).click();
  await page.getByLabel("Service ID").fill("1");
  await page.getByLabel("Method ID").fill("1");
  await page.getByLabel("原始载荷").fill("playwright-hardware");
  await page.getByRole("button", { name: "确认执行" }).click();
  await expect(page.getByText("原始 RPC 已完成", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "截图", exact: true }).click();
  await expect(page.getByAltText("设备实时画面")).toBeVisible();
  await page.getByRole("button", { name: "启动镜像", exact: true }).click();
  await expect(page.getByText("镜像中", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "交互输入", exact: true }).click();
  const surface = page.locator(".screen-surface");
  const box = await surface.boundingBox();
  expect(box).not.toBeNull();
  await page.mouse.move(box!.x + 10, box!.y + 10);
  await page.mouse.down();
  await page.mouse.move(box!.x + 30, box!.y + 30, { steps: 3 });
  await page.mouse.up();
  await page.getByRole("button", { name: "停止镜像", exact: true }).click();

  await page.getByRole("button", { name: "文件", exact: true }).click();
  await expect(page.getByText("README.txt", { exact: true })).toBeVisible();
  const uploadName = `hardware-playwright-${Date.now()}.txt`;
  await page.getByLabel("选择上传文件").setInputFiles({
    name: uploadName,
    mimeType: "text/plain",
    buffer: Buffer.from("real ESP-Iris workbench upload\n"),
  });
  await expect(page.getByText(`已上传 ${uploadName}`, { exact: true })).toBeVisible();
  const uploadedFile = page.getByRole("row").filter({ hasText: uploadName });
  page.once("dialog", (dialog) => void dialog.accept());
  await uploadedFile.getByRole("button", { name: "删除", exact: true }).click();
  await expect(page.getByText(`已删除 ${uploadName}`, { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "记录", exact: true }).click();
  await expect(page.getByRole("tab", { name: "设备操作" })).toBeVisible();
  await page.getByRole("button", { name: "设置", exact: true }).click();
  await expect(page.getByText("Agent Token", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "API 文档", exact: true }).click();
  await expect(page.getByText("/v1/health", { exact: true })).toBeVisible();

  const screenshot = process.env.ESP_IRIS_E2E_SCREENSHOT;
  if (screenshot) await page.screenshot({ path: screenshot, fullPage: true });
  expect(apiErrors).toEqual([]);
  expect(browserErrors).toEqual([]);
});
