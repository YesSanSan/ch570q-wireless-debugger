# 1.3.4：独立 EP0 USB 会话恢复

日期：2026-10-25。基于已提交的 1.3.3（`b2ca5d5`）。实现、原生回归及构建通过，已完成 [1.3.4 上板复测](v134-hardware-validation.md)：Bulk 20 轮中 17 轮留下 RESPONSE 阻塞，均无需拔插恢复；HID 20 轮杀后均 idle，重置与后续 Info 校验通过。目标 RAM 写读另有独立测试；GDB 20/20、30 min 浸泡重跑通过，首次浸泡有一次未定位 EPIPE。处理中 RESET、HALT/suspend 和本版烧录仍待验证。历史现象见 [1.3.3 上板验收 §F](v133-hardware-validation.md#f-强杀客户端仍然打死主机)。

## 解决范围

HID 与 Bulk 共享单个 DAP 请求槽。响应提交后，设备等待对应 IN 完成才释放槽；客户端若在此时退出，可能留下无人读取的响应，导致两个 OUT 都 NAK，普通 HID 管理命令也无法送入。该路径与 1.3.3 的失联现象吻合，但尚不足以唯一确定真实故障根因。

1.3.4 在 WinUSB interface 1 上提供独立的 vendor EP0 状态查询与显式重置，不依赖 DAP 请求槽。它可以在请求 READY、PROCESSING 或 RESPONSE 时执行。没有响应自动到期机制：延迟客户端或未取消的旧 IN 可能读走下一条命令的响应，CMSIS-DAP 没有事务 ID 可以纠正这种对应关系。

重置只结束本地主机的旧 USB/DAP/RF 软件会话，不重放旧命令，不宣称已撤销目标写入，不确认从机连通或目标健康。已经进入空口或已经执行的目标操作无法撤回。需要重新连接调试软件，完成新 HELLO、DAP Connect 和目标初始化。

## 操作

推荐两块板都更新到 1.3.4。EP0 功能位于主机，主机更新即可获得此入口；`DAP_Info(4)` 版本来自从机，不能用它单独证明主机版本。EP0 `WUSB` 是独立协议版本（当前为 1），不是固件版本。

先关闭 OpenOCD、pyOCD、HID/Bulk 调试客户端和 CDC 串口，确保旧客户端退出、旧读写已取消，再执行：

```powershell
build/host-venv/Scripts/python tools/debugger_ctl.py usb-status
build/host-venv/Scripts/python tools/debugger_ctl.py usb-recover
build/host-venv/Scripts/python tools/debugger_ctl.py usb-status
```

工具通过 PyUSB/libusb 与现有 WinUSB interface 1 通信，不打开 HID，不发送普通 DAP 请求，也不调用 `set_configuration`、`set_interface`、整设备 reset 或驱动解绑。若依赖缺失，需在所用 Python 环境安装 PyUSB 及可用 libusb backend；无需为恢复更改 HID/CDC 驱动。存在多个相同 VID/PID 设备时拒绝猜测，当前占位 USB 身份可能没有可区分的序列号，应断开多余设备。

`usb-recover` 先读状态，只发送一次 RESET，随后最多等待 2 s 检查专用计数加一且前台确认当前 generation。RESET 控制传输超时时也先查询实际计数，不自动再发 RESET。成功返回 `status=local_session_reset` 与前后状态；它只确认本地会话清理。资源释放失败作为附加警告报告，不掩盖已确认结果或“结果不确定”的错误。

端点 HALT、toggle、CDC 线路设置及 DTR 均保留；该命令不是端点 clear-halt 或 USB resume。状态仍显示 HALT/suspend 时需单独定位。主机前台按既有规则隔离 CDC、取消旧缓冲；`cleanup_generation` 不代表 CDC 取消已完成。之后先以 DTR 低打开串口，等待设备观察到关闭状态，再拉高 DTR。USB/RF 主循环本身不再运行、EP0 不响应或 OS 后端无法访问时，该软件入口不能保证恢复。

## EP0 协议

所有请求严格限定：已配置，`bRequest=0x52`，`wIndex=1`，使用 interface recipient。其他方向、长度或 magic 返回 STALL；旧固件也会 STALL，工具据此提示不支持。

| 操作 | bmRequestType | wValue | wLength | 行为 |
| --- | --- | --- | --- | --- |
| STATUS | `0xc1` | `0` | `24` | 不消费 DAP 响应、不增加 generation |
| RESET | `0x41` | `0x5253` | `0` | 状态 IN 完成后才提交重置 |

24 B STATUS 布局：

| 偏移 | 大小 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `WUSB` |
| 4 | 1 | 协议版本 1 |
| 5 | 1 | DAP 状态：0 idle、1 control RX、2 control status、3 ready、4 processing、5 response |
| 6 | 1 | 传输：0 none、1 HID、2 Bulk |
| 7 | 1 | configuration |
| 8 | 1 | suspended |
| 9 | 1 | IN HALT bitmask，bit N 对应 EP N |
| 10 | 1 | OUT HALT bitmask |
| 11 | 1 | 保留，0 |
| 12 | 4 | USB generation，LE32 |
| 16 | 4 | 专用 `usb_reset_counter`，LE32，启动归零、回绕；普通 bus reset 不增加 |
| 20 | 4 | `cleanup_generation`，LE32，主循环已丢弃旧应用会话的代次 |

新的 SETUP 会按 USB 规则取消未提交的前一个控制请求，包括不完整的 HID SET_REPORT。STATUS 不消费已经发布的 DAP 请求或响应；它不能保留被新 SETUP 合法取消的半截控制传输。

重置在 `RB_UIF_TRANSFER` 与 `RB_UC_INT_BUSY` 阻止下一事务的窗口内执行，保留 EP1/2 的 RX/TX toggle 与 HALT，不清写 live DMA。保留的前一笔 IN/OUT 完成先处理，再解释新 SETUP。重置清 DAP 状态、待发长度与 HID 快照，并增加 generation 和专用计数；前台看到 generation 变化后清旧 session/phase/CDC 后缀，才写入确认代次。旧 tag 无法确认新的 reset，也不能向新请求提交或丢弃响应。

普通 HID 管理 `reconnect` 仍保留，用于 DAP 通道可用时清理 RF 软件会话；USB 请求槽阻塞时使用 EP0 `usb-recover`。

## 软件验证

- `pwsh -NoProfile -File tests/run.ps1`：14 个 native 套件（包含 USB IRQ/polling 两种模型）及 33 个 Python 管理工具测试通过。
- EP0 回归覆盖 HID/Bulk 的 READY/PROCESSING/RESPONSE，状态查询不消费响应，提交前取消、错误请求拒绝、重复完成不重置两次、保留 toggle/HALT/CDC，保留旧 IN/OUT 完成、旧响应拒绝、计数回绕、连续 reset 和 stale ack。
- 应用回归覆盖 HELLO、REQUEST 和待交付响应阶段换代：拒绝旧 RF 响应、不重放 payload、清理 CDC 后缀、前台确认与新连接握手。
- CLI 使用假 USB 后端测试，不访问真实 USB：精确请求、超时但已提交、未提交、确认等待、异常计数、不支持/权限/backend、资源清理失败及不打开 HID。
- 完整 HOST/SLAVE/AUTO 与全部历史诊断镜像构建通过。寄存器 mock 不模拟真实 SIE/DMA 仲裁、USB PID 检查、Windows URB 取消时序，不能代替下列上板验收。

## 上板验收步骤

1. 正常读取 `usb-status`，确认 WUSB v1，记录 generation、专用计数、端点状态。正常结束一次 Bulk/HID 工作后查询，确认没有额外 RESET。
2. 分别在持续 Bulk 与 HID 工作期间强杀客户端；关闭残留进程和 CDC。保留首次故障、HID/Bulk 错误及 EP0 状态，不先拔插。
3. 若状态为 response，验证普通请求仍阻塞时 EP0 可以读；执行一次 `usb-recover`，记录专用计数恰好加一、前台确认代次一致及恢复时间。
4. 重新启动客户端，先读 DAP Info/DP IDCODE，再执行 RAM 写后回读、恢复原值及连续块读。确认旧响应未被当成新结果、原不确定操作没有被自动重放。分别覆盖处理中请求与未读响应两个退出窗口。
5. CDC 以 DTR 低→高重开，检查日志与旧 TX 后缀不跨会话。单独验证 suspend/HALT 的状态和处理，不能把“本地 session reset 成功”当作这些故障已清除。
6. 建议 HID/Bulk 各重复 20 轮强杀→EP0 恢复→读写校验，随后连续 fb 30 min 和 GDB 8×1 KiB 20 轮。验收目标是无需物理拔插且结果对应正确；EP0 本身失联时记录主循环心跳、USB IRQ/端点寄存器和主机错误，继续定位。

上板验收已于 2026-10-25 执行，逐条结果见 [1.3.4 上板验收](v134-hardware-validation.md)。未覆盖项：真实 USB suspend / 端点 HALT、在 READY/PROCESSING 窗口内执行 RESET（两个窗口实测都短于 150 ms）。1.3.3 已通过的长期读与烧录结果保留为历史基线，不能转记为 1.3.4 的实板通过结果。
