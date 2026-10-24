# 无线 DAP 事务协议 v1

本协议承载 CMSIS-DAP v1 的单个请求/响应，每次最多 64 字节；仅适用于本项目主从机。PHY 与 DMA 适配见 src/radio.c，编解码及从机去重见 src/link.c。

## 帧格式

RF payload 固定 78 字节，SDK DMA 在它前面另有 2 字节硬件头：`0x55` 和 payload 长度 `78`。RX DMA 至少 264 字节，按 SDK 要求分配；硬件 CRC 错误不投递到应用层。

| payload 偏移 | 字节数 | 内容 |
| --- | ---: | --- |
| 0 | 2 | ASCII `WD` |
| 2 | 1 | 版本 `1` |
| 3 | 1 | 类型：1 HELLO，2 HELLO_ACK，3 REQUEST，4 RESPONSE；扩展为 16 DISCOVER、17 DISCOVER_REPLY |
| 4 | 4 | session，小端 uint32 |
| 8 | 4 | sequence，小端 uint32，不为 0 |
| 12 | 1 | DAP 数据有效长度，0～64 |
| 13 | 1 | 保留，必须为 0 |
| 14 | 64 | DAP 数据，未用部分必须填 0 |

编解码不通过 packed 结构体转换，避免对齐和端序依赖。会话标识使用运行计数器生成，仅用于隔离事务，不提供认证、加密或跨断电的唯一性保证。

## 握手与有序执行

1. 主机首次有 USB 请求时发送 HELLO，session=0、sequence=本次握手 nonce、length=0。
2. 从机对新的 HELLO 分配非零 session，清空最近请求/响应缓存，初始化 DAP 状态。对相同 nonce 的 HELLO 重传返回同一个 HELLO_ACK，ACK 回显 nonce。1.3.3 起，若该会话已执行数据，ACK 带 length=1、data=01，但不清缓存；新主机收到后换 nonce 重新握手，不延长原 HELLO 超时。用于处理仅主机重启时的 nonce 碰撞。未使用会话的 ACK 仍为 length=0。
3. 主机获得 ACK 后发送 REQUEST，使用该 session，sequence 从 1 开始。REQUEST 数据长度为 1～64。
4. 从机只执行当前 session 的下一个 sequence，将结果和完整请求缓存后返回 RESPONSE；响应携带相同 session、sequence。
5. 完整重复的最近请求返回缓存响应。相同序号但内容变化、跳号、过旧序号或旧会话直接丢弃，均不重新操作目标。
6. 主机收到匹配的 RESPONSE 后才提交 USB IN。USB IN 收到 ACK 后才允许下一 USB OUT，因此仅有一条 DAP 事务在途。序号溢出后重新握手。

## 超时与复位

完整固件 HELLO 和 REQUEST 采用自适应 RTO，并把间隔限制在剩余 1500 ms 阶段预算内以保留重传机会；RF 忙时保留当前请求。历史最小固件仍为固定 100 ms 重发。

- HELLO 超时：返回命令对应的失败/空结果；普通连接下一次可重新握手，fault 下发起的 Connect 若 HELLO 失败则仍保持 fault。
- REQUEST 超时：目标可能已经执行写入但响应丢失，返回失败并锁定后续目标请求。1.3.3 允许合法 `DAP_Connect`（端口 0/1）显式新建会话，只有 Connect 成功才解除该路径的门控；或本地管理命令 13 清会话后重新连接。旧 payload 不跨会话自动重放。
- 从机缓存只保留最近一次响应，且仅在本次会话和本次供电期间有效。不承诺跨板复位、断电或新握手的 exactly-once 执行。
- USB reset、SET_CONFIGURATION、DAP SET_INTERFACE 和接受的 HID/Bulk 切换会增加 generation。主机原子获取请求及其 generation，丢弃先前 USB 会话的 RF 工作和响应。CDC 数据清理并等待 DTR 关闭，防旧后缀跨 RF 会话。
- 任一板重启后应重新连接，执行 DAP Connect 和目标初始化；两块板都更新到 1.3.3 才具备完整恢复语义。
- TransferAbort（0x07）按 CMSIS-DAP 规则不返回 HID 响应，主机直接消费。当前一条在途请求和同步 SWD 执行模型不支持即时抢占。

RF ISR 处理库中断并复制单个 RX mailbox；1.3.3 的 RX 重挂由前台 `radio_poll` 执行。每轮主循环都调用看护，即使 fault 或从机待发响应阻塞也不停止。TX 超过 100 ms 进入隔离恢复，失败按 10 ms 间隔重试；确认硬件 idle、排空旧事件后才重用 DMA。从机待响应发送超过 100 ms 会释放门控并保留链路去重缓存。细节及固定 SDK ABI 约束见 [重连实现说明](reconnection.md)。

## 完整固件的扩展服务

仍使用版本 1、78 字节帧、64 字节数据区和同一条严格有序的请求/响应通路。USB HID 与 Bulk 共用 DAP 额度；CDC 服务和 DAP 共用 RF session/sequence，不能让另一种服务覆盖尚需重传的请求。已完成事务的缓存响应只执行一次目标操作或 UART TX 入队。

- 定向 HELLO：length=8，数据是所选从机硬件 ID；从机先核对自身 ID 才进入握手。length=0 保留单从机兼容行为。HELLO 重传不得清除已建立会话的最近数据缓存。
- DISCOVER：session=0、非零 sequence、length=0；DISCOVER_REPLY 回显 sequence，length=8，数据为本机 ID。发现不改变既有 DAP 状态。响应使用混合完整 ID 和轮次的退避槽，当前只扫描已配置的信道/接入地址，主机最多缓存 4 个结果。
- REQUEST 数据 0x80 为主机本地管理，不向 RF 转发；0x81 为远端管理；0x82 为内部 UART 服务。管理子命令定义于 `include/control.h`，工具为 `tools/debugger_ctl.py`。
- UART 请求：`82 baud_le32 stop parity bits flags tx_length tx_bytes`，共 10+N 字节，N≤54。flags bit0=启用桥接，bit1=主机 RX 无额度、禁止从机取走 RX，其他位拒绝。stop/parity/bits 按 CDC 值解释。响应为 `82 status accepted_tx_length rx_length rx_bytes`。主机只移除已明确接受的 TX 前缀；响应丢失时重传同一帧，从机返回缓存，不再次入队/取走数据。
- 主机另有 256 B CDC RX 队列，USB IN 忙时仍可处理 DAP；新 UART 请求在队列剩余不足 54 B 时声明零 RX 额度。DTR 关闭后先排空已经取走的主机 TX 后缀，再关闭从机 UART；如果从机仍有 RX，返回 BUSY 并继续排空，不把残留字节带入下一次打开。
- 扩展主机初始 RTO=100 ms，仅未重传的事务用于 RTT 平滑估计，RTO 下限20 ms、上限400 ms，并在重传时后退。1.5 s 阶段总等待保留；UART 不确定响应不能静默换新会话重放。1.3.3 在故障/代际失效时隔离 CDC，清理设备端旧包并等待 DTR 关闭后才恢复。
- RF 配置先暂存，APPLY 至少延迟500 ms、待本机收发/请求/串口无占用后切换，切换重建会话。角色变更仅在保存并重启后生效。双配置区保存采用 CRC、序号和最后提交标记，RX 暂停且 Flash 操作返回后才应答；失败按状态返回。

1.3.3 新增本地 HOST 命令 13 `RECONNECT`（DTR 高返回 BUSY，远端拒绝），只清本地会话、并非链路就绪确认。命令 14 `RECOVERY_STATS` 本地/远端可读：`cmd sub status version fault cdc_quarantined` + 8 个 LE32（RX 重挂失败/非 TX 的 TIMEOUT 合计、TX 看护、恢复尝试、恢复失败、恢复成功、旧事件、重连、待响应到期），version=1，有效长度38B。旧命令1的12字段不变。

1.3.3 修正超时响应布局：`05 00 08`、`06 00 00 08`（16位完成数）、`1d ff`（状态字节）；此前1.3.2文档的相反解释错误。

管理工具与故障恢复见 [使用与维护](usage.md)。模型测试覆盖协议与软件状态机；当前版本直连上板、长期稳定性和吞吐率的验证边界见 [项目说明](../README.md)。
