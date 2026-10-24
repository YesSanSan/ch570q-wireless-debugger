# 使用与维护

## 电脑端工具

Python 3.10+，建议使用虚拟环境。在项目根目录执行：

```powershell
python -m venv .venv
.venv/Scripts/python -m pip install -r tools/requirements.txt
.venv/Scripts/python tools/debugger_ctl.py info
.venv/Scripts/python tools/debugger_ctl.py stats
.venv/Scripts/python tools/debugger_ctl.py --remote info
.venv/Scripts/python tools/debugger_ctl.py --remote uart-stats
.venv/Scripts/python tools/debugger_ctl.py discover
```

后续命令中的 `python` 指安装了依赖的解释器。Linux 下激活 `.venv/bin/activate`；HID/libusb 的系统依赖和设备权限由发行版提供。管理工具使用 HID，不要与 OpenOCD 同时占用设备。

```powershell
python tools/debugger_ctl.py pair <16位十六进制ID>
python tools/debugger_ctl.py get-config
python tools/debugger_ctl.py --remote get-config
```

发现仅覆盖当前信道与接入地址，最多返回 4 台；选择从机不提供认证或加密。命令的详细参数见 `python tools/debugger_ctl.py --help` 和各子命令 `--help`。

**RF 配置持久化尚有上板失败记录。**`configure` 修改暂存配置，`--save` 请求保存，`apply` 请求延迟应用；不要将其当作可靠恢复手段。修改 RF 参数必须协调两端，单端切换会失联。稳定使用建议通过构建参数配置相同的信道和接入地址，并使用固定角色镜像。

## UART

主机 CDC COM 口打开并置 DTR 后，桥接所选从机 PA2 RX / PA3 TX。支持 5–8 数据位、无/奇/偶/mark/space 校验和 1/2 停止位，拒绝 1.5 停止位；波特率按分频误差不超过 2% 检查。RTS 没有硬件接线，USB SEND_BREAK 尚未转换为目标 break。

从机收发和主机 CDC 接收均为有限队列（各 256 B），RF 阻塞或电脑不读取时可能溢出。先选较低速率验证，再检查 `uart-stats`。可用以下工具保存串口数据，端口号按实际设备替换：

```powershell
python tools/capture_uart.py --port COM13 --dtr --baud 115200 --seconds 30 --output build/uart.log
```

## 连接与故障恢复

- 首次上板使用固定主机/从机镜像，检查供电、共地、晶振及 ISP 恢复入口。
- 先读取 DAP Info 和目标 DP IDCODE，再执行内存访问、下载及运行控制。
- 请求超过 1.5 秒没有结果时，目标写入可能已经发生。主机会锁定调试会话；重新枚举主机 USB 后再连接目标。
- 从机复位、断电或新会话会清除最近响应缓存；去重不跨断电保证。
- 没有 NRST，不能使用硬件复位或 connect-under-reset。Cortex-M 软件复位依赖目标 MEM-AP 可访问。
- SWJ 时钟接受 100 kHz–1 MHz 请求值，实际 GPIO 频率仍待标定。默认从 100 kHz 开始。
- 用了 `DAP_SWD_Sequence`（`0x1d`）且含读操作后，必须补一条线复位再继续普通事务：实测不补的话目标从下一条事务起就不再应答（`05 00 08`，`SWD_ERROR`），补一条 `0x12` 64 位 0xFF 就恢复（详见[排查记录 §11.4](bulk-cdc-investigation.md)）。
- 若出现“请求照样 1.5 s 超时、但 `stats` 里 `tx` 增量为 0、`tx_failures` 也为 0”的静默状态，就是 `radio_send()` 卡在 `sending`：`APPLY` 会被 `radio_is_idle()` 挡住，`SET_INTERFACE` 同传输不换代、切 HID 只能清 `fault` 治不了静默，**只能拔插主机 USB**。从机侧会卡成互补形态（主机 `tx` 正常增、`rx` 恒为 0），也需要给从机断电（[§12.4](bulk-cdc-investigation.md)）。
- **不要用 GDB `load` 刷这个目标**：实测两次运行一次失败，失败时 Flash 被擦空、CPU lockup。用 `openocd ... -c "flash write_image erase <elf>" -c "verify_image <elf>"`，已验证可用（[§12.1](bulk-cdc-investigation.md)）。
- 1 KiB 帧缓冲读取单次可用；**连续多次（约 4–24 s 内）会把整机卡成静默**，之后只能复位（[§12.5](bulk-cdc-investigation.md)）。
- 真实调试负载下 `retransmissions` 不再等于空口丢帧：15 字块读要 29–41 ms，超过 20 ms 的 RTO 下限，每个块读必然重传一次。看链路质量应改看 `crc_errors` 与 `drops`；`rejected` 也会因重传后的重复应答而增长（[§11.3](bulk-cdc-investigation.md)）。

固件 1.3.1 将正常物理 SWD 操作与额外重试预算分离，正常操作最多 48 次、额外 WAIT/匹配重试最多 32 次。模型验证合法 64 字节报文不会再因旧 32 次预算被截断；真实 WAIT/FAULT 仍可能造成部分完成。HID / Bulk 分别直连已通过下述上板测试。**Bulk 与 CDC 并发的重传预算、超时响应形状已在 1.3.2 修复并上板复测**（见[排查记录](bulk-cdc-investigation.md)）；`SET_INTERFACE` 跨接口复位仍只有原生测试覆盖。

**2026-10-24 真实 SWD 负载复测（1.3.2）：**用 `tools/swd_workload.py` 对 STM32F103C8T6 跑多事务 `DAP_Transfer` + 15 字块读写 + `DAP_SWD_Sequence` + halt/寄存器/死栈往返，三组各 120 s（Bulk 单独、Bulk+CDC @100 kHz、Bulk+CDC @1 MHz）与一次 OpenOCD/GDB 并发回放全部通过：约 2.9 万次 DAP 事务、**0 次请求超时、无锁存、无数据损坏**，其中 flash 块读逐次逐位比对、RAM 写入-回读-恢复逐字节一致。CDC 并发把 p99 从 67 抬到 146 ms。数据见[排查记录 §11](bulk-cdc-investigation.md)。

**同日的真实 GDB 复测推翻了“已彻底修好”的结论：**真 OpenOCD+GDB 跑 Flash 擦写、硬件断点、8x1 KiB 帧缓冲连读时，**连读 8 个 KiB 即复现了现场故障**（GDB 在帧缓冲读后的参数写入处报 `Cannot access memory`）。定位结果：不是重传预算耗尽，而是 `radio_send()` 的 `sending` 卡死导致的**射频静默**——失败窗口内 `tx` 增量为 0，清 `fault` 后仍死，只能复位；主机与从机两侧都能卡。硬件断点与单步两轮均通过；Flash 擦写走 OpenOCD 命令可用，GDB `load` 未通过。详见[排查记录 §12](bulk-cdc-investigation.md)。

**同日与 WCH-LinkW 的对照：**同一块板、同一份 `build/ks2.elf`、同一套工具，只换探头。同一个 8x1 KiB 帧缓冲读序列，WCH-LinkW **5/5 通过、单轮 3.1–3.4 s**；`program`+`verify` 烧录 15/15（1000 kHz 中位 14.45 s，100 kHz 15.28 s）；`--mode cycle` 为 23–29 cycle/s、p99 10–20 ms，是 CH570Q 的 3–5 倍。它自己的弱点是最高事务率下 16 轮中 5 轮出现一次 5 s 无应答，**但能自愈，不需要拔插复位**。结论：帧缓冲连读卡死是 CH570Q 的实现缺陷，不是无线调试器的共性。数据见[排查记录 §13](bulk-cdc-investigation.md)。

旧 1.3.0 从机可使用保留的 32 字节兼容桥。在一个终端运行：

```powershell
python tools/dap_budget_proxy.py
```

另一个终端运行：

```powershell
openocd -f config/openocd-budget.cfg -f target/stm32f1x.cfg -c "cortex_m reset_config sysresetreq"
```

`tools/run_openocd.py` 为旧桥配套启动器；OLED 验证工具支持 `--legacy`。更新后的默认入口使用普通 OpenOCD HID。

## 上板验证（2026-10-24）

现场从机的 DAP 固件版本返回 `1.3.2`（版本字段由**从机**镜像提供，仅刷主机时仍会显示旧版本；旧设备为 `1.3.1`/`1.3.0`，工具按 `1.3.` 前缀兼容）。测试使用 OpenOCD 0.12.0+dev-02554、STM32F103C8T6 和本仓库 OLED 示例，不使用 32 字节兼容桥。

| 测试 | 结果 |
| --- | --- |
| 原生回归 | 13 组全部通过，含 59 组 SWD GPIO 用例 |
| HID / RF 身份 | 64 B 报文、单包额度和版本检查通过 |
| SWD IDCODE | 100 / 500 / 1000 kHz 请求值各读两次，均为 `0x1BA01477`；不是物理频率测量 |
| HID、Bulk 分别直连 | Cortex-M3、device ID `0x20036410`、64 KiB Flash 识别通过；已有 OLED Flash 与本地 ELF 校验通过 |
| HID、Bulk 分别运行控制 | main / oled_update 硬件断点、单步、暂停恢复、RAM 参数读写和 1 KiB 帧缓冲读取通过 |
| CDC 单独运行 | 115200 8N1 连续接收 5 条心跳；一次详细采集 285 B，RX 收取与交付均为 285 B、队列溢出 0 |
| Bulk + CDC 并发 | 两次复现请求超时；GDB 在帧缓冲读取后的参数恢复写入处显式失败，首个异常事务尚未定位 |

并发失败后，主机故障锁定会连带拒绝后续 DAP 和远端管理，并停止 CDC；远端管理看到的 `81ff` 可能是主机合成的失败响应，不能据此判断从机死机。后一次只拔插主机 USB 即恢复远端通信。建议暂时分开使用调试与无线串口。

**后续排查补充：**三轮原 GDB / Bulk + CDC 并发回放均通过，另完成 DAP Info 和 DP IDCODE 的单独/并发对照。第二轮用新增的 `tools/bulk_cdc_stress.py` 拿到逐事务延迟分布，把根因定量到**重传预算**：`rto` 饱和到上限（单次 ≥134 ms 的首发样本即可）后，`REQUEST_TIMEOUT_MS=1500` 窗口内只剩 4 次发送，而实测每次发送丢帧率约 13.9%；CDC 并发把 p999 从 64 ms 抬到 146 ms，正是把 `rto` 顶到上限的推手。USB 复合层经独立审计确认不存在缓冲重叠、信用被 CDC 抢占或载荷损坏。已修复重传预算、超时后合成响应的命令形状错误、`SET_INTERFACE` 跨接口复位与射频侧静默失败的可观测性；修复尚未烧录验证。实测数据、命令、判别式与边界见 [Bulk + CDC 排查记录](bulk-cdc-investigation.md)。

CDC 初次启用时曾记录 8 B 丢弃和一次硬件 overrun，与启用前 FIFO 积压相符；后续接收未增加队列溢出。此结果不等于持续高吞吐无损验证。验证结束已恢复目标运行；最终 5 秒采集收到 275 B，frames 从 16 增至 40，ready=1、i2c_errors=0、fault=0。

本轮没有烧写 CH570Q、没有擦写 STM32 Flash，也没有复测配置持久化或长时间干扰稳定性；设备版本通过协议读取，未对 CH570Q Flash 做逐字节回读比对。

详细日志和 JSON 保存在本机 `build/validation-*`，由 Git 忽略：`validation-hid.json`、`validation-swd.json`、`validation-hid-final-gdb.log`、`validation-bulk-gdb.log`、`validation-bulk-cdc-retest-openocd.log`、`validation-cdc-state.json`、`validation-final-uart.log`。重建或清理构建目录前如需保留证据，应另行备份。

## 诊断与测试工具

- `tools/bulk_cdc_stress.py --port COM14`：高交错 Bulk + CDC 压测。逐事务分开记录 `write_ms`（USB NAK 反压）与 `read_ms`（空中延迟），输出延迟分位数，并在任一事务超过 `--outlier-ms` 时立即在同一 Bulk 通道回读固件计数器。`--no-cdc` 做对照，`--dp-idcode` 改用只读 DP 读取。
- `tools/bulk_cdc_diagnostic.py --port COM14`：Bulk 单独/CDC 并发对照；`--dp-idcode` 增加只读 DP 验证，`--snapshot-only` 仅采集本地 Bulk 故障状态。需要 PyUSB、libusb 后端和 WinUSB 驱动，先关闭 OpenOCD / 其他探针客户端。
- `tools/induce_timeout.py`：把主机切到另一个信道以制造一次真实 1500 ms 超时，抓取合成响应形状后切回原信道。`APPLY` 会清 `session`，所以超时落在 phase 1、不锁存 `fault`，**无需重新插拔**；可用于验证 `fail_request()` 形状与主机镜像身份。
- `tools/verify_bulk_cdc_fixes.py --port COM14`：用 `SET_INTERFACE(2)` 前后线路编码与 CDC 字节流是否存活来验证跨接口复位修复。需要能对接口 2 发控制传输的环境（Windows 上 libusb 做不到，因为在 usbser 手里）。
- `tools/hid_smoke.py --expect-wireless`：USB/DAP 基础验证。
- `tools/swd_identify.py`：读取目标 DP / MEM-AP 身份。
- `tools/swd_workload.py [--mode cycle|fb] [--port COM13] [--seconds 120] [--speed 100000] [--stop-on-error] [--vid 0x1209 --pid 0x0001]`：真实 SWD 负载压测。`cycle` 模式每周期发多事务 `DAP_Transfer`、15 字 flash 块读（满 64 字节包）与 `DAP_SWD_Sequence` 原始捕获，每 `--debug-every` 周期做一次 halt / 读 DHCSR 与 SP、PC / 死栈区块读写往返 / resume；`fb` 模式每周期连读 17 次 60 B（正好一个 1 KiB 帧缓冲，对应 GDB 的 `dump binary memory`），用于复现连续帧缓冲读卡死。每一步都校验 SWD ACK 与数据内容，并逐事务计时、离群时回读计数器并给出 `tx_per_second` 判据。`--stop-on-error` 在第一个非 OK 应答处停下并快照现场。先关闭 OpenOCD。
  `--vid/--pid/--serial/--interface` 可指向任意 CMSIS-DAP v2 探头（Bulk 端点自动识别，不再写死 `0x02`/`0x82`）；`--no-local-stats` 关掉 CH570Q 专有的 `0x80` 计数器——外来探头（如 WCH-LinkW）没有这些计数器，工具会自动降级，只失去计数器、不失去延迟与功能测量。
- `tools/flash_compare.py --probe linkw|wireless [--attempts 10]`：两探头、同一条 OpenOCD 烧录命令（与 `scripts/upload.ps1` 完全一致，含默认 `adapter speed`）的逐次基准。它**不内部重试**，只统计首烧失败率与耗时分布，所以能直接回答“第一次烧录到底会不会失败”。逐次日志与 `summary.json` 落在 `build/flash-compare/`。
- `tools/gdb_dump_stress.py --probe linkw|ch570q [--passes 5] [--dumps 8] [--size 1024]`：OpenOCD + GDB 连续 1 KiB `dump binary memory` 压力，即[§12.3](bulk-cdc-investigation.md) 那条打死 CH570Q 的命令行；换 `--probe` 就能对两个探头跑同一个脚本。证据（生成的 GDB 脚本、每轮 GDB 日志、`summary.json`）落在 `build/gdb-dump-stress/`。
- `tools/verify_oled_gdb.py [--bulk] [--script verify.gdb] [--timeout 600]`：OLED 示例的 GDB 自动验证，会复位并控制目标；运行前关闭其他 OpenOCD 实例。`--script verify-flash.gdb` 跑 Flash 擦写 + 回读比对 + 硬件断点 + 8x1 KiB 帧缓冲读，但其中的 GDB `load` 与连续帧缓冲读目前会卡死会话，见[§12](bulk-cdc-investigation.md)。
- `examples/ks2-oled/scripts/render_buffer.py`：将 1024 B OLED 页式缓冲转为 PNG。

最小主从、占位与 USB 诊断镜像默认不构建；需要时启用：

```powershell
cmake -S . -B build-diag -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/wch-riscv.cmake" -DWD_BUILD_DIAGNOSTICS=ON
cmake --build build-diag
```

完整功能固件需要板级日志时，可另外配置 `-DWD_UART_LOG_FULL=ON`。日志使用 PA3 TX / PA2 RX，115200 8N1，**日志字节会进入目标 UART 数据流**；启用前断开目标 UART。默认完整镜像禁用日志。

诊断镜像包括 `ch570q_usb_poll`、`ch570q_usb_irq`、`ch570q_usb_reference`；`ch570q_host` / `ch570q_slave` 为历史精简实现，不含完整 UART 与管理调度。`ch570q_placeholder` 仅用于工具链检查。
