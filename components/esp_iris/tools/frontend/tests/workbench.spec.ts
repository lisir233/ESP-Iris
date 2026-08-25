import { expect, test } from "@playwright/test";

test("authenticated demo workbench covers device and system views", async ({ page }) => {
  const errors: string[] = [];
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  await page.goto("/");
  await expect(page).toHaveTitle("ESP-Iris 开发者工作台");
  expect(new URL(page.url()).pathname).toBe("/");
  await expect.poll(() => page.evaluate(() => getComputedStyle(document.documentElement).colorScheme)).toBe("light");
  await page.getByLabel("开发口令").fill("dev-password");
  await page.getByRole("button", { name: "进入工作台" }).click();
  await expect(page.getByText("Mosaico Alpha").first()).toBeVisible();
  const observeMode = page.getByRole("button", { name: "观察模式", exact: true });
  if (await observeMode.isVisible()) {
    await observeMode.click();
    await expect(page.getByRole("button", { name: "开发模式", exact: true })).toBeVisible();
  }
  await page.getByRole("button", { name: /Mosaico Alpha/ }).click();
  await expect(page.getByText("USB Highspeed", { exact: false }).first()).toBeVisible();
  await expect(page.getByText("设备操作时间线")).toBeVisible();
  await expect(page.getByText("设备日志")).toBeVisible();

  await page.getByRole("button", { name: "启动镜像" }).click();
  await expect(page.getByRole("button", { name: "停止镜像" })).toBeVisible();
  const screenshot = page.getByRole("button", { name: "PNG 截图" });
  await expect(screenshot).toBeEnabled();
  await screenshot.click();
  await expect(page.getByText("PNG 截图已完成，实时镜像保持运行")).toBeVisible();
  await expect(page.getByRole("button", { name: "停止镜像" })).toBeVisible();
  await page.getByRole("button", { name: "停止镜像" }).click();
  await expect(page.getByRole("button", { name: "启动镜像" })).toBeVisible();
  const capturedImage = page.getByRole("img", { name: "设备实时画面" });
  await expect(capturedImage).toBeVisible();
  const imageBox = await capturedImage.boundingBox();
  const surfaceBox = await page.locator(".screen-surface").boundingBox();
  expect(imageBox?.width).toBeLessThanOrEqual(640);
  expect(imageBox?.height).toBeLessThanOrEqual(360);
  expect(surfaceBox?.height).toBeLessThanOrEqual(360);

  await page.evaluate(async () => {
    const response = await fetch("/v1/devices/demo-a1b2c3d4/mirror/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ channel: "screen", fps: 5, description: {} }),
    });
    if (!response.ok) throw new Error(`mirror pre-start failed: ${response.status}`);
  });
  await screenshot.click();
  await expect(page.getByText("PNG 截图已完成，实时镜像保持运行")).toBeVisible();
  await expect(page.getByRole("button", { name: "停止镜像" })).toBeVisible();
  await page.getByRole("button", { name: "停止镜像" }).click();

  await page.getByRole("button", { name: "RPC Catalog" }).click();
  await expect(page.getByRole("heading", { name: "调用 RPC Catalog" })).toBeVisible();
  await page.getByRole("button", { name: "确认执行" }).click();
  await expect(page.getByText("RPC 已完成")).toBeVisible();

  await page.getByRole("button", { name: "设备概览" }).click();
  await expect(page.getByRole("heading", { name: "设备概览" })).toBeVisible();
  await expect(page.getByText("能力矩阵")).toBeVisible();

  await page.getByRole("button", { name: "操作记录" }).click();
  await expect(page.getByRole("heading", { name: "操作记录" })).toBeVisible();
  await expect(page.getByText("只记录真实设备侧行为", { exact: false })).toBeVisible();

  await page.getByRole("button", { name: "系统设置" }).click();
  await expect(page.getByText("远程 Agent Token", { exact: true })).toBeVisible();
  await expect(page.getByText("系统审计", { exact: true })).toBeVisible();

  await page.getByRole("button", { name: "API 文档" }).click();
  await expect(page.getByRole("heading", { name: "API 文档与试验台" })).toBeVisible();
  await expect(page.getByText("UNIT TEST CONSOLE")).toBeVisible();

  await page.getByRole("button", { name: "设备工作区" }).click();
  await page.getByRole("button", { name: "开发模式" }).click();
  await expect(page.getByText("所有业务设备请求已停止", { exact: false })).toBeVisible();
  await expect(page.getByRole("button", { name: "RPC Catalog" })).toBeDisabled();
  await page.getByRole("button", { name: "EN", exact: true }).click();
  await expect(page.getByText("Device controls", { exact: true })).toBeVisible();
  await expect(page.getByText("Device operation timeline", { exact: true })).toBeVisible();
  await expect(page.getByText("Device logs", { exact: true })).toBeVisible();
  const untranslated = new Set<string>();
  for (const destination of ["Overview", "Operations", "Settings", "API Docs", "Workspace"]) {
    await page.getByRole("button", { name: destination, exact: true }).click();
    const values = await page.locator("body").evaluate((body) => {
      const found = new Set<string>();
      const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT);
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        const parent = node.parentElement;
        const value = node.nodeValue?.trim() || "";
        if (/[\u3400-\u9fff]/u.test(value) && !parent?.closest(".language-button, .log-body, pre, code, [data-no-translate]")) found.add(value);
      }
      for (const element of body.querySelectorAll("[aria-label], [placeholder], [alt], [title]")) {
        if (element.closest(".language-button, .log-body, pre, code, [data-no-translate]")) continue;
        for (const name of ["aria-label", "placeholder", "alt", "title"]) {
          const value = element.getAttribute(name) || "";
          if (/[\u3400-\u9fff]/u.test(value)) found.add(`${name}=${value}`);
        }
      }
      return [...found];
    });
    for (const value of values) untranslated.add(`${destination}: ${value}`);
  }
  expect([...untranslated]).toEqual([]);
  await page.screenshot({ path: process.env.ESP_IRIS_SCREENSHOT || "/tmp/esp-iris-workbench.png", fullPage: true });
  expect(errors).toEqual([]);
});
