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

固件 1.3.1 将正常物理 SWD 操作与额外重试预算分离，正常操作最多 48 次、额外 WAIT/匹配重试最多 32 次。模型验证合法 64 字节报文不会再因旧 32 次预算被截断；真实 WAIT/FAULT 仍可能造成部分完成。该版本直连上板验证尚未完成。

旧 1.3.0 从机可使用保留的 32 字节兼容桥。在一个终端运行：

```powershell
python tools/dap_budget_proxy.py
```

另一个终端运行：

```powershell
openocd -f config/openocd-budget.cfg -f target/stm32f1x.cfg -c "cortex_m reset_config sysresetreq"
```

`tools/run_openocd.py` 为旧桥配套启动器；OLED 验证工具支持 `--legacy`。更新后的默认入口使用普通 OpenOCD HID。

## 诊断与测试工具

- `tools/hid_smoke.py --expect-wireless`：USB/DAP 基础验证。
- `tools/swd_identify.py`：读取目标 DP / MEM-AP 身份。
- `tools/verify_oled_gdb.py`：OLED 示例的 GDB 自动验证，会复位并控制目标；运行前关闭其他 OpenOCD 实例。
- `examples/ks2-oled/scripts/render_buffer.py`：将 1024 B OLED 页式缓冲转为 PNG。

最小主从、占位与 USB 诊断镜像默认不构建；需要时启用：

```powershell
cmake -S . -B build-diag -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/wch-riscv.cmake" -DWD_BUILD_DIAGNOSTICS=ON
cmake --build build-diag
```

完整功能固件需要板级日志时，可另外配置 `-DWD_UART_LOG_FULL=ON`。日志使用 PA3 TX / PA2 RX，115200 8N1，**日志字节会进入目标 UART 数据流**；启用前断开目标 UART。默认完整镜像禁用日志。

诊断镜像包括 `ch570q_usb_poll`、`ch570q_usb_irq`、`ch570q_usb_reference`；`ch570q_host` / `ch570q_slave` 为历史精简实现，不含完整 UART 与管理调度。`ch570q_placeholder` 仅用于工具链检查。
