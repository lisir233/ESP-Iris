# ESP-Iris

[English](README.md)

ESP-Iris 是面向 ESP-IDF 设备的开发链路。设备侧组件通过 USB 或 TCP
提供日志、状态，以及可选的 RPC、媒体、崩溃证据、配对和 OTA 服务；Python
Developer Gateway 与 React Web Workbench 运行在 PC 上，因此 HTTP、WebSocket、
JSON、数据存储和界面渲染都不进入设备固件。

当产品需要浏览器工程界面，但不希望在量产固件中嵌入 Web 服务器或调试 UI
时，可以使用 ESP-Iris。

```text
ESP-IDF 应用                 开发者 PC
┌──────────────────┐        ┌─────────────────┐        ┌───────────────┐
│ ESP-Iris 设备组件 │ USB/TCP│ Python Gateway  │ HTTP/WS│ Web Workbench │
│                  ├────────┤ + 本地持久化     ├────────┤ 或 CLI/Agent  │
└──────────────────┘        └─────────────────┘        └───────────────┘
```

## 包含内容

| 层级 | 能力 |
| --- | --- |
| 设备组件 | 单个有界 worker、二进制协议、日志、状态、RPC/Job、媒体、崩溃证据、配对和 OTA |
| Developer Gateway | USB/TCP 发现、会话管理、认证、持久化操作、制品存储和 REST/WebSocket API |
| Web Workbench | 设备概览、日志、RPC、屏幕/输入、媒体、固件、操作记录和系统设置 |
| 命令行客户端 | 面向开发者与 Agent 的可脚本化控制和稳定 JSON 输出 |

默认配置只启动核心链路以及日志/状态能力。RPC 表、媒体缓冲区、配对和 OTA
均由 Kconfig 限制，并且只在配置或实际使用时启用。

## 环境与兼容性

| 范围 | 要求 |
| --- | --- |
| ESP-IDF | 5.5 或更高版本 |
| Raw TCP | 可移植的默认传输；Wi-Fi/Ethernet 和地址配置由应用负责 |
| Application USB CDC0 | ESP32-S31；ESP-Iris 独占 TinyUSB CDC0 |
| USB Serial/JTAG | 具有 `SOC_USB_SERIAL_JTAG_SUPPORTED` 的 target；串口不能同时作为控制台 |
| Gateway | Python 3.11 或更高版本；当前真实设备主要在 Linux 验证 |
| Workbench | 构建随组件发布的 React 源码需要当前 Node.js/npm 环境 |

每个固件只编译一种设备传输，运行时不能切换。

## 快速开始

### 1. 添加组件

在应用的 `main/idf_component.yml` 中添加：

```yaml
dependencies:
  lisir233/esp_iris: "^0.1.0"
```

新增或修改 managed dependency 后运行 `idf.py reconfigure`。

### 2. 选择传输

运行 `idf.py menuconfig`，进入：

```text
Component config > ESP-Iris device link > Device transport
```

- **Raw TCP** 默认监听 `19772` 端口。网络接口的创建和重连由应用负责。
- **USB CDC0** 独占应用 TinyUSB CDC 端口。该端口只承载 ESP-Iris 二进制协议，
  不是文本控制台或自动下载端口。
- **USB Serial/JTAG** 独占固定串口但保留 JTAG。Gateway 与下载/监视工具不能
  同时打开同一个串口端点。

具体限制参见[示例索引](examples/README.md)。

### 3. 启动 ESP-Iris

```c
#include "esp_iris.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
}
```

`esp_iris_start()` 是幂等的。`esp_iris_stop()` 会释放 worker、传输、VFS 和
stdio 所有权，之后可以再次启动。

### 4. 构建固件

```bash
idf.py build
```

建议先使用 [`minimal`](examples/minimal/README.md) 示例验证 TCP、USB CDC0
或 USB Serial/JTAG，再集成到产品中。

### 5. 启动 Developer Gateway

在源码仓库中执行：

```bash
ESP_IRIS_COMPONENT_DIR=components/esp_iris
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.txt"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

如果组件由 Component Manager 下载，将 `ESP_IRIS_COMPONENT_DIR` 设置为
`managed_components/lisir233__esp_iris`。Gateway 启动后打开
`http://127.0.0.1:8443/`。

无需硬件即可运行 Demo：

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

USB/TCP 选择、认证、TLS、CLI、数据保留和开发命令参见
[Gateway 与 Workbench 指南](tools/README.md)。

## 功能与资源行为

| 功能 | 默认行为 | 设备侧上限 |
| --- | --- | --- |
| 链路和状态 | 启用 | 一个 worker 和固定协议缓冲区 |
| stdout/stderr 日志 | 启用 | `CONFIG_ESP_IRIS_LOG_RING_BYTES` |
| RPC handler | 由应用注册 | `CONFIG_ESP_IRIS_MAX_RPC_HANDLERS` 和 `CONFIG_ESP_IRIS_RPC_BODY_BYTES` |
| 保留 Job | 由应用创建 | `CONFIG_ESP_IRIS_MAX_JOBS` |
| 屏幕/图像/音频 | 主机开始流传输前保持空闲 | 每个活动 channel 一个 `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` 缓冲区 |
| 崩溃证据 | 存在时只读提供 | 使用 `CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES` 分块 |
| TCP 配对 | 默认关闭 | 一个 NVS token 和 challenge-HMAC 状态 |
| OTA writer | 可配置 | 使用 `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES` 分块，不在 RAM 中缓存完整镜像 |

媒体 channel 使用 credit 和 latest-chunk 策略，慢速主机不会在设备侧产生无界队列。

## 安全边界

- USB 依赖物理访问，不执行链路配对。
- Raw TCP 配对默认关闭；开启后 token 保存在 NVS 中，链路通过随机 challenge
  证明持有 token，token 本身不在链路上传输。
- Gateway 默认允许 loopback 免登录；非 loopback 客户端需要开发者登录或
  命名 Agent Token。
- HTTP 会向本地网络暴露凭据和设备数据，只应在可信开发网络使用；其他环境
  应开启 Gateway TLS。
- Wi-Fi 密码、配对 token、TLS 私钥和 Agent Token 必须放在被忽略的本地配置
  或私有文件中。

## 示例

所有公开示例都随组件发布，位于 [`examples/`](examples/README.md)。

| 示例 | 传输 | 适用场景 |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP、USB CDC0 或 USB Serial/JTAG | 身份、生命周期、状态和日志 |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | 应用管理 Wi-Fi 和 DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC 配对和 token 配置 |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 | RPC handler 和可取消 Job |
| [`display_input`](examples/display_input/README.md) | USB CDC0 | 截图、屏幕镜像和指针输入 |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 | 合成图像和 PCM 音频流 |
| [`ota`](examples/ota/README.md) | USB CDC0 | Recovery-first/直接 OTA、验收和回滚 |

硬件内部 fixture 保留在 `test_apps/`，不进入发布归档。

## 文档导航

- [公共 C API](include/esp_iris.h)
- [协议常量](include/esp_iris_protocol.h)
- [Wire protocol v1](protocol/spec.md)
- [Golden vectors](protocol/golden_vectors.json)
- [Gateway 与 Workbench](tools/README.md)
- [示例索引](examples/README.md)
- [工程架构](https://github.com/lisir233/ESP-Iris/blob/master/docs/esp-iris-architecture.md)
- [变更记录](CHANGELOG.md)

## 版本与许可证

当前组件版本为 `0.1.0`，对应 Git tag `v0.1.0`。ESP-Iris 使用
[Apache-2.0](LICENSE) 许可证。
