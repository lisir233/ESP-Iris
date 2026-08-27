# ESP-Iris PC Gateway ↔ ESP32 实机验证报告

日期：2026-08-26 至 2026-08-27
目标芯片：ESP32-S31
ESP-IDF：`esp-idf-master-1`（构建日志显示 IDF 6.2-dev）
测试接口：USB High-Speed CDC、USB Serial/JTAG、配对 TCP/Wi-Fi

## 结论

计划内三类链路和核心服务均完成实机验证。握手、状态、日志、RPC/Job、图像与音频媒体、截图/镜像/输入、文件服务、OTA、崩溃回滚、令牌轮换以及正常断线重连均通过。损坏帧、文件写入中断、空间耗尽、Gateway 中断、设备重启和 OTA 中断没有产生已提交的损坏数据、错误的 OTA 成功状态、持续堆增长或失控会话。

本轮结果可判定为通过，但保留三个需要在产品准入阈值中明确的资源/时序观察项：

- OTA 传输中断路径观测到 ESP-Iris 主任务最低剩余栈 540 字节，文件空间耗尽路径为 816 字节；未发生溢出或重启，但量产配置应定义最小栈余量门槛。
- OTA 擦写期间 `worker_active_max_us` 峰值约 2.39 秒；操作状态和回滚均正确，若产品要求更严格的控制面时延，应单独设定上限。
- TCP 在人为取消 Gateway 默认退避、约 50 ms 内连续抢连时可能先命中仍在回收的旧连接；使用 Gateway 默认至少 250 ms 的退避时，20 次连续重连全部成功。

## 实机矩阵

| 范围 | 主要验证 | 结果与证据 |
|---|---|---|
| USB High-Speed | HELLO、状态、日志、RPC、计划重启、Gateway 重开 | 通过；重启前后 boot/session 变化正确，Gateway 重开不误触发设备重启 |
| USB Serial/JTAG | HELLO、状态、日志、RPC、Job 查询/取消、计划重启 | 通过；固定端点重开保持 boot，计划重启后自动恢复 |
| TCP 配对 | 正确/错误令牌、重连、计划重启、令牌轮换与持久化 | 通过；错误令牌不能发现设备，新令牌跨设备重启有效，旧令牌失效 |
| RPC/Job | echo、info、长任务、查询、取消、终态 | 三链路相关固件实机通过；取消终态为 `cancelled` |
| 图像/音频 | RGB565 与 PCM S16LE 双流 | 各连续 12 帧；图像 1,920 B/帧，音频 3,200 B/帧，序号递增，音频幅值为 ±16,384 |
| 显示/输入 | 480×480 截图、镜像分片、51 点手势 | PNG 尺寸和确定性像素正确；30 个镜像分片边界/长度正确；指针 RPC 全部回显 |
| 文件正常路径 | 上传、下载、Range、mkdir、rename、delete、审计 | 13,337 B 内容和 SHA-256 一致；路径穿越、非空目录删除、非原子覆盖均被拒绝 |
| 文件写入中断 | 512 KiB 上传时强制终止 Gateway | 设备已接收 505,856 B；恢复后操作为 `interrupted`，目标及 `.iris-*` 临时文件均不存在 |
| 文件空间耗尽 | 向不足 1 MiB 的 FAT 分区写入 2 MiB | 返回 HTTP 507/`file_no_space`；无目标/临时文件；失败后 RPC 正常 |
| 异常帧 | 截断 COBS、错误 CRC、错误 session、超长帧 | 设备 `invalid_frames` 精确增加到 4；随后 Gateway 握手、状态和 RPC 正常 |
| OTA 正常闭环 | factory/ota0/ota1 切换、固件元数据、自动 healthy | 通过；操作在 healthy 后才成功，running/last-good/target/planned 状态闭合 |
| OTA 中断 | 传输中强制终止 Gateway | 操作为 `interrupted`，未重启到半写镜像，恢复后 running/last-good 未改变 |
| OTA 崩溃回滚 | pending 镜像启动前注入崩溃 | OTA 未误报成功；bootloader 回滚到 last-good；保留有效 4,768 B coredump，ELF SHA 匹配 |
| USB 压力 | 1,000×256 B RPC；30 次端点关闭/重开 | 约 170 RPC/s；可用堆和 ESP-Iris heap 前后完全一致；30 个新 session、同一 boot |
| TCP 压力 | 1,000×256 B RPC；20 次默认退避重连 | 全部成功；P50 11.4 ms、P99 42.6 ms、最大 67.0 ms；堆不下降、20 个独立 session |

## 主机与构建回归

- Ruff：全量 `iris_gateway` 与 `tests` 通过。
- mypy：24 个生产 Python 模块通过。
- pytest：105 个通过。
- Vitest：3 个通过。
- Playwright：5 个通过；测试会自动启动使用唯一临时状态目录的 Demo Gateway，不再受历史设备缓存影响。
- TypeScript/Vite：生产构建通过。
- ESP32-S31：`services_usb`、`minimal`、`rpc_jobs`（含 USB Serial/JTAG）、`tcp_pairing`、`coredump`、`media_streams`、`display_input`、`file_transfer` 均零编译警告。

关键构建结果保存在各项目的 `build/codex-runs/` 下，例如：

- `test_apps/services_usb/build/codex-runs/20260826-231227-build-3913502/result.json`
- `examples/minimal/build/codex-runs/20260826-231811-build-3924565/result.json`
- `examples/rpc_jobs/build/codex-runs/20260826-232058-build-3934236/result.json`
- `examples/tcp_pairing/build/codex-runs/20260826-232324-build-3943765/result.json`
- `test_apps/coredump/build/codex-runs/20260826-233408-build-3965486/result.json`
- `examples/media_streams/build/codex-runs/20260826-234359-build-3975380/result.json`
- `examples/display_input/build/codex-runs/20260826-234642-build-3985042/result.json`
- `examples/file_transfer/build/codex-runs/20260827-000032-build-3997694/result.json`

## 固化的测试补强

- coredump/OTA 夹具在 pending-verify 正常启动后自动调用 `esp_iris_mark_healthy()`；崩溃注入仍先执行，因此正常升级和回滚都可自动闭环。
- CI 将 Ruff 从部分致命规则扩展为全量规则，并新增生产代码 mypy。
- CI 新增隔离 Playwright E2E。
- CI 新增 `file_transfer`、USB Serial/JTAG `rpc_jobs`、`services_usb` 和 `coredump` 构建。
- Demo Hub 补齐设备事件订阅接口，并增加重启事件订阅回归测试。

## 安全与数据完整性说明

- 报告和提交差异不包含 Wi-Fi 密码、配对令牌、TLS 私钥或其他测试密钥。
- TCP 认证验证覆盖错误令牌、正确令牌、令牌轮换、旧令牌撤销效果及设备重启后的新令牌持久化。
- 文件上传使用临时文件、完成哈希校验后提交；会话结束、写入失败和空间耗尽路径均实机确认会清理临时文件。
- OTA 只有在同设备重连并报告 healthy 后才记为成功；中断和崩溃均保留可查询的失败/中断状态。
