# ESP-Iris

[English](README.md)

ESP-Iris 减少嵌入式开发中不必要的编译和烧录时间，让设备的日志、
状态和操作结果更容易被开发者与 AI Agent 同时理解、控制和追踪。把
零散的串口调试，变成一个统一、结构化、可恢复的设备开发流程。

ESP-Iris 将调试和控制的 Web plane 移到 PC。每台 ESP32 只运行一条
有界二进制链路和一个 worker task，通过 USB 或 TCP 提供日志、状态，
以及可选的 RPC、媒体、崩溃证据、配对、OTA 和有界文件服务。设备侧不运行
HTTP server、WebSocket server、JSON parser、framebuffer mirror，也不进行
媒体规模的内存分配。

多台设备可以通过独立的 USB 或 TCP 端点同时连接到一个 Python Developer
Gateway。Gateway 通过稳定的 `device_id` 识别设备，汇聚并持久化设备集群事件，
再将它们同时分发给 React Web Workbench、命令行客户端和外部 Agent。

当产品需要浏览器工程界面，但不希望在量产固件中嵌入 Web 服务器或调试 UI
时，可以使用 ESP-Iris。

```text
ESP32 A -- USB CDC0 ---------\
ESP32 B -- USB Serial/JTAG ---+--> Python Gateway + 持久化 --+--> Web Workbench
ESP32 C -- TCP/Wi-Fi --------/                                  +--> CLI 客户端
ESP32 D -- TCP/Ethernet -----/                                  +--> 外部 Agent
```

每个固件镜像只选择一种设备传输，每台物理设备同时最多只有一个 Gateway
会话。这不会限制设备集群：一个 Gateway 可以并发监督多个设备端点。
Workbench、CLI 和 Agent 连接分别接收独立的事件流；修改设备的操作会按
设备串行化。

## 包含内容

| 层级 | 能力 |
| --- | --- |
| 设备组件 | 单个有界 worker、二进制协议、日志、状态、RPC/Job、媒体、崩溃证据、配对和 OTA |
| Developer Gateway | 并发 USB/TCP 端点监督、设备集群会话管理、认证、持久化操作、制品存储和 REST/WebSocket API |
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
| Gateway | Python 3.8 或更高版本；当前真实设备主要在 Linux 验证 |
| Workbench | 构建随组件发布的 React 源码需要当前 Node.js/npm 环境 |

每个固件可以编译一种或多种设备传输。启用多种传输时，物理连接按有界窗口
协商；首个返回合法 HELLO_ACK（需要 TCP 配对时还须认证通过）的传输独占
活动 session。仅打开端口不会取得所有权。其余传输暂停，winner 断开后全部
配置的传输重新等待。

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
Component config > ESP-Iris device link > Device transports
```

- **Raw TCP** 默认监听 `19772` 端口。网络接口的创建和重连由应用负责。
- **USB CDC0** 独占应用 TinyUSB CDC 端口。该端口只承载 ESP-Iris 二进制协议，
  不是文本控制台或自动下载端口。
- **USB Serial/JTAG** 独占固定串口但保留 JTAG。Gateway 与下载/监视工具不能
  同时打开同一个串口端点。

可以选择任意兼容组合。启用多种传输时，候选连接受可配置握手超时约束；没有
候选正在协商或活动时，`esp_iris_status_t` 的 transport 为 `NONE`。

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
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.lock"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

单个锁文件使用 Python 版本条件，pip 会为当前激活解释器自动选择已验证的依赖组。
解释器 major/minor 变化时应重新创建隔离环境。Gateway 运行时与所选 ESP-IDF
版本要求的 Python 相互独立；ESP-IDF 工具环境仍须使用该版本所要求的解释器。

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
| OTA writer | 可配置；默认允许跨项目更新 | 使用 `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES` 分块；`CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` 可要求与当前项目名匹配 |
| System Inventory | 产品注册只读 provider 后启用 | 返回系统保护区实际哈希和上一笔提交结果，不含任何写回调 |
| System Update | recovery 注册产品 backend 后才启用 | 签名 manifest、组件/manifest/签名/分块上限，且不提供通用 raw-Flash API |
| 文件服务 | 应用注册逻辑卷后才启用 | 一个文件任务、一个流和每块 `CONFIG_ESP_IRIS_FILE_CHUNK_BYTES` |

媒体 channel 使用 credit 和 latest-chunk 策略，慢速主机不会在设备侧产生无界队列。

应用必须在 `esp_iris_start()` 前只注册产品明确允许导出的目录：

```c
ESP_ERROR_CHECK(esp_iris_file_volume_register(
    &(esp_iris_file_volume_config_t) {
        .id = "cfg",
        .base_path = "/littlefs/export",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME |
                        ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    }));
ESP_ERROR_CHECK(esp_iris_start());
```

线上路径始终相对于注册根目录。Gateway 以流式方式上传和下载，不会把完整文件
缓存到内存，下载支持 HTTP Range。上传在目标同目录创建临时文件，使用严格
offset ACK、SHA-256、`fsync` 和 rename；只有底层 VFS 确实满足替换语义时才应
声明 `ATOMIC_REPLACE`。重命名不覆盖已有目标，删除只接受文件和空目录，不开放
递归删除或跨卷操作。

## 安全边界

- USB 依赖物理访问，不执行链路配对。
- Raw TCP 配对默认关闭；开启后 token 与稳定 Device ID 保存在
  `CONFIG_ESP_IRIS_NVS_PARTITION_NAME` 选择的 NVS 分区中（默认 `nvs`），链路
  通过随机 challenge 证明持有 token，token 本身不在链路上传输。
- Gateway 默认允许 loopback 免登录；非 loopback 客户端需要开发者登录或
  命名 Agent Token。Agent Token 的文件权限分为 `files.read`、`files.write`
  和 `files.delete`，新 token 默认只有 `files.read`。
- HTTP 会向本地网络暴露凭据和设备数据，只应在可信开发网络使用；其他环境
  应开启 Gateway TLS。
- Wi-Fi 密码、配对 token、TLS 私钥和 Agent Token 必须放在被忽略的本地配置
  或私有文件中。
- System Update 只应由保留的 factory recovery 同时注册只读 inventory provider
  和写 backend；正常固件仅注册 inventory provider，用于重启后的闭环校验。
  Gateway 和设备分别验证签名 manifest；产品 backend 必须固定信任公钥、保护
  系统固定区并逐项核对组件。Inventory 必须对当前 Flash 的完整保护区计算哈希，
  包含擦除态 `0xff` 填充，不能直接回传 sysmeta 中保存的期望值。

## 使用 mDNS 发现 TCP 设备

启用 TCP transport 的产品可在自行初始化 mDNS 并设置 hostname 后发布 DNS-SD
服务：

```c
ESP_ERROR_CHECK(mdns_init());
ESP_ERROR_CHECK(mdns_hostname_set("my-product-a1b2c3"));
ESP_ERROR_CHECK(esp_iris_mdns_register(NULL));
```

该 API 发布 `_esp-iris._tcp.local.`，实例名为唯一的
`ESP-Iris-<MAC 后缀>`。ESP-Iris 只拥有这项服务；产品调用 `mdns_free()` 前应先
调用 `esp_iris_mdns_unregister()`。mDNS 元数据只用于本地链路发现，不承担认证，
也不会广播 pairing token。

## 示例

所有公开示例都随组件发布，位于 [`examples/`](examples/README.md)。

| 示例 | 传输 | 适用场景 |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP、USB CDC0、USB Serial/JTAG 或三者同时 | 身份、生命周期、状态、日志和传输仲裁 |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | 应用管理 Wi-Fi 和 DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC 配对和 token 配置 |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 | RPC handler 和可取消 Job |
| [`display_input`](examples/display_input/README.md) | USB CDC0 | 截图、屏幕镜像和指针输入 |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 | 合成图像和 PCM 音频流 |
| [`file_transfer`](examples/file_transfer/README.md) | USB CDC0 | 流式文件上传/下载和元数据操作 |
| [`ota`](examples/ota/README.md) | USB CDC0 | Recovery-first/直接 OTA、验收和回滚 |
| [`file_service`](examples/file_service/README.md) | USB CDC0 | FATFS 逻辑卷和有界文件操作 |
| [`crash_recovery`](examples/crash_recovery/README.md) | USB CDC0 | 保留 Core Dump，并在连续崩溃后进入 factory recovery |
| [`lifecycle`](examples/lifecycle/README.md) | USB CDC0 | 停止、注销、重启和重连 |

硬件内部 fixture 保留在 `test_apps/`，不进入发布归档。

## 文档导航

- [公共 C API](include/esp_iris.h)
- [System Inventory provider API](include/esp_iris_system_inventory.h)
- [System Update backend API](include/esp_iris_system_update.h)
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
