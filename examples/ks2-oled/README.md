# STM32F103 OLED 调试示例

独立运行在 STM32F103C8T6 的 SSD1306 128×64 动画固件，用于验证 CH570Q 无线 SWD 与 UART。包含启动、链接、寄存器支持和精简 OLED 驱动，不依赖 STM32 HAL 或 FreeRTOS。

## 接线

| 目标引脚 | 连接 |
| --- | --- |
| PA13 / SWDIO | CH570Q 从机 PA0 |
| PA14 / SWCLK | CH570Q 从机 PA1 |
| PA9 / UART1 TX | CH570Q 从机 PA2 RX |
| PA10 / UART1 RX | 可选接 CH570Q 从机 PA3 TX；本例不处理串口命令 |
| PB10 | OLED SCL |
| PB11 | OLED SDA |
| 3V3 / GND | OLED 供电；目标与从机共地 |

OLED 使用 7 位地址 `0x3C`，软件 I2C 需要上拉至 3.3 V。KS2 板的 U27 pin 1 为独立 `DBG_PWR` 网络，不能当作目标 3V3 或电压参考；目标从实际 3.3 V 电源轨供电。OLED 接头 H1 的 1/2/3/4 脚分别为 GND、3V3、SCL、SDA。

## 构建与调试

PATH 需要 CMake、Ninja、GNU Arm Embedded 工具链和 OpenOCD：

```powershell
cmake -S . -B build -G Ninja
cmake --build build
openocd -f ch570q.cfg -c "program build/ks2-oled.elf verify reset exit"
```

输出 ELF、HEX 和 BIN，BIN 基地址 `0x08000000`。按 64 KiB Flash 器件处理，链接器只允许使用前 62 KiB，最后两页保留；整片擦除仍会清除保留区中的旧数据。

VS Code 直接打开本目录，安装推荐扩展：`Ctrl+Shift+B` 构建，任务 `flash` 下载并校验，`debug-server` 启动 OpenOCD，随后 F5 连接。`verify-hardware` 自动执行 GDB 验证，会复位和控制目标；不可与其他 OpenOCD 实例同时运行。

默认配置使用标准 CMSIS-DAP HID，要求 CH570Q 从机更新至 1.3.1；该版本已通过编译与模型测试，直连上板验证尚未完成。历史 1.3.0 使用 32 字节兼容桥已验证下载、Flash 校验、断点、单步及 UART。旧设备入口见项目根目录的 [使用说明](../../docs/usage.md)。

## 运行与观察

- 内部 HSI / 2 × 16 得到标称 64 MHz，PLL 失败回落 8 MHz。
- UART1 输出 115200 8N1 启动日志与每秒心跳；通过主机 CDC 串口观察，端口号按系统分配。
- OLED 显示帧号、秒数、反弹方块和进度条；未接 OLED 时仍输出日志并重试初始化。
- `g_frame_count`、`g_uptime_ms`、`g_oled_ready`、`g_i2c_errors`、`g_uart_errors` 和 `g_fault` 可用于观察状态。
- 修改 `g_animation_pause` 可暂停动画；`g_frame_delay_ms` 控制帧间隔（10–200 ms）。
- `g_oled_buffer` 为 1024 B 页式像素缓存。使用 `scripts/render_buffer.py` 将导出的缓冲渲染为 PNG；图像是 RAM 内容重建。

## 许可证

本示例整体采用 **GPL-3.0-or-later**，条款见 [COPYING](COPYING)。SSD1306 初始化和页面寻址精简自 KS2 OLED 驱动，保留 Guilherme Amorim / Renan Guedes (2020) 署名。示例单独构建，不链接到 CH570Q 固件。
