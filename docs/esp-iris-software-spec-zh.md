# ESP-Iris 软件规格书

| 项目 | 内容 |
| --- | --- |
| 文档状态 | Draft |
| 文档语言 | 中文 |
| 适用范围 | 当前 ESP-Iris 源码树 |
| 组件版本 | `0.1.0`，并包含 `Unreleased` 源码能力 |
| 目标平台 | ESP-IDF 5.5 及以上 |
| 许可证 | Apache-2.0 |

## 1. 文档目的

本文档定义 ESP-Iris 需要解决的工程问题、系统边界、解决方法、功能与非功能规格、技术优势和典型应用场景。本文档既用于产品定位和技术评审，也可作为后续宣传资料、方案设计、集成测试和验收工作的共同依据。

本文描述的是当前源码树已经提供的能力。标记为“可选”的功能需要通过 Kconfig 或产品侧注册启用；标记为“产品负责”的功能需要由应用、BSP、Recovery 固件或安全后端实现相应策略。

## 2. 产品概述

ESP-Iris 是连接 ESP32 设备与 PC 开发环境的结构化观测和控制系统。它由以下三部分组成：

1. 运行在 ESP32 上的 ESP-IDF 组件；
2. 运行在 PC 或开发服务器上的 Python Developer Gateway；
3. 面向开发者和 AI Agent 的 Web Workbench、CLI、REST API 与 WebSocket API。

ESP-Iris 的核心目标不是简单替换串口终端，而是将传统单向、纯文本、单设备的串口调试方式升级为统一、结构化、可恢复、可审计的设备开发工作流，使人类开发者与 AI Agent 能够同时理解、观察和操作同一台或同一组嵌入式设备。

```text
ESP32 设备集群
  ├─ USB CDC0
  ├─ USB Serial/JTAG
  └─ TCP / Wi-Fi / Ethernet
             │
             ▼
     Python Developer Gateway
       ├─ 设备与会话管理
       ├─ 事件与日志持久化
       ├─ 操作编排与审计
       ├─ 制品与崩溃证据存储
       └─ 认证、REST、WebSocket
             │
       ┌─────┼──────────┐
       ▼     ▼          ▼
  Web Workbench  CLI  AI Agent
```

## 3. 术语

| 术语 | 定义 |
| --- | --- |
| Device | 集成 ESP-Iris 组件的 ESP32 设备 |
| Gateway | 在 PC 侧管理设备链路、状态、数据和操作的 Developer Gateway |
| Workbench | Gateway 提供的 React Web 工程界面 |
| Agent | 通过 CLI、REST 或 WebSocket 使用 ESP-Iris 的自动化或 AI 客户端 |
| `device_id` | 跨重启保持稳定的设备身份 |
| `boot_id` | 标识设备的一次启动 |
| `session_id` | 标识一次有效链路会话 |
| `operation_id` | 标识一次可持久查询的长期 Gateway 操作 |
| RPC | 主机向设备发起的有界二进制远程调用 |
| Job | 设备侧可查询进度、结果并支持取消的长期任务 |
| Artifact | Gateway 保存的固件、截图、Coredump 等工程制品 |

## 4. 需要解决的问题

### 4.1 传统串口工作流以纯文本为中心

传统嵌入式调试通常依靠 stdout、stderr 和交互式串口命令。该模式存在以下问题：

- 信息主要由非结构化文本表达，机器解析依赖正则表达式或自然语言猜测；
- 时间、设备身份、请求与响应之间缺少稳定关联；
- 同一状态可能被重复打印，传输冗余且难以形成可靠的状态模型；
- 图像、音频、屏幕、文件和固件等二进制数据难以复用同一开发通道；
- 断线或终端关闭后，现场上下文通常无法连续恢复。

### 4.2 编译和烧录被用于过多开发动作

在缺少运行时控制接口时，查询内部状态、调整参数、执行诊断动作或替换设备数据，都可能要求修改源码、重新编译和重新烧录。这会增加迭代等待时间，也会改变原始故障现场。

传统 ROM 下载或串口烧录仍然是设备首次安装、分区损坏恢复等场景的必要手段。ESP-Iris 不承诺改变 ROM 串口下载协议本身的速度，而是通过 RPC、文件服务、OTA 和远程操作减少必须重新编译和有线烧录的次数。

### 4.3 人类开发者与 Agent 无法共享设备现场

串口端点通常被单个进程独占。开发者打开串口监视器后，Agent 很难同时读取同一事件；Agent 执行操作时，开发者也未必能观察到完整过程。多个客户端争抢串口还可能触发设备复位、丢失日志或破坏命令响应边界。

### 4.4 虚拟机、WSL、容器和远程环境访问硬件困难

USB 和串口设备的透传、权限、端口重枚举及独占访问，在虚拟机、WSL、容器和远程开发环境中往往不稳定。将所有客户端直接连接硬件会放大这些环境差异。

### 4.5 多设备调试缺少统一身份和集中管理

传统工具通常以临时端口名识别设备。设备重启、USB 重新枚举或切换到 Recovery 固件后，端口可能变化；同时调试多台设备时还需要维护多个终端窗口，难以统一查询状态、历史和操作结果。

### 4.6 长期操作缺少可恢复的状态闭环

OTA、Recovery、文件传输和长期设备任务可能跨越断线、重启或 Gateway 重启。若只依赖一次同步请求，客户端无法可靠判断操作究竟成功、失败、中断还是仍在进行。

### 4.7 嵌入式设备资源有限

直接在产品固件中嵌入 HTTP Server、WebSocket Server、JSON Parser、Web UI 或无界媒体队列，会增加 Flash、RAM、线程和攻击面负担。调试基础设施必须具有明确资源上限，并且不能因为主机处理缓慢而导致设备内存持续增长。

### 4.8 远程控制扩大了安全风险

网络控制、文件写入、OTA 和 Agent 自动化都可能改变设备状态。系统需要区分物理 USB 信任、TCP 设备配对、Gateway 用户认证和 Agent 权限，并避免默认暴露设备根文件系统或原始 Flash 写入能力。

## 5. 解决问题的方法

### 5.1 使用统一的有界二进制设备协议

ESP-Iris 在 USB 或 TCP 之上使用版本化二进制协议承载控制、事件、日志、RPC、媒体、OTA、崩溃证据和文件数据。协议使用明确的帧边界、校验、会话标识、请求标识和能力协商，避免将控制语义建立在自由格式文本之上。

协议与设备实现遵循有界原则：

- 请求体、响应体和分块大小受 Kconfig 限制；
- 日志使用固定容量 Ring Buffer；
- 媒体仅保留每个活动 Channel 的最新数据块；
- 文件和 OTA 使用分块传输；
- Coredump 使用只读分块读取，不在 RAM 中复制完整 Coredump；
- 控制和可靠事件优先于媒体数据调度。

### 5.2 将复杂开发平面放在 PC 侧

设备侧只运行传输、协议、服务调度和产品回调。以下能力由 Gateway 承担：

- HTTP、WebSocket 与 OpenAPI；
- Web Workbench；
- 多设备发现、重连和集中管理；
- SQLite 持久化；
- 日志轮转、制品和证据存储；
- 用户认证、Agent Token 和 TLS；
- OTA、Recovery 等跨会话操作编排。

该设计避免在量产固件中嵌入浏览器工程界面及其依赖。

### 5.3 由 Gateway 独占设备链路并向多客户端扇出

每台物理设备同一时刻最多存在一个有效 Gateway 会话。Workbench、CLI 和 Agent 不直接争抢 USB/TCP 链路，而是连接 Gateway：

- 每个 WebSocket 客户端拥有独立事件队列；
- 客户端读取事件不会消费其他客户端的事件；
- 新客户端可先接收近期持久化事件，再进入实时事件流；
- 设备修改操作按 `device_id` 串行化；
- 观察动作不会被同设备的写操作阻断；
- 不同设备之间可以并行工作。

### 5.4 以稳定身份和关联 ID 建立可追溯性

系统使用 `device_id`、`boot_id`、`session_id`、`operation_id`、`request_id`、`job_id`、`event_id` 和固件 SHA 关联设备身份、启动、连接、请求、任务、长期操作和固件版本。正常固件和 Recovery 固件只要报告同一个 `device_id`，就能够归入同一设备历史。

### 5.5 将重复烧录动作转换为运行时能力

产品可通过以下扩展点暴露安全且明确的运行时操作：

- 注册自定义 RPC；
- 创建可取消 Job；
- 注册截图后端；
- 提交图像或音频流；
- 注册允许访问的逻辑文件卷；
- 接入产品 Recovery、健康确认和 OTA 分区策略；
- 在受控 Recovery 固件中注册多镜像系统更新安全后端。

### 5.6 使用持久化状态表达长期操作

Gateway 为 OTA、Recovery 等长期操作创建 `operation_id`，持久记录阶段、进度和终态。终态一旦形成便不可变；Gateway 重启不会盲目重放设备写操作。结果不确定时，客户端使用原始 `operation_id` 查询，而不是再次提交可能重复执行的请求。

### 5.7 建立分层安全边界

- USB 链路依赖物理访问，不执行设备配对；
- Raw TCP 可启用 Challenge-HMAC 配对，Token 本身不在线上传输；
- Gateway 支持开发者密码、命名 Agent Token 和 Token 撤销；
- Agent 文件权限分为 `files.read`、`files.write` 和 `files.delete`；
- 非 Loopback 部署应启用 TLS；
- 文件服务只暴露产品显式注册的逻辑卷；
- OTA 不选择 Factory、NVS、Coredump 或崩溃元数据分区；
- 多镜像系统更新不提供原始 Flash 授权，所有 Manifest 认证和目标授权由产品安全后端负责。

## 6. 系统架构规格

### 6.1 设备组件

设备组件应负责：

- 生命周期启动、停止和会话协调；
- USB CDC0、USB Serial/JTAG、Raw TCP 传输；
- HELLO/HELLO_ACK 握手和能力协商；
- stdout/stderr 捕获、状态、RPC/Job、媒体、文件、Crash、OTA 等服务；
- 产品回调和 Recovery 适配接口；
- 有界缓冲、优先级调度和流控。

设备组件不应负责：

- HTTP 或 WebSocket 服务；
- JSON 解析；
- Web UI；
- SQLite 或长期工程记录；
- 产品 Wi-Fi 配网和网络接口生命周期；
- 未经产品授权的文件系统或原始 Flash 访问。

### 6.2 Developer Gateway

Gateway 应负责：

- 并发监督多个 USB/TCP 端点；
- 按稳定设备 ID 聚合状态和历史；
- 自动重连和会话状态机；
- REST/WebSocket API；
- Workbench 静态资源服务；
- 操作编排、审计、持久化和制品存储；
- 认证、Agent Token、TLS、Health 和 Metrics；
- 为 CLI 和 Agent 提供稳定 JSON 输出。

### 6.3 Web Workbench

Workbench 应提供以下工程视图：

| 页面 | 主要能力 |
| --- | --- |
| Overview | 设备身份、连接、固件、健康和当前状态 |
| Logs | 实时及保留日志 |
| Workspace | RPC、Console、截图、镜像、输入、媒体、固件、OTA 和重启 |
| Files | 逻辑卷、目录、上传、下载、重命名、建目录和安全删除 |
| Operations | 长期操作的阶段、进度和终态 |
| Records | 会话、证据、制品和历史记录 |
| Settings | 访问模式、密码、Token、TLS 和系统设置 |

### 6.4 CLI 与 Agent 接口

CLI 和 API 应支持：

- 设备枚举和状态查询；
- 日志历史与 Follow；
- 结构化或 Raw RPC；
- Job 查询与取消；
- 截图、镜像、输入和媒体访问；
- 固件制品导入和 OTA；
- 操作状态查询与 Watch；
- 崩溃报告和 Coredump 下载；
- 文件操作；
- Observe 模式；
- 稳定、可脚本化的 JSON 输出。

## 7. 功能规格

### 7.1 传输与会话

| 编号 | 规格 |
| --- | --- |
| FR-TR-01 | 系统应支持 Raw TCP 传输，默认端口为 `19772`。 |
| FR-TR-02 | 兼容芯片应支持 USB Serial/JTAG 传输。 |
| FR-TR-03 | ESP32-S31 应支持 Application USB CDC0 传输。 |
| FR-TR-04 | 同一固件可启用一种或多种兼容传输。 |
| FR-TR-05 | 多传输模式下，首个完成合法握手及必要认证的候选链路应独占活动 Session。 |
| FR-TR-06 | 活动链路断开后，其他已配置传输应重新进入等待状态。 |
| FR-TR-07 | Application USB CDC0 只承载 ESP-Iris 二进制协议，不作为文本控制台或自动下载端口。 |
| FR-TR-08 | USB Serial/JTAG 的串行端点不得同时被 Gateway、串口监视器或烧录工具占用。 |

### 7.2 设备身份、状态和日志

| 编号 | 规格 |
| --- | --- |
| FR-OBS-01 | Gateway 应以稳定 `device_id` 管理设备。 |
| FR-OBS-02 | 状态应至少包含生命周期、传输、连接、Session、Uptime、帧统计、无效帧、日志丢弃、栈余量、Worker 最大活动时间、内部堆占用和重启信息。 |
| FR-OBS-03 | stdout/stderr 写入不应因主机未读取而无限阻塞设备。 |
| FR-OBS-04 | 日志 Ring 满时应丢弃最旧数据并报告丢弃字节数。 |
| FR-OBS-05 | Gateway 应保留日志索引，并对原始日志执行可配置轮转。 |
| FR-OBS-06 | 每个事件在适用时应包含足以关联设备、启动、Session、操作或请求的标识。 |

### 7.3 RPC 与 Job

| 编号 | 规格 |
| --- | --- |
| FR-RPC-01 | 产品应能在启动阶段注册和注销有界二进制 RPC Handler。 |
| FR-RPC-02 | RPC 应包含 Service ID、Method ID、Request ID 和 Deadline。 |
| FR-RPC-03 | RPC Handler 数量及请求/响应体大小应受 Kconfig 限制。 |
| FR-JOB-01 | 产品应能创建、更新、完成和查询长期 Job。 |
| FR-JOB-02 | Job 应报告状态、千分比进度、结果和取消请求。 |
| FR-JOB-03 | Gateway 应允许查询和请求取消 Job。 |

### 7.4 屏幕、媒体与输入

| 编号 | 规格 |
| --- | --- |
| FR-MED-01 | 产品应能注册按需读取的截图后端。 |
| FR-MED-02 | 系统应支持 RGB565、RGB888、JPEG、PNG、PCM S16LE 和 Opus 格式描述。 |
| FR-MED-03 | 图像、音频和屏幕镜像 Channel 应具有独立 Credit。 |
| FR-MED-04 | 慢速主机不得导致设备侧形成无界媒体队列。 |
| FR-MED-05 | 新媒体块可以替换尚未发送的旧块，并应记录丢弃信息。 |
| FR-MED-06 | 控制响应和可靠事件应优先于媒体数据。 |
| FR-IN-01 | Gateway 应提供指针或触控输入接口，由产品 RPC 或输入适配层执行。 |

### 7.5 文件服务

| 编号 | 规格 |
| --- | --- |
| FR-FILE-01 | 产品必须在 `esp_iris_start()` 前显式注册逻辑卷。 |
| FR-FILE-02 | 每个逻辑卷必须声明 Read、List、Mtime、Write、Delete、Mkdir、Rename 或 Atomic Replace 等能力。 |
| FR-FILE-03 | 线上路径必须为逻辑卷根目录下的 UTF-8 相对路径。 |
| FR-FILE-04 | 系统应拒绝路径穿越、跨卷操作、覆盖式 Rename、递归删除和非空目录删除。 |
| FR-FILE-05 | 下载应为流式传输，并支持 HTTP Range。 |
| FR-FILE-06 | 上传应使用严格 Offset ACK 和 SHA-256 校验。 |
| FR-FILE-07 | 覆盖上传应在目标同目录暂存，并仅在底层 VFS 满足语义时执行原子替换。 |
| FR-FILE-08 | 失败、断线或空间耗尽不得留下已提交的损坏目标文件。 |

### 7.6 OTA、Recovery 与崩溃证据

| 编号 | 规格 |
| --- | --- |
| FR-OTA-01 | 启用 OTA 时，设备应分块接收、校验并写入合法的非运行 OTA 分区。 |
| FR-OTA-02 | Factory、NVS、Coredump 和崩溃元数据分区不得成为 OTA 目标。 |
| FR-OTA-03 | 传输取消、Session 断开或写入错误时应调用 OTA Abort。 |
| FR-OTA-04 | Gateway 应支持直接 OTA 和产品配置允许的 Recovery-first OTA。 |
| FR-OTA-05 | Gateway 应在设备重连后校验固件身份，并仅在设备报告 Healthy 后将操作标记为成功。 |
| FR-OTA-06 | 产品可选择要求 OTA 镜像 Project Name 与当前固件一致；默认允许有意的跨项目更新。 |
| FR-CRASH-01 | 设备应报告上一次启动崩溃、Reset Reason、Panic Reason 和可用 Coredump 元数据。 |
| FR-CRASH-02 | Coredump 应只读、分块传输，读取行为不得自动擦除证据。 |
| FR-CRASH-03 | Gateway 不得在缺少直接证据时将崩溃自动归因于 OTA。 |

### 7.7 可选多镜像系统更新

该能力默认关闭，只应在保留的 Recovery 固件中启用。

| 编号 | 规格 |
| --- | --- |
| FR-SYS-01 | 系统应支持包含 Application、Recovery、Bootloader、Partition Table 或产品数据的签名更新计划。 |
| FR-SYS-02 | ESP-Iris 只负责有界传输和状态机，不得自行授权任意 Flash Offset。 |
| FR-SYS-03 | 产品安全后端必须认证 Manifest，并授权和校验每个 Component Descriptor。 |
| FR-SYS-04 | 每个 Component 结束时必须校验实际 SHA-256。 |
| FR-SYS-05 | 敏感镜像的写入和最终 Commit 策略必须由产品后端实现。 |
| FR-SYS-06 | 系统应支持查询当前布局、Bootloader/Partition Table 哈希和上次操作结果。 |

### 7.8 Gateway API、持久化和协同

| 编号 | 规格 |
| --- | --- |
| FR-GW-01 | 一个 Gateway 应能并发管理多个 USB/TCP 设备端点。 |
| FR-GW-02 | 每台物理设备同一时刻最多有一个活动 Gateway Session。 |
| FR-GW-03 | Workbench、CLI 和 Agent 应分别接收独立事件流。 |
| FR-GW-04 | WebSocket 事件流应支持从保留游标恢复。 |
| FR-GW-05 | 设备写操作应按设备串行化，不同设备之间不应被全局写锁互相阻塞。 |
| FR-GW-06 | Gateway 应持久化设备、Session、Operation、Audit、Token 元数据和日志索引。 |
| FR-GW-07 | 长期操作终态应不可变，并可通过原始 `operation_id` 查询。 |
| FR-GW-08 | Observe 模式应阻止业务写请求，同时继续协议维护和设备事件观察。 |
| FR-GW-09 | Gateway 应提供 `/v1/health`、`/v1/metrics`、`/v1/openapi.json` 和交互式 API 文档。 |
| FR-GW-10 | CLI 的 `--json` 输出应保持稳定并适合 Agent 脚本化使用。 |

### 7.9 认证与授权

| 编号 | 规格 |
| --- | --- |
| FR-SEC-01 | Loopback 默认可免登录，并可通过配置强制本地认证。 |
| FR-SEC-02 | 非 Loopback 客户端必须使用开发者会话或命名 Agent Token。 |
| FR-SEC-03 | Gateway 应支持 Agent Token 创建、列举和撤销。 |
| FR-SEC-04 | 新 Agent Token 的文件权限默认应为只读。 |
| FR-SEC-05 | Raw TCP 可要求 Challenge-HMAC 配对，配对 Token 不得在设备链路上传输。 |
| FR-SEC-06 | 对非可信开发网络提供服务时应启用 TLS。 |
| FR-SEC-07 | 系统不得在日志、公开配置或仓库中存储 Wi-Fi 密码、配对 Token、TLS 私钥和 Agent Token。 |

## 8. 非功能规格

### 8.1 资源确定性

- 核心链路使用一个有界 Worker Task 和固定协议缓冲区；
- RPC Handler、Job、日志 Ring、文件卷和分块大小均有 Kconfig 上限；
- 媒体缓冲只在实际启用流时分配；
- 文件服务使用单独且有界的任务、流和分块缓冲；
- 慢速客户端不得引起无界内存增长；
- Status 应暴露最低剩余栈、内部堆占用和最大 Worker 活动时间，供产品制定准入阈值。

### 8.2 可靠性与数据完整性

- 协议应检测截断帧、CRC 错误、错误 Session 和超长帧；
- 异常帧不得使后续合法握手、状态查询和 RPC 永久失效；
- 文件和 OTA 中断不得被错误记录为成功；
- 写操作结果不确定时不得自动重放；
- OTA 只有在重连、固件身份校验和 Healthy 确认后才能完成闭环；
- Gateway 和设备重启后，应尽可能保留可查询的操作与证据状态。

### 8.3 兼容性

- ESP-IDF 版本要求为 5.5 或以上；
- Raw TCP 为默认可移植传输，网络接口由产品应用创建和管理；
- Application USB CDC0 当前限定 ESP32-S31；
- USB Serial/JTAG 依赖目标芯片相应 SoC 能力；
- Gateway 要求 Python 3.11 或以上；
- Gateway 可运行于 Linux、macOS 或 Windows，当前真实设备验证重点为 Linux；
- Workbench 源码构建需要 Node.js 与 npm。

### 8.4 可测试性

协议和 API 变更应具有兼容性测试。测试体系应覆盖：

1. C、Python、TypeScript 单元测试；
2. 跨语言 Golden Vector 和 HTTP/RPC Contract 测试；
3. Fake Link、Gateway、SQLite、Workbench 集成测试；
4. ESP32 构建和真实硬件验证；
5. 损坏帧、断线、中断写入、空间耗尽、崩溃和回滚等故障注入。

### 8.5 可扩展性

- Wire Protocol 使用版本和 Capability 协商；
- 新增字段应可被旧实现安全跳过；
- 现有 v1 字段不得改变原有语义；
- 产品功能通过 C API、RPC Catalog、文件逻辑卷、媒体后端和 Recovery Adapter 扩展；
- Gateway 上层通过 REST、WebSocket、OpenAPI 和稳定 JSON 扩展；
- 设备传输与 RPC、媒体、文件、OTA 等服务语义解耦。

## 9. 技术优势

### 9.1 人与 AI Agent 共享同一设备事实源

Workbench、CLI 和 Agent 读取相同的 Gateway 状态、事件、日志、Operation 和 Artifact。独立事件队列避免观察者互相消费数据，持久化历史使新加入的参与者能够恢复上下文。这是 ESP-Iris 区别于单用户串口终端的核心价值。

### 9.2 从文本猜测升级为结构化操作

稳定的设备身份、状态字段、RPC、错误码、事件契约和 JSON 输出，使 Agent 不必依赖脆弱的串口文本解析。人类仍可阅读日志，而自动化系统能够基于明确状态做决策。

### 9.3 减少重复编译和有线烧录

查询、控制、诊断、文件替换和固件更新可以在运行时执行。它优化的是完整开发迭代链路，而不是夸大传统串口下载协议的物理速度。

### 9.4 多模态设备可观测性

日志、状态、截图、屏幕镜像、触控输入、图像、音频、文件和 Coredump 被纳入同一设备会话和权限模型，适合带屏、带摄像头、带音频或本地存储的复杂嵌入式产品。

### 9.5 多设备和远程开发友好

一个 Gateway 可以集中管理多个 USB 和网络设备。TCP 或远程 Gateway 模式降低 WSL、虚拟机、容器和远程 Agent 直接透传 USB 的依赖，并把临时端口转换为稳定设备身份。

### 9.6 操作可查询、可审计、可恢复

长期操作具有 ID、阶段、进度和不可变终态。设备重启、切换 Recovery 或 Gateway 重启不再天然意味着操作上下文丢失。日志、Audit、Firmware SHA 和 Crash Evidence 可以形成完整工程证据链。

### 9.7 设备侧保持轻量和有界

复杂的 Web、存储、认证展示和历史分析被移至 PC。设备端不需要运行 HTTP、WebSocket、JSON Parser 或 UI，也不会为完整媒体或 Coredump 进行大块无界分配。

### 9.8 扩展能力与产品安全边界并存

产品可以扩展 RPC、Job、媒体、文件卷、Recovery 和更新策略，但 ESP-Iris 不默认开放根文件系统或任意 Flash 写入。能力必须由 Kconfig、Capability 和产品注册共同授权。

## 10. 应用场景

### 10.1 AI Agent 辅助嵌入式开发

Agent 可通过稳定 JSON 和 OpenAPI 完成设备发现、状态检查、日志跟踪、RPC 调用、Job 观察、截图、OTA 和崩溃证据收集；开发者同时通过 Workbench 查看相同现场、审查操作和接管决策。

适用任务包括：

- 自动复现和诊断缺陷；
- 修改代码后自动构建、更新、验证；
- 对设备执行有边界的探测操作；
- 收集日志、截图、状态和 Coredump，形成问题报告；
- 在人类批准后执行固件更新或恢复操作。

### 10.2 带屏设备和 HMI 开发

开发者可以远程获取截图和屏幕镜像、注入指针或触控事件，并把输入动作与日志、RPC 和固件版本关联。适用于智能面板、家电屏、工业 HMI、可穿戴设备和其他图形交互产品。

### 10.3 音频、图像和传感设备调试

设备可以通过统一媒体 Channel 输出图像或音频样本，同时控制流保持可用。适用于摄像头预览、麦克风采样、音频播放链路、图像算法和多模态 AI 终端验证。

### 10.4 多设备实验室与小规模设备集群

一个 Gateway 可监督多个通过 USB、Wi-Fi 或 Ethernet 连接的设备，以稳定设备 ID 统一展示健康、日志和历史。适用于研发实验室、测试台架、老化测试、兼容性矩阵和多节点系统联调。

### 10.5 WSL、虚拟机、容器和远程团队开发

Gateway 可运行在能够稳定访问硬件的主机，Workbench、CLI 和 Agent 通过网络访问 Gateway。这样可减少每个开发环境分别解决 USB 权限、透传、端口重枚举和串口独占问题的成本。

### 10.6 现场问题复现和崩溃分析

Gateway 持久化设备、Session、日志、操作和崩溃证据。设备恢复连接后仍可读取 Coredump 和固件 SHA，帮助开发者确认故障发生在哪次启动、哪个固件和哪项操作之后。

### 10.7 安全文件维护和配置更新

产品可仅开放明确的配置或资源目录，通过权限受控的逻辑卷完成文件读取、流式更新、哈希校验和原子替换，而不必向开发工具暴露整个文件系统。

### 10.8 OTA、Recovery 和回滚验证

适用于开发阶段固件快速迭代、A/B 分区验证、Recovery-first 更新、异常镜像回滚和升级证据记录。对于包含 Bootloader 或 Partition Table 的系统级更新，应使用默认关闭的多镜像更新能力，并由产品安全后端认证和授权。

### 10.9 自动化硬件在环测试

测试系统可通过 Gateway API 操作设备，并使用同一事件、状态和 Artifact 作为验收证据。适用于 RPC 压力测试、断线重连、文件中断、OTA 中断、异常帧、崩溃回滚和长期稳定性测试。

## 11. 非目标与使用边界

ESP-Iris 当前不负责以下事项：

- 不替代 ESP-IDF 编译系统；
- 不替代首次烧录、Bootloader 损坏恢复或底层 JTAG 调试；
- 不保证传统 ROM 串口烧录本身提速；
- 不为产品创建 Wi-Fi/Ethernet 网络接口或管理配网；
- 不允许多个 Gateway 同时控制同一台物理设备；
- 不让同一设备同时维持 USB 和 TCP 两个活动 Session；
- 不自动决定产品哪些 RPC、文件目录或 Flash 区域可以开放；
- 不在缺少直接证据时判断一次崩溃由 OTA 引起；
- 不应在不可信网络上以明文 HTTP 暴露密码、Token、日志或设备控制接口；
- 当前实机验证不能自动等同于所有 ESP32 型号和所有产品配置均已完成量产认证。

## 12. 当前实现与验证状态

截至仓库中的 2026-08-27 实机报告，USB High-Speed CDC、USB Serial/JTAG 和配对 TCP/Wi-Fi 三类链路，以及状态、日志、RPC/Job、媒体、显示/输入、文件、OTA、崩溃回滚、Token 轮换和断线重连均已完成 ESP32-S31 实机验证。

已记录的关键结果包括：

- USB 压力场景完成 `1,000 × 256 B` RPC，约 `170 RPC/s`；
- TCP 压力场景完成 `1,000 × 256 B` RPC，P50 `11.4 ms`、P99 `42.6 ms`；
- 文件上传、下载、Range、Rename、Delete、中断和空间耗尽路径通过；
- OTA 正常闭环、中断、Pending 镜像崩溃和回滚路径通过；
- 异常 COBS、CRC、Session 和超长帧未破坏后续合法通信；
- 主机侧 Ruff、mypy、pytest、Vitest、Playwright 和 TypeScript/Vite 构建通过。

量产导入仍应针对具体产品明确资源与时序门槛。现有报告中的观察项包括：

- OTA 中断路径最低剩余栈曾观测为 `540 B`；
- 文件空间耗尽路径最低剩余栈曾观测为 `816 B`；
- OTA 擦写期间 Worker 最大活动时间峰值约 `2.39 s`；
- TCP 极端快速重连应保留 Gateway 默认退避，避免命中尚未回收的旧连接。

## 13. 建议验收标准

产品集成 ESP-Iris 时，至少应验证：

1. 目标芯片和所选传输能够完成握手、状态、日志和断线重连；
2. 每台设备的 `device_id` 稳定，重启后的 `boot_id` 正确变化；
3. Workbench、CLI 和 Agent 能同时观察且不会互相消费事件；
4. 产品 RPC、Job、文件卷和媒体后端符合声明的权限与资源限制；
5. 异常帧、慢客户端和端点重开不会造成持续堆增长或不可恢复 Session；
6. 文件写入中断、空间耗尽和哈希错误不会提交损坏文件；
7. OTA 中断不会启动半写镜像，成功状态只在重连和 Healthy 后形成；
8. Recovery、回滚和 Coredump 证据能够关联正确的设备及固件；
9. 非 Loopback 部署已启用认证，并根据网络环境启用 TLS；
10. Wi-Fi 密码、配对 Token、Agent Token 和 TLS 私钥未进入日志或版本库；
11. 最低栈余量、堆占用、Worker 最大活动时间和业务实时性满足产品门槛；
12. 相关 Python、前端和 ESP-IDF 构建及测试全部通过。

## 14. 产品价值总结

ESP-Iris 将嵌入式开发中的设备连接、状态观测、运行时控制、多媒体数据、文件维护、固件更新和崩溃证据统一到一个有界协议和 PC 侧 Gateway 中。它让人类开发者和 AI Agent 不再争抢串口，而是围绕同一设备身份、同一事件历史和同一操作结果协同工作。

其主要价值可以归纳为：

- **协同**：人、CLI 和 Agent 同时观察与操作；
- **结构化**：从自由文本升级为明确状态、RPC、事件和 API；
- **高效**：通过运行时控制、文件服务和 OTA 减少重复编译与有线烧录；
- **可观测**：覆盖日志、资源、屏幕、媒体、文件、操作和崩溃证据；
- **可恢复**：跨断线、重启和 Recovery 保留操作及设备历史；
- **可扩展**：支持产品 RPC、Job、媒体、文件卷和更新后端；
- **有边界**：设备资源、权限、文件路径和更新目标均受到明确限制。

ESP-Iris 因此不是另一个串口终端，而是一套面向 AI 时代的嵌入式协同开发基础设施。

## 15. 参考资料

- `components/esp_iris/README_zh.md`
- `components/esp_iris/tools/README.md`
- `components/esp_iris/protocol/spec.md`
- `components/esp_iris/include/esp_iris.h`
- `components/esp_iris/include/esp_iris_system_update.h`
- `components/esp_iris/Kconfig`
- `components/esp_iris/TEST_REPORT_2026-08-27.md`
- `docs/esp-iris-architecture.md`
