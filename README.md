# CH570Q 无线 ARM SWD 调试器

使用两块 CH570Q，通过自定义 2.4 GHz 链路连接电脑与 ARM SWD 目标，同时提供无线 UART。当前源码/构建固件版本 **1.3.4**，项目处于实验阶段。`DAP_Info` 的版本字段由**从机**镜像返回，不能单独确认主机版本。两块板已于 2026-10-25 完成 1.3.4 上板验收。

**1.3.4 USB 恢复入口（2026-10-25 已上板复测）：**新增独立 EP0 `usb-status` / `usb-recover`，即使 HID/Bulk 共享请求槽被旧响应占用，也可查询状态并显式清理本地会话。重置保留端点 toggle/HALT，前台确认清理完成，旧命令不重放。停止调试和 CDC 客户端后运行 `python tools/debugger_ctl.py usb-recover`，再重新连接。**实板结果**：Bulk 20 轮中 17 轮留下 RESPONSE 阻塞，均无需拔插恢复；HID 20 轮杀后均 idle，重置与后续 Info 校验通过。恢复耗时 0.6–52 ms；目标 RAM 写读另有独立测试。GDB 20/20、30 min fb 重跑通过，首次浸泡有一次未定位 EPIPE。处理中 RESET、真实 HALT/suspend 及本版烧录仍待验证。协议、操作与逐项结果见 [USB 会话恢复](docs/usb-recovery.md)与[1.3.4 上板验收](docs/v134-hardware-validation.md)。

**1.3.3 重连改进（软件验证通过，2026-10-25 已上板复测）：**新增主从独立射频发送看护、确认硬件停止及排空旧事件后恢复 RX、从机待响应门控有界退出；故障后允许通过 `DAP_Connect` 或本地 `reconnect` 建立新会话，旧写请求不会自动重放。CDC 会话换代清理旧收发数据并等待 DTR 关闭；新增 `recovery-stats`。已通过 14 个原生套件及完整/诊断固件构建。

**上板结论（2026-10-25，详见 [1.3.3 上板验收](docs/v133-hardware-validation.md)）：**1.3.2 的连续块读静默与 GDB `load` 失败本轮没有复现：fb 30 min、2214 cycle 零超时，GDB 8×1 KiB 在 100 kHz 与 1 MHz 各 20/20 轮通过，`program+verify` 10/10，GDB `load` 5/5。三组混合负载零超时，cycle 数提升约 9–23 %；CDC 组日志流量不同，不能把增幅全部归因于固件。真实 HELLO 超时响应格式正确，切回信道后恢复通信。**发送看护与 fault 锁存后重连仍缺上板证据；强杀客户端后两次失联，现场仍需拔插，具体 USB 根因待定位。**行为、命令与 SDK 约束见 [重连实现说明](docs/reconnection.md)。

**协议勘误：**此前 1.3.2 记录对超时响应的解释有误；正确的块传输错误响应为 `06 00 00 08`，原始 SWD 序列错误响应为 `1d ff`。1.3.3 已修正固件与测试，下述历史记录中的相反判断以[勘误](docs/reconnection.md#超时响应格式勘误)为准。

```text
电脑 ─ USB ─ CH570Q 主机 ─ 2.4 GHz RF ─ CH570Q 从机 ─ SWD / UART ─ ARM 目标
```

## 功能与限制

- CMSIS-DAP v1 HID、v2 Bulk / WinUSB 与 CDC ACM 复合 USB 设备。
- SWD 读写、AP posted read、WAIT/FAULT 处理；支持经 MEM-AP 发起 Cortex-M 软件复位。
- 无线 UART、设备发现与选择、RF 参数管理、统计和自适应重传。
- 固定主机、固定从机及启动时自动识别角色三种完整固件。
- 不支持 JTAG、SWO、硬件 NRST、目标电压检测、电平转换或无线 OTA。

**验证边界（2026-10-24）：**现场设备报告 **1.3.2**，已通过普通 OpenOCD 的 HID / Bulk 分别直连测试：STM32F103C8T6 身份读取、现有 OLED Flash 镜像校验、硬件断点、单步、暂停恢复和 1 KiB 帧缓冲读取。CDC 单独运行可连续接收目标日志。**Bulk 与 CDC 同时运行曾两次出现请求超时并锁定主机会话。**1.3.2 把根因定量到重传预算：`rto` 饱和后 1500 ms 窗口内只剩 4 次发送，而实测每次发送丢帧率约 10–14%；CDC 并发抬高时延尾部（p999 64 ms → 146 ms）正是把 `rto` 顶到上限的推手。已修复重传预算、超时后合成响应的格式错误、`SET_INTERFACE` 跨接口复位和射频侧静默失败的可观测性。**上板复测（2026-10-24）**：同样 120 s 交错压测下 p999 145 → 84 ms、最大 306 → 225 ms（正好落在重传收缩后的理论台阶）、吞吐 +11 %；原 GDB/OpenOCD 与 CDC 并发回放输出 `HARDWARE_VALIDATION_PASS`，累计 3 万次事务无超时、无锁存；另用信道切换人为触发三次 1500 ms 超时，合成响应分别为 `06 00 08` / `1d 00 08` / `05 00 08`，不再出现旧的 `06 00 00 08`（假成功）与 `1d ff`。详见 [并发排查记录](docs/bulk-cdc-investigation.md)。`SET_INTERFACE(2)` 隔离仍只有原生测试覆盖（Windows 上 libusb 无法对 usbser 占用的接口 2 发控制传输）。RF 配置保存仍有历史上板失败记录，未复测；长期稳定性、实际 SWCLK 频率及待机功耗仍待测量。历史 1.3.0 的目标下载验证依赖 32 字节兼容桥；本轮只校验现有目标 Flash，未执行擦写。

**真实 SWD 与 GDB 复测（2026-10-24）：**`tools/swd_workload.py` 的三组 120 s 负载（Bulk 单独、Bulk+CDC @100 kHz、Bulk+CDC @1 MHz）全绿：约 2.9 万次 DAP 事务、0 次请求超时、无 `fault` 锁存、无数据损坏（flash 块读逐次逐位比对、RAM 写入-回读-恢复逐字节一致）。但**真 OpenOCD+GDB 复测未能保持这个结论**：硬件断点、单步、单次 1 KiB 帧缓冲读通过，`flash write_image erase` + `verify_image` 通过（2728 B，10.9 s），但 **GDB `load` 两次运行一次失败**（失败时目标 Flash 被擦空、CPU lockup），且**连续 8x1 KiB 帧缓冲读在 4–24 s 内复现了现场故障**（GDB 在帧缓冲读后的参数写入处报 `Cannot access memory`）。定位：**不是重传预算**，而是 `radio_send()` 卡在 `sending` 导致的**射频静默**——失败窗口内 `tx` 增量为 0、清 `fault` 后仍死、只能复位，主机与从机两侧都能卡；另发现 `DAP_SWD_Sequence` 读序列后必须由主机补一条线复位，否则目标停止应答。**所以 1.3.2 的重传预算与响应形状修复不足以消除现场故障**，需按[§12.7](docs/bulk-cdc-investigation.md)补发送看护。详见[并发排查记录 §11–§12](docs/bulk-cdc-investigation.md)。

**与商用无线调试器的对照（2026-10-24）：**用一台 WCH-LinkW（`1a86:8012`，固件 `2.0.0`）在**同一块板、同一份 ELF、同一套工具**上重跑上述负载。结果把问题定到了实现而非品类：连续 8x1 KiB 帧缓冲读那个打死 CH570Q 的序列，WCH-LinkW **5/5 通过、单轮 3.2 s**；`--mode cycle` 吞吐是 CH570Q 的 3–5 倍（23–29 cycle/s 对 8.3）、p99 低一个数量级（10–20 ms 对 67 ms）；`program`+`verify` 烧录 15/15 全过。同时它也暴露了自己的弱点：在工具能压到的最高事务率下，16 轮里有 5 轮出现一次**请求完全不返回**的 5 秒停顿——**但它会自愈，不需要拔插复位**，而 CH570Q 一旦卡住在 `radio_send()` 就是永久静默。所以本项目的下一步不是追速度，而是让一次失败变成可重试的失败。数据与边界见[并发排查记录 §13](docs/bulk-cdc-investigation.md)。

**结论与改进评估：**见 [SWD 真实负载与 WCH-LinkW 对照总结](docs/swd-conclusions.md)：统一比较口径、当前使用边界、主要缺点的软件改进可行性及下一轮验收条件。三组 120 s 混合负载本身共 14966 次 DAP 交换；上述约 2.9 万次为该次上电后的累计完成计数。

## 硬件连接

| 引脚 | 主机 | 从机 |
| --- | --- | --- |
| PA0（脚 3） | USB D− | 目标 SWDIO |
| PA1（脚 4） | USB D+ | 目标 SWCLK |
| PA2 | 不接目标 UART | 目标 UART TX → 从机 RX |
| PA3 | 不接目标 UART | 从机 TX → 目标 UART RX |
| GND | 共地 | 与目标共地 |

板级配置采用外部 32 MHz 晶振、CL=12 pF、无外部负载电容，内部电容选择 `HSECap_12p`，系统时钟 100 MHz。移植到其他硬件前需核对 `include/board.h`、供电和 IO 电平；项目不含硬件原理图。

PA0/PA1 与 CH570Q 本机两线调试接口复用，功能固件会释放本机调试功能。请预先确认板子的 WCH ISP 烧录与恢复入口。目标须独立正确供电；没有目标供电控制或电平转换。

建议首次使用固定主机和固定从机镜像。AUTO 在启动时启用 USB 上拉并最多探测 3 秒，随后锁定角色；该过程会短暂改变从机 SWCLK 电平。独立上电后才连接电脑的 AUTO 板可能需要重新上电。

## 获取与构建

官方 SDK 以 Git 子模块固定在 `openwch/ch570` 的 `1e34c9450cd6df390153952c097cd09dd8a91aed`（EVT v1.3）。克隆本项目时使用 `git clone --recurse-submodules <本项目仓库地址>`；已克隆的目录执行：

```powershell
git submodule update --init --recursive
```

需要 CMake ≥3.21、Ninja、WCH RISC-V GCC。`riscv-none-embed-gcc`、`riscv-none-embed-objcopy` 和 `riscv-none-embed-size` 应在 PATH 中；也可以指定包含 `bin` 的工具链目录：

```powershell
cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/wch-riscv.cmake"
# 工具链不在 PATH 时，在上行追加："-DWCH_GCC_ROOT=C:/path/to/wch-toolchain"
cmake --build build
```

默认仅生成以下完整功能镜像，同时输出 ELF、BIN、MAP：

| HEX | 用途 |
| --- | --- |
| `build/ch570q_host_full.hex` | 固定 USB 主机 |
| `build/ch570q_slave_full.hex` | 固定 SWD 从机，启动不探测 USB |
| `build/ch570q_auto.hex` | 启动探测角色 |

使用 WCH ISP 按地址 **0** 烧录。应用区域为 `0x00000–0x39FFF`（232 KiB），`0x3A000–0x3BFFF` 预留配置双记录；`0x3C000` 起为芯片厂商区域。烧录工具的全片擦除设置可能清除已保存配置。项目没有驻留 Bootloader 或运行中 USB 更新客户端。

默认 RF 信道 16、接入地址 `0x8E89BED6`、2 Mbps PHY、0 dBm；不使用 BLE 协议栈。可通过 `-DWD_RF_CHANNEL=16 -DWD_RF_ADDRESS=0x8E89BED6` 配置，同一对设备必须一致。多对设备应分配不同参数。

项目兼容 WCH GCC 8.2.0：CMake 在构建目录生成 SDK 的 `memcpy` 适配副本，不修改上游。链接器至少预留 2 KiB 栈，该预留不代表已测量栈峰值。

## 使用

先给从机及目标供电，再将主机连接电脑。USB 名称为 `CH570Q Wireless CMSIS-DAP`。HID 的基础 OpenOCD 入口：

```powershell
openocd -f config/openocd-probe.cfg -f target/stm32f1x.cfg -c "cortex_m reset_config sysresetreq"
```

根据实际目标更换 target 配置。Bulk 使用 `config/openocd-bulk.cfg`，要求操作系统已正确绑定 WinUSB / libusb 驱动。直连实测与并发限制见 [使用与维护](docs/usage.md)；旧 1.3.0 设备仍需兼容入口。

无线串口通过主机 CDC COM 口使用，打开串口并置 DTR 后转发所选从机 PA2/PA3。端口号由操作系统分配；完整功能镜像默认关闭这两个引脚上的诊断日志。实际持续吞吐受 RF 与有限队列限制。

USB VID `0x1209` / PID `0x0001` 是开发占位值，序列号暂固定为 `CH570Q-0001`；公开分发硬件产品前应分配有权使用的标识及唯一序列号。无线发现与会话号不提供认证或加密。

## 开发与测试

原生测试需要 PowerShell 7、Windows GCC / G++，无需连接硬件：

```powershell
pwsh -NoProfile -File tests/run.ps1
```

测试覆盖 DAP、SWD GPIO 模型、USB IRQ/轮询、RF 去重、UART、重传、配置与应用调度。模型通过不代表电气时序或空口性能已验证。

- `src/`、`include/`：固件；默认完整功能入口为 `src/app.c`。
- `cmake/`、`config/`：工具链与 OpenOCD 配置。
- `tools/`：设备管理、兼容桥与验证工具；Python 依赖见 `tools/requirements.txt`。
- `tests/`：原生测试与寄存器模拟。
- [使用与维护](docs/usage.md)：管理工具、故障恢复、诊断构建。
- [无线协议](docs/protocol.md)：事务与扩展服务定义。
- [STM32 OLED 示例](examples/ks2-oled/README.md)：可独立构建的目标固件及 VS Code 调试入口。

## 许可证

本项目原创固件、工具、测试、配置与文档采用 [MIT License](LICENSE)。[第三方声明](THIRD_PARTY_NOTICES.md) 列出了例外：官方 WCH SDK 保持上游许可，STM32 OLED 示例采用 GPL-3.0-or-later。SDK 中的 RF / ISP 库仅提供预编译形式。
