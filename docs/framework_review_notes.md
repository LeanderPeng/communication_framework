# 通讯框架复习与实现说明

这套框架负责把连续字节流转换成可供应用处理的完整消息，再把请求、应答、超时和业务模型更新串成一条清晰的数据链路。阅读目标是能够沿着源码说明“一次状态查询如何发出去、响应如何回来、哪里会失败、失败后谁负责处理”，并能把相同职责迁移到真实项目。

**当前结论：通讯框架第一阶段已完成。** 当前版本作为后续项目接入的基础，不再把增加模拟场景、单独完成 UART 或 RTOS 适配作为本仓库阶段收尾的前提。实际协议适配、驱动接入及硬件验证，放到正式项目和电源板上完成。LVGL 9 与页面导航框架将在独立仓库开展。

本文保留关键代码片段，其余实现通过完整路径和源码链接定位。既适合作者复习，也适合接手者从头熟悉实现。

| 项目 | 内容 |
| --- | --- |
| 记录日期 | 2026 年 10 月 7 日 |
| 源码基准 | [1ea827393746046450dfab424d99f9a85e229fd0](https://github.com/LeanderPeng/communication_framework/tree/1ea827393746046450dfab424d99f9a85e229fd0) |
| Linux 仓库根目录 | /home/rain/communication_framework |
| Windows 共享目录 | `\\192.168.0.66\rain\communication_framework` |
| 本笔记路径 | /home/rain/communication_framework/docs/framework_review_notes.md |
| 当前实现语言 | 可移植核心为 C99；Linux 适配使用 pthread |
| 框架与 GUI 的关系 | 通讯生成业务事件和模型；GUI 在自己的任务中消费数据并刷新 |
| 阅读链接约定 | 源码链接固定到上述提交，行号不会随后续提交漂移；本机路径用于编辑器或服务器定位 |

## 1 如何使用这份笔记

### 1.1 快速复习路线

已经读过框架时，先看第 2 节总链路、第 11 节请求事务、第 13 节完整调用过程，以及第 15 节线程和内存归属。能回答下面几个问题，就找回了主要结构：

1. 字节在哪一层变成完整帧？
2. TX 队列和 pending 为什么需要同时存在？
3. 回复按什么字段匹配请求，匹配后先释放还是先回调？
4. 哪个任务调用 Message Manager，超时由谁推动？
5. 业务数据在哪一层校验，哪一层才能决定刷新 UI？
6. 断线取消、最终超时、业务拒绝分别意味着什么？

### 1.2 新接手者阅读路线

先按本文顺序理解职责，再按以下顺序打开源码：

| 顺序 | 文件 | 阅读重点 |
| --- | --- | --- |
| 1 | [/home/rain/communication_framework/include/comm_frame.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame.h) | 线路格式与内存帧的区别 |
| 2 | [/home/rain/communication_framework/src/comm_codec.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_codec.c) | 完整帧如何编码和校验 |
| 3 | [/home/rain/communication_framework/src/comm_ringbuffer.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_ringbuffer.c) | 未消费字节如何保存 |
| 4 | [/home/rain/communication_framework/src/comm_parser.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_parser.c) | 如何从任意字节分段中提取帧 |
| 5 | [/home/rain/communication_framework/src/comm_frame_queue.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_frame_queue.c) | 完整帧如何复制和排队 |
| 6 | [/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c) | 线程等待、唤醒及关闭 |
| 7 | [/home/rain/communication_framework/src/comm_frame_channel.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_frame_channel.c) 与 [/home/rain/communication_framework/ports/linux/comm_frame_channel_pthread.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_channel_pthread.c) | 如何隔离平台队列 |
| 8 | [/home/rain/communication_framework/src/comm_message_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c) | sequence、pending、回复匹配和重试 |
| 9 | [/home/rain/communication_framework/examples/appliance_manager/appliance_client.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_client.c) 与 [/home/rain/communication_framework/examples/appliance_manager/appliance_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.c) | 从通用消息进入产品业务 |
| 10 | [/home/rain/communication_framework/examples/linux_runtime/main.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c) | 如何组装成真正运行的多任务程序 |

每个模块先看头文件里的公开约定，再看实现，最后用对应测试理解边界。不要一开始就在 Linux 示例的线程和模式分支中寻找协议算法。

## 2 框架完成了什么

### 2.1 实现范围

| 层次 | 当前已经实现的能力 | 需要注意的边界 |
| --- | --- | --- |
| 协议帧 | 双字节同步头、版本、类型、序号、长度、payload、CRC16 | 当前是一套示例协议，不等于某块电源板协议 |
| Codec | 一帧的编码与解码、长度及 CRC 校验 | 不直接处理连续流中的拆包和粘包 |
| RingBuffer | 固定容量字节缓存、绕回、读取、窥视和丢弃 | 不自带并发锁，不自动覆盖旧数据 |
| Parser | 增量提帧、半帧保留、多帧处理、坏帧重同步 | 不理解业务，不自带半帧等待超时 |
| 帧队列 | 固定容量、整帧复制、先进先出 | 核心队列不自带锁和阻塞 |
| Linux 队列 | mutex、条件变量、超时、关闭唤醒 | pthread 代码不应直接移入 RTOS 核心 |
| Channel | 统一帧队列接口与结果码 | 是帧通道抽象，不是 UART/TCP 字节传输抽象 |
| Message Manager | 请求登记、序号分配、回复匹配、有限重试、最终超时 | 不内建线程，不直接读取系统时钟，不认识产品字段 |
| 业务示例 | 查询状态、校验完整快照、模型更新、变化通知 | 洗衣机字段是示例，应由具体产品替换 |
| Linux 运行示例 | 实际收发线程、模拟设备、状态同步、停止和模拟重连 | 使用本地 socketpair，未连接真实串口或电源板 |
| 可选诊断 | 解析错误、队列积压和事务结果等统计 | 辅助日志定位，不是业务正常运行的前提 |

当前没有实现 LVGL 页面、导航框架、真实 UART 驱动、RTOS 任务适配、实际电源板协议映射。这些属于正式项目集成边界，不能把已有模拟运行解释为硬件兼容性已经验证。

### 2.2 一条链路中的五种数据

| 数据 | 代表什么 | 主要存在位置 |
| --- | --- | --- |
| 原始字节 | 线路上收到的任意一段数据，可能不足一帧 | 传输读取缓冲、RingBuffer |
| comm_frame_t | 已经分离出来的一条逻辑通信帧 | Codec 输出、TX/RX 队列 |
| pending | 一条已投递、尚未得到最终结果的本端请求事务 | Message Manager |
| comm_message_event_t | 已分类的请求、回复、上报、错误或超时事件 | manager 同步回调 |
| appliance_model_t | 可供业务和 UI 使用的设备状态快照 | Appliance Manager |

从字节到帧，解决的是协议边界；从帧到事件，解决的是事务关系；从事件到模型，解决的是业务含义。某一层成功不能替代后一层成功。

### 2.3 发送和接收总链路

```text
发送：
业务查询
  → Appliance Client 生成业务 payload
  → Message Manager 分配 sequence 并投递请求
  → TX Channel → 线程安全 TX 帧队列
  → 发送线程取出完整帧
  → Codec 编码
  → 字节传输

接收：
字节传输
  → 接收线程读取任意一段字节
  → RingBuffer 保存未消费字节
  → Parser 寻找并验证完整帧
  → RX Channel → 线程安全 RX 帧队列
  → manager 所属任务取出帧
  → Message Manager 分类和匹配
  → Appliance Manager 校验业务字段并更新模型
  → 通知应用或 UI 适配层
```

接收侧的 Message Manager 不会自己从 RX 队列取帧。取帧和调用 handle_frame 是外层运行任务的工作。这个区别决定了 core 能否脱离具体操作系统复用。

## 3 分层和文件组织

| 目录完整路径 | 内容 | 正式项目中的处理方式 |
| --- | --- | --- |
| /home/rain/communication_framework/include | 对外结构、接口及契约 | 随可移植核心引入 |
| /home/rain/communication_framework/src | 不依赖 pthread、RTOS、LVGL 的核心实现 | 保持产品无关 |
| /home/rain/communication_framework/ports/linux | pthread 帧队列及 Channel 绑定 | Linux 项目可参考或使用 |
| /home/rain/communication_framework/ports/rtos | 当前只有占位文件 | 按目标 RTOS 实现 |
| /home/rain/communication_framework/examples/appliance_manager | 业务协议、查询封装、模型管理 | 按产品字段替换 |
| /home/rain/communication_framework/examples/linux_runtime | 多任务组装、模拟传输和场景入口 | 参考生命周期与任务边界，不原样当硬件驱动 |
| /home/rain/communication_framework/tests | 13 个现有测试程序 | 用于理解已有接口和异常语义 |
| /home/rain/communication_framework/docs | 路线记录和复习文档 | 与对应代码版本一起维护 |

特别注意：Linux 示例里的 send/recv 目前直接写在运行示例中，并没有一个已经完成的通用字节传输接口。不能把 comm_frame_channel_t 当作串口驱动接口。

## 4 协议帧和线路格式

代码定义：[/home/rain/communication_framework/include/comm_frame.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame.h)。

### 4.1 线路字段

| 起始偏移 | 长度 | 字段 | 当前定义 |
| --- | --- | --- | --- |
| 0 | 1 字节 | 同步字节 0 | 0xAA |
| 1 | 1 字节 | 同步字节 1 | 0x55 |
| 2 | 1 字节 | version | 0x01 |
| 3 | 1 字节 | type | REQUEST=1、RESPONSE=2、REPORT=3、ERROR=4 |
| 4 | 2 字节 | sequence | 大端序 |
| 6 | 2 字节 | payload_length | 大端序，只表示 payload 长度 |
| 8 | N 字节 | payload | 具体业务数据 |
| 8+N | 2 字节 | CRC16 | 大端序 |

固定头为 8 字节，CRC 为 2 字节，因此线路总长度为 **10 + payload_length**。默认最大 payload 为 256 字节，最大编码帧长为 266 字节。即使 payload 为空，一帧也占 10 字节。

COMM_FRAME_MAX_PAYLOAD_SIZE 可以在编译时配置，但整个程序中所有涉及该结构的编译单元必须保持一致，否则结构体大小和存储布局会不一致。

### 4.2 内存中的帧不是线路布局

comm_frame_t 保存 version、type、sequence、payload_length 和固定大小的 payload 数组。它没有把 AA55 和 CRC 存成业务字段。

不能直接把 comm_frame_t 的内存发送出去，原因有三点：

- 多字节整数在 CPU 内存中的字节序不一定符合线路约定。
- 编译器可能给结构体插入填充。
- payload 数组容量固定，而有效负载长度由 payload_length 决定。

同理，sizeof(comm_frame_t) 不能用作线路帧长。编码器只编码有效 payload，并主动添加同步头和 CRC。

### 4.3 CRC 的计算范围

当前使用 CRC-16/CCITT-FALSE，参数为多项式 0x1021、初始值 0xFFFF、最终异或 0x0000，代码按最高位向左推进。

参与 CRC 的范围是 **version、type、sequence、payload_length、payload**，不包括 AA55，也不包括 CRC 自身。范围或大小端任一项不一致，都可能出现“帧看起来正确但 CRC 一直失败”。

实现入口：[/home/rain/communication_framework/src/comm_codec.c:34](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_codec.c#L34) 中的 comm_crc16_ccitt_false，编码和解码共用同一算法。

### 4.4 查询与回复的字节示例

一次 sequence=1 的状态查询，payload 只有业务 ID 0x02：

```text
AA 55 | 01 | 01 | 00 01 | 00 01 | 02 | CRC_H CRC_L
同步头 版本 请求    序号     长度   查询ID
```

若响应表示“运行中、快洗、进度 40%、剩余 30 分钟、门锁定、无故障”，payload 为：

```text
01 02 02 28 00 1E 01 00 00
│  │  │  │  └─┬─┘ │  └─┬─┘
│  │  │  │   剩余30 门锁  故障0
│  │  │  进度40
│  │  快洗
│  运行中
状态快照ID
```

响应外层 type 为 RESPONSE，sequence 仍为 1，payload_length 为 9，总线路长度为 19 字节。CRC_H/CRC_L 在这里是占位表示，不是可直接发送的十六进制数据。

业务字段定义：[/home/rain/communication_framework/examples/appliance_manager/appliance_protocol.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_protocol.h)。

## 5 Codec 如何保证完整帧转换可靠

接口：[/home/rain/communication_framework/include/comm_codec.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_codec.h)。

实现：[/home/rain/communication_framework/src/comm_codec.c:56](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_codec.c#L56) 的 comm_codec_encode；[/home/rain/communication_framework/src/comm_codec.c:119](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_codec.c#L119) 的 comm_codec_decode。

### 5.1 编码顺序

编码先检查指针、版本、消息类型、payload 上限和输出容量。检查通过后才写 output，再计算 CRC，最后填入实际编码长度。

返回成功表示“已经生成可发送字节”，不表示字节已经交给驱动或设备。encoded_size 指针有效时，编码失败会把它写成 0；output 不会在正常参数校验失败路径中被写成半帧。

输出空间由调用者提供，函数不申请动态内存。

### 5.2 解码顺序

解码检查：最小长度 → 同步字 → 版本 → type → payload 上限 → 总长度严格一致 → CRC。全部通过后，才把临时 decoded_frame 一次性赋给调用者的 frame。

因此解码失败不会留下“版本更新了，但 payload 还是上次数据”的半更新结果。

decode 要求输入恰好是一整帧。给它半帧、两帧粘在一起或者带垃圾前缀的数据，它不会替调用者切分；切分由 Parser 负责。

### 5.3 业务合法性不在 Codec 中判断

Codec 可以确认 payload 的字节没有被当前校验规则拒绝，却不知道“进度 101%”是否合理。业务字段错误应该在 Appliance Manager 中被拒绝，不能把业务规则硬塞进通用 Codec。

## 6 RingBuffer 如何保存字节流

接口：[/home/rain/communication_framework/include/comm_ringbuffer.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_ringbuffer.h)。

实现：[/home/rain/communication_framework/src/comm_ringbuffer.c:109](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_ringbuffer.c#L109) 的 write；[/home/rain/communication_framework/src/comm_ringbuffer.c:163](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_ringbuffer.c#L163) 的 peek；[/home/rain/communication_framework/src/comm_ringbuffer.c:219](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_ringbuffer.c#L219) 的 discard；[/home/rain/communication_framework/src/comm_ringbuffer.c:248](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_ringbuffer.c#L248) 的 read。

### 6.1 四个运行量

| 字段 | 含义 |
| --- | --- |
| capacity | 底层字节数组容量 |
| read_index | 下一次读取的位置 |
| write_index | 下一次写入的位置 |
| used | 当前有效字节数 |

read_index 等于 write_index 时，可能是空，也可能是满。used 用来消除这个歧义，所以 capacity 个字节都可以使用，无须空出一个位置。

正常状态应满足下标在容量内、used 不超过容量、写位置与“读位置向前移动 used”一致。代码会检查这些不变量，避免在对象状态损坏时继续复制数据。

### 6.2 绕回只改变复制方式

写入区域跨过数组尾部时，拆成尾部一段和头部一段复制。读取、peek 也采用相同思路。

例如容量为 8、写位置为 6，写入 4 字节时，先写位置 6 和 7，再写位置 0 和 1。逻辑数据依然连续，并没有被当作两条消息。

### 6.3 四种操作的区别

| 操作 | 是否复制字节 | 是否消费已有数据 |
| --- | --- | --- |
| write | 从外部复制到环形缓冲 | 不消费旧数据，增加 used |
| peek | 从环形缓冲复制到外部 | 不消费 |
| discard | 不复制 | 消费指定数量 |
| read | 先 peek，再 discard | 消费指定数量 |

Parser 先 peek 看头部和候选帧，只有决定接受或丢弃后才消费。这样半帧可以一直保留，等待下一次输入。

### 6.4 容量不足是整次失败

write 空间不足时返回 INSUFFICIENT_SPACE，不会只写一部分，也不会覆盖最旧数据。调用者不能假设“给它多少，它会尽可能吞下多少”。

Linux 示例先解析已有字节，再查询 free_space，最多读取剩余空间允许的字节数。对应代码：[/home/rain/communication_framework/examples/linux_runtime/main.c:199](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L199) 的 receive_frame。

RingBuffer 自身不加锁。本项目让接收任务独占它；若以后改为中断/DMA 生产、任务消费，需要另行设计所有权与同步，不能直接按当前单任务用法共享。

## 7 Parser 如何处理拆包粘包和坏帧

接口：[/home/rain/communication_framework/include/comm_parser.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_parser.h)。

核心循环：[/home/rain/communication_framework/src/comm_parser.c:41](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_parser.c#L41) 的 comm_parser_next_internal。

### 7.1 一次 next 的工作

1. 检查 RingBuffer 和 scratch 都能容纳最大合法编码帧。
2. 从当前读位置寻找第一个同步字节 AA。
3. 若只剩 AA，保留它并返回 NEED_MORE_DATA。
4. 检查后续字节是否为 55；不匹配则丢掉一个字节继续寻找。
5. 固定头不完整时返回 NEED_MORE_DATA。
6. 读取长度；超过 payload 上限则拒绝这个候选头，前进一个字节重新寻找。
7. 候选整帧尚未到齐时返回 NEED_MORE_DATA。
8. 将候选帧复制到连续 scratch，调用 Codec 解码。
9. 解码成功则消费恰好一帧，返回 FRAME_READY。
10. 解码失败则只丢掉候选起点的一个字节，再寻找同步头。

它是增量解析器，但没有额外保存一份 Parser 状态对象。未完成的解析状态主要体现为 RingBuffer 中尚未消费的字节。

### 7.2 为什么坏帧只前进一个字节

错误的长度或偶然出现的 AA55，可能把后面的真实帧包进“候选帧”范围。一次丢弃整个候选长度，会顺便删掉真实帧。

逐字节重新同步保留了在坏候选内部发现下一帧的机会。这里处理的是流中的噪声或损坏，不应因此直接重启业务状态机。

### 7.3 一次成功只返回一帧

即使 RingBuffer 中有三条完整帧，一次 next 也只提取一条。外层继续调用，直到 NEED_MORE_DATA，才能消费所有已有完整帧。

Linux 示例通过 receive_frame 的下一次调用继续解析缓存；它会先调用 Parser，再尝试 recv，因此已经留在 RingBuffer 中的完整帧不会等待下一批线路数据才被处理。

### 7.4 NEED_MORE_DATA 不表示没有做任何事

Parser 可能已经丢掉垃圾前缀，只是剩下的合法候选尚不完整。不能把 NEED_MORE_DATA 解释为“输入完全没被处理”，也不能因此 reset RingBuffer。

### 7.5 当前边界

- RingBuffer 容量和 scratch 容量至少为 COMM_FRAME_MAX_ENCODED_SIZE，默认 266 字节。
- Parser 不访问队列、不更新 pending、不改变业务模型。
- Parser 没有自己的时钟。一个长度合法但迟迟未完成的候选，会等待后续字节；不会自动因为过了若干毫秒而清空。
- 断线时丢弃半帧属于运行层的连接生命周期策略。
- 串口项目是否需要半帧超时、驱动缓冲清理或噪声处理，需要在实际输入条件下确定。


## 8 完整帧队列为什么单独存在

接口：[/home/rain/communication_framework/include/comm_frame_queue.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame_queue.h)。

实现：[/home/rain/communication_framework/src/comm_frame_queue.c:143](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_frame_queue.c#L143) 的 push_internal 与 [/home/rain/communication_framework/src/comm_frame_queue.c:183](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_frame_queue.c#L183) 的 pop_internal。

RingBuffer 存的是字节，解决半帧与分段输入；帧队列存的是 comm_frame_t，解决模块或任务之间的完整消息交接。两者不能互相替代。

### 8.1 整帧复制带来的所有权边界

入队的关键动作是：

```c
queue->storage[queue->write_index] = *frame;
```

这里复制整个 comm_frame_t，而不是保存 frame 指针。因此 push 成功后，调用者可以复用原来的局部变量。pop 同样复制到输出对象，出队后也不依赖队列槽位中的内容。

代价是每个队列槽位都容纳最大 payload 对应的完整结构体，即使当前消息只有 1 字节 payload。这个选择换来了明确的生命周期和固定内存上界，现阶段没有引入共享数据块、引用计数或零拷贝机制。

### 8.2 核心队列只处理存储

核心 push 满时返回 FULL，pop 空时返回 EMPTY，均不阻塞。FIFO 顺序和绕回使用与 RingBuffer 类似的 read_index、write_index、used 维护，但容量单位是“帧”。

它不检查 CRC，也不会替调用者验证业务 payload。队列只保证按照接口约定复制和保存帧。

reset 清空逻辑队列，不代表发生了正常出队，也不自动清除外部统计。不要在 pthread 线程正在访问时绕过包装层直接 reset 底层 core。

## 9 pthread 队列怎样实现等待和退出

接口：[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.h)。

初始化：[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:106](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L106)。

入队与出队：[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:309](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L309)、[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:459](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L459)。

关闭与销毁：[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:253](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L253)、[/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:600](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L600)。

### 9.1 mutex 保护数据 条件变量等待条件

| 对象 | 作用 |
| --- | --- |
| mutex | 保护 core、closed 和启用的队列统计 |
| not_empty | 队列为空时，消费者等待可用帧 |
| not_full | 队列已满时，生产者等待空槽位 |
| closed | 持久保存关闭状态，使等待者醒来后知道应该退出 |

条件变量不存储一条“永久有效的通知”。真正的条件是队列是否为空、是否已满、是否关闭，这些状态必须在同一把锁下检查和修改。

### 9.2 为什么一定用 while 等待

等待线程可能虚假唤醒，也可能在醒来后输掉资源竞争。例如两个消费者都醒来，但只有一帧，第二个拿到锁时仍应继续等待。

因此逻辑是“在尚未关闭且条件不满足时反复等待”，而不是检查一次后无条件出队。

一次入队后 signal not_empty，一次出队后 signal not_full。close 则 broadcast 两个条件变量，因为生产者和消费者都可能需要退出。

### 9.3 三种等待方式

| timeout_ms | 语义 |
| --- | --- |
| 0 | 不进入条件变量等待；立即尝试，条件不满足返回 TIMEOUT |
| 普通正数 | 有限等待，使用 CLOCK_MONOTONIC 计算截止时间 |
| UINT32_MAX | WAIT_FOREVER，直到条件满足或队列关闭 |

有限等待的绝对截止时间只计算一次。虚假唤醒后不会重新获得完整 timeout_ms，否则一次“等待 20 ms”可能被不断延长。

零等待也仍然要取得 mutex，不是整个调用绝不会受调度或锁竞争影响。有限等待的实际返回时间同样可能因为调度而晚于截止时刻，不能把它当作硬实时保证。

### 9.4 close 与 destroy 不能混为一谈

close 的含义：

- 拒绝新的 push。
- 唤醒所有入队和出队等待者。
- pop 仍允许排空已经入队的帧；队列关闭且为空时才返回 CLOSED。
- 不销毁 mutex，不释放调用者的 storage。

destroy 的前提是已经没有任何使用者或等待者。正常顺序是 close → 等相关任务退出 → destroy。

Linux 运行示例选择“优先停止，不继续排空”的策略；这是运行层的选择，不是队列接口取消了关闭后可排空的能力。

### 9.5 初始化失败也需要回收

初始化会依次创建 mutex、条件变量属性、not_empty 和 not_full。中途任何一步失败，都回收此前成功创建的对象。

不能只验证“初始化全部成功时能跑”，而忽略第二个条件变量失败等部分完成状态。Linux 运行示例在线程创建和 socket 创建阶段也采用同样的资源回收思路。

## 10 Channel 如何隔离平台

接口和结构：[/home/rain/communication_framework/include/comm_frame_channel.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame_channel.h)。

转发实现：[/home/rain/communication_framework/src/comm_frame_channel.c:20](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_frame_channel.c#L20)。

pthread 绑定：[/home/rain/communication_framework/ports/linux/comm_frame_channel_pthread.c:72](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_channel_pthread.c#L72)。

Channel 保存一个 context 和 push、pop、close 三个函数指针。Message Manager 只调用 Channel，不需要知道 context 实际指向 pthread 队列还是某种 RTOS 队列。

### 10.1 Channel 本身不拥有资源

bind 保存地址，不初始化后端，也不复制、申请或释放后端对象。因此：

- 后端队列必须先初始化，才能绑定 Channel。
- 后端必须活得比所有 Channel 调用都久。
- Channel 的线程安全能力来自后端；函数指针本身不会自动加锁。
- manager 保存 TX Channel 的地址，但不负责替它 close 或 destroy。

### 10.2 结果码分层映射

| 运行情况 | pthread 队列 | Channel | Message Manager |
| --- | --- | --- | --- |
| 操作成功 | OK | OK | OK |
| 空或满导致等待失败 | TIMEOUT | TIMEOUT | CHANNEL_TIMEOUT |
| 队列已关闭 | CLOSED | CLOSED | CHANNEL_CLOSED |
| 后端对象或系统同步异常 | 相应错误码 | BACKEND_ERROR | CHANNEL_ERROR |

TIMEOUT 与 CLOSED 要保留区别：前者可能稍后重试，后者应该进入停止或重新建立连接的流程。不能把所有非零返回都当作“再发一次就行”。

这里抽象的是**完整帧队列**。真正的 UART 字节发送、DMA 完成通知、串口打开关闭并不在 Channel 的职责内。

## 11 Message Manager 如何管理请求事务

接口和数据结构：[/home/rain/communication_framework/include/comm_message_manager.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_message_manager.h)。

完整实现：[/home/rain/communication_framework/src/comm_message_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c)。

### 11.1 TX 队列和 pending 是两个不同账本

TX 队列记录“等待发送线程取走的帧”。pending 记录“已成功交给发送通道、仍等待最终响应的请求事务”。

一帧被发送线程出队后，TX 队列已经没有它，但 pending 必须继续保留原请求，才能判断收到的回复属于谁，也才能在超时时重发。

REPORT、RESPONSE 和 ERROR 发送一般只需要进入 TX 队列，不建立本端请求 pending；只有 send_request 登记新的请求事务。

### 11.2 pending 中的字段

| 字段 | 意义 |
| --- | --- |
| request | 初次发送的完整 REQUEST 副本，重试时复用 |
| deadline_ms | 本轮回复等待的绝对截止时间 |
| retries_done | 初次发送之后，成功投递的重发次数 |
| active | 1 表示活动事务，0 表示槽位空闲 |

active 为 0 后，其他字段可能保留旧值，不能再把旧 sequence 或旧 deadline 当成有效事务。reset 只需要清 active，不必反复清零每个最大 payload 数组。

### 11.3 一次 send_request 的提交顺序

实现：[/home/rain/communication_framework/src/comm_message_manager.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L489)。

调用顺序是：检查配置与参数 → 找空 pending → 找可用 sequence → 构造完整帧 → 尝试投递 TX → 成功后提交 pending → 更新下次分配游标和输出序号。

关键提交片段如下，位于同一函数内部：

```c
pending->request = frame;
pending->deadline_ms = comm_message_manager_make_deadline(
    now_ms, manager->config.response_timeout_ms);
pending->retries_done = 0u;
pending->active = 1;
```

它发生在 Channel 返回 OK 之后。否则队列根本没接受请求，却留下一个会不断等待和重试的“假事务”。

正常发送失败时，不启用 pending、不推进成功分配后的序号游标，也不修改调用者的 sequence 输出值。pending 已满和 TX 队列已满是不同错误，应分别定位。

极少数 pthread 内部错误可能发生在帧已经复制入队之后，例如 signal 或 unlock 失败；包装层会报系统错误，不承诺回滚已复制的帧。这类结果应按后端异常处理，不能套用“普通满队列拒绝”的恢复逻辑。

### 11.4 序号分配的保证和限制

序号范围是 1 到 65535，达到上限后回到 1，分配时跳过仍被活动 pending 占用的序号。0 不分配给请求，manager 发送主动上报时使用 0。

next_sequence 只是“下一次从哪里开始找”的游标，不是当前正在等待的请求，也不表示设备状态的新旧。

回复匹配依赖活动请求的 sequence，当前没有额外校验“这个回复的业务 ID 是否就是那条请求期望的业务 ID”。业务层仍必须验证消息内容。

序号只保证活动事务之间不冲突，没有永久记录历史请求。事务结束很久以后，序号经过绕回或 reset 被再次使用，极晚的同序号旧回复仍可能与新请求混淆；当前协议没有会话标识或历史去重窗口。

### 11.5 先入队再登记 pending 为什么不会被接收线程抢先处理

因为 manager 的全部接口只由同一个所属任务串行调用。

发送线程可能很快把请求发出去，接收线程也可能把回复放进 RX 队列，但它们都不能直接调用 manager。所属任务必须先执行完 send_request，之后才会从 RX 队列取出回复。

若以后让接收线程直接调用 handle_frame，就破坏了这个保证；不能只看到“RX 队列有锁”，就认为 manager 自身也已经线程安全。

### 11.6 收到一帧之后怎样分类

实现：[/home/rain/communication_framework/src/comm_message_manager.c:607](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L607)。

| 帧类型或条件 | 产生的事件 | pending 变化 |
| --- | --- | --- |
| REQUEST | REQUEST_RECEIVED | 不动本端 pending |
| REPORT | REPORT_RECEIVED | 不动 pending |
| RESPONSE 且找到活动 sequence | RESPONSE_RECEIVED | 清 active，结束事务 |
| ERROR 且找到活动 sequence | ERROR_RECEIVED | 清 active，结束事务 |
| RESPONSE 或 ERROR 未匹配 | UNMATCHED_REPLY | 不创建新事务 |
| 重试用尽后再次到期 | REQUEST_TIMEOUT | 清 active，结束事务 |

匹配回复后，先释放 pending，再同步回调。业务回调看到“已经回复”的事件时，事务状态已经提交完成。

handle_frame 的输入应当已经经过 Parser/Codec 验证。它不是第二个 CRC 检查器，不能把未经解码校验的线路数据直接塞进来。

### 11.7 匹配成功不代表业务成功

一个 sequence 正确、CRC 合法但进度字段为 101 的响应，会先结束 pending，再在业务层被拒绝。manager 不会因为业务层拒绝而自动恢复这个 pending 或触发它的超时重试。

这是明确的分层结果：通信事务收到最终回复，与业务结果是否接受，是两件事。需要“业务拒绝后重新查询”时，应由所属任务在当前回调返回后发起新事务。

ERROR 也会结束 pending。Linux 示例收到匹配 ERROR 会直接标记业务失败，不能再等这个已结束事务产生超时。

### 11.8 时间是谁提供的

manager 不调用系统时钟，也不会自己创建定时线程。调用者向 send_request 和 process_timeouts 提供同一时基的单调毫秒时间。

相关实现：

- 截止时间的饱和加法：[/home/rain/communication_framework/src/comm_message_manager.c:256](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L256)。
- 超时遍历：[/home/rain/communication_framework/src/comm_message_manager.c:685](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L685)。
- Linux 单调时钟读取：[/home/rain/communication_framework/examples/linux_runtime/main.c:177](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L177)。

响应截止时间从调用者提供的 now_ms 开始，不从 UART 最后一个字节真正发完时开始。因此发送队列积压会占用回复等待窗口，配置时必须把实际发送时延考虑进去。

### 11.9 有限重试的具体时序

以 response_timeout_ms=200、max_retries=1 为例，假设调用者及时检查：

| 时间 | 动作 | 状态 |
| --- | --- | --- |
| 1000 ms | 初次投递成功 | retries_done=0，deadline=1200 |
| 1199 ms | 检查超时 | 没到期，不做事 |
| 1200 ms | 首次重发投递成功 | 使用原 sequence，retries_done=1，deadline=1400 |
| 1300 ms | 收到匹配回复 | 结束 pending，不再超时 |
| 1400 ms | 若仍未收到回复 | 结束 pending，产生 REQUEST_TIMEOUT |

如果实际到 1250 ms 才执行第一次检查，新的 deadline 为 1450 ms，不是继续沿旧 deadline 得到 1400 ms。这样不会为了“补齐错过的时刻”连续补发多次。

重发投递失败时，不增加 retries_done，不刷新 deadline，也不释放 pending；后续调用再次尝试。遍历还会继续处理其他槽位，避免一个失败请求挡住其他请求的最终超时。

因此，max_retries 限制的是**成功交给通道的重发次数**，不是所有 API 尝试次数。如果 TX 始终拒绝，core 并不单独保证这个事务一定在固定墙钟时长内结束。运行示例还有连接期限和停止策略，实际产品也需要自己的异常收敛策略。

### 11.10 reset 的含义

实现：[/home/rain/communication_framework/src/comm_message_manager.c:458](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L458)。

普通 reset 清除活动 pending，并把 next_sequence 恢复为 1；保留已有统计，不关闭 TX Channel，不产生逐请求的取消或超时事件。

Linux 示例的 cancel_pending 先数活动事务，再调用 reset，所以日志里有 cancelled_pending，但框架并没有因此新增一个通用 CANCELLED 事件。正式项目如果需要把取消原因返回给业务调用者，应在任务或业务接口层明确交接。

### 11.11 回调不能重入同一个 manager

event 只在同步回调期间有效。需要交给别的任务时，应复制事件或所需数据，不能保存回调参数地址稍后访问。

回调中不要再次调用同一个 manager，包括立即发送响应或新请求。处理对端 REQUEST 时，可以先保存需要回复的工作项，等回调返回后由所属任务调用 send_response/send_error。

回调执行会占用所属任务的时间，应尽快返回。耗时工作应复制必要数据后交给其他任务；不要在回调里等待一个又需要当前 manager 继续运行才能完成的操作。主循环的 10 ms 空队列等待不能抵消回调自身的长时间阻塞。

## 12 业务协议与 Appliance Manager

业务布局：[/home/rain/communication_framework/examples/appliance_manager/appliance_protocol.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_protocol.h)。

查询接口：[/home/rain/communication_framework/examples/appliance_manager/appliance_client.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_client.h)；实现：[/home/rain/communication_framework/examples/appliance_manager/appliance_client.c:5](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_client.c#L5)。

模型接口：[/home/rain/communication_framework/examples/appliance_manager/appliance_manager.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.h)；实现：[/home/rain/communication_framework/examples/appliance_manager/appliance_manager.c:140](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.c#L140)。

### 12.1 外层 type 与业务消息 ID 不相同

| 层次 | 例子 | 回答的问题 |
| --- | --- | --- |
| 外层 type | REQUEST、RESPONSE、REPORT、ERROR | 这条消息在通信事务中是什么角色 |
| payload 业务 ID | QUERY_STATUS=0x02、STATUS_SNAPSHOT=0x01 | 这段业务数据表达什么操作或状态 |

外层 type=2 表示 RESPONSE，payload[0]=2 表示 QUERY_STATUS，虽然数值相同，却不属于同一套枚举。排查消息方向时不要把它们混在一起。

Appliance Client 只构造业务 payload，再调用 send_request；它不自行分配 sequence，也不直接绕过 manager 入队。

### 12.2 当前状态快照的业务字段

| payload 偏移 | 长度 | 含义 | 校验或解释 |
| --- | --- | --- | --- |
| 0 | 1 | 业务 ID | 必须为 STATUS_SNAPSHOT |
| 1 | 1 | 运行状态 | 支持 OFF、IDLE、RUNNING、PAUSED、COMPLETED、FAULT |
| 2 | 1 | 程序 | 支持 NONE、COTTON、QUICK、DELICATE |
| 3 | 1 | 进度 | 0 到 100 |
| 4 | 2 | 剩余分钟 | 大端 uint16_t |
| 6 | 1 | 门锁 | 只能为 0 或 1 |
| 7 | 2 | 故障码 | 大端 uint16_t，0 表示无故障 |

这里完成的是字段范围和消息形态校验，没有实现完整洗衣程序状态机，也没有规定所有字段之间的产品联动关系。

### 12.3 先解析临时模型 再整体提交

流程是：检查事件类型 → 检查业务 ID 和长度 → 逐字段解析到 candidate → 验证 candidate → 与旧模型逐字段比较 → 有变化时替换模型并通知。

关键提交点：

```c
manager->model = candidate;
manager->notify_callback(manager->notify_context, &manager->model);
```

这两句执行前，完整候选模型已经通过校验。不能边解析边直接改旧模型，否则最后一个字段失败时，UI 可能看到新旧数据混在一起的状态。

模型比较逐字段进行，不直接 memcmp 整个结构体，避免填充字节影响比较结果。

### 12.4 通知去重与连接状态是两回事

重复收到相同快照，业务处理返回 OK，但不再次触发模型变化回调。这减少了无意义 UI 刷新。

重连后，即使新快照与缓存完全一样，仍然应该宣布“连接已恢复在线”。Linux 示例因此把连接状态单独放在 application_t 中，不能依赖模型变化回调推断连接状态。

### 12.5 业务层不直接操作 LVGL

通知回调的 model 指向 manager 内部对象。未来接 GUI 时，应复制快照或所需字段，投递给 UI 所属任务，再由 UI 任务更新页面。

当前示例只是打印 model 日志，没有已经完成的 UI 消息队列、LVGL 绑定或页面导航接口。这些在新的 GUI 仓库中实现。

## 13 沿源码走完一次状态查询

下面按“主线程发起查询，模拟设备回复，模型进度变成 40”的路径阅读：

| 步骤 | 调用位置 | 此时发生了什么 |
| --- | --- | --- |
| 1 | [/home/rain/communication_framework/examples/linux_runtime/main.c:758](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L758) 的 run_sessions | 为本次连接初始化资源，状态进入 syncing |
| 2 | [/home/rain/communication_framework/examples/linux_runtime/main.c:615](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L615) 的 run_manager | 读取单调时间，发起同步状态查询 |
| 3 | [/home/rain/communication_framework/examples/appliance_manager/appliance_client.c:5](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_client.c#L5) | 生成 QUERY_STATUS payload |
| 4 | [/home/rain/communication_framework/src/comm_message_manager.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L489) | 分配序号、投递 TX、登记 pending |
| 5 | [/home/rain/communication_framework/examples/linux_runtime/main.c:275](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L275) 的 tx_main | 从 TX 取出完整帧 |
| 6 | [/home/rain/communication_framework/examples/linux_runtime/main.c:264](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L264) 的 send_frame | Codec 编码，send_bytes 分段发送 |
| 7 | [/home/rain/communication_framework/examples/linux_runtime/main.c:393](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L393) 的 peer_main | 模拟设备从实际字节中解码查询，按收到的序号构造回复 |
| 8 | [/home/rain/communication_framework/examples/linux_runtime/main.c:296](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L296) 的 rx_main | receive_frame 经 RingBuffer 和 Parser 提取帧，投递 RX |
| 9 | [/home/rain/communication_framework/examples/linux_runtime/main.c:615](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L615) 的 run_manager | 从 RX 取帧，调用 handle_frame |
| 10 | [/home/rain/communication_framework/src/comm_message_manager.c:607](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L607) | 匹配 sequence，结束 pending，产生 RESPONSE_RECEIVED |
| 11 | [/home/rain/communication_framework/examples/linux_runtime/main.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L489) 的 message_event | 把匹配响应交给业务解析 |
| 12 | [/home/rain/communication_framework/examples/appliance_manager/appliance_manager.c:140](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.c#L140) | 校验并提交状态快照，触发模型通知 |
| 13 | [/home/rain/communication_framework/examples/linux_runtime/main.c:463](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L463) 的 model_changed | 输出模型变化；未来这里负责把快照交给 UI |
| 14 | [/home/rain/communication_framework/examples/linux_runtime/main.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L489) 的 message_event | 同步查询业务处理成功后，将连接状态改为 online |

这里没有“收到了字节就更新模型”的捷径。每层的返回值和交接点都可以成为日志或断点位置。


## 14 Linux 运行示例怎样把模块组装起来

运行实现：[/home/rain/communication_framework/examples/linux_runtime/main.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c)。

已有运行说明：[/home/rain/communication_framework/examples/linux_runtime/README.md](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/README.md)。

### 14.1 四个执行角色

| 执行角色 | 独占或主要负责的状态 | 不应该做的事 |
| --- | --- | --- |
| 主线程 | Message Manager、pending、Appliance Manager、连接状态、连接生命周期 | 不永久阻塞在 TX 满等待上 |
| 发送线程 | 当前待编码帧和发送字节缓冲 | 不登记 pending，不直接改模型 |
| 接收线程 | 本端 RingBuffer、scratch、当前解析帧 | 不直接调用 manager 或 LVGL |
| 模拟设备线程 | 对端 RingBuffer、设备回复构造和场景数据 | 不读取本端 pending 来猜序号 |

主线程虽然没有另外起一个名为 manager 的 pthread，但它本身就是 manager 的所属任务。移植到 RTOS 时可把这一职责映射到专门的通信管理任务。

### 14.2 生命周期分为应用和连接两层

application_t 跨连接保留业务模型、连接状态以及模型通知计数。runtime_t 属于一条连接，持有本次 TX/RX 队列、manager、pending、socket 和工作线程。

保留 application_t 的意义是断线时仍有最后一次设备快照。重建 runtime_t 的意义是新连接不能继续消费旧队列、旧 pending 或旧半帧。

这项分层位于示例运行层，不是 comm_message_manager_t 内建的“自动重连服务”。

### 14.3 初始化顺序

init_runtime 的入口：[/home/rain/communication_framework/examples/linux_runtime/main.c:524](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L524)。

顺序为：清初始对象并将无效描述符设为 -1 → 创建停止锁 → 初始化 TX/RX 队列 → 绑定 Channel → 创建 socketpair → 初始化本次 manager。

start_runtime 的入口：[/home/rain/communication_framework/examples/linux_runtime/main.c:558](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L558)，依次创建发送、接收、模拟设备线程，并记录成功创建的数量。

主线程随后调用 run_manager。部分初始化失败时，只释放已取得的资源；部分线程启动失败时，只 join 成功启动的线程，不能拿一个未成功创建的 pthread_t 去 join。

### 14.4 manager 主循环如何兼顾收包和定时

主循环每轮最多取一帧，再调用 process_timeouts。RX 为空时最多等待 10 ms，没有数据也会继续检查时间。

如果采用“只要 RX 不为空就一直处理，清空后再检查超时”，持续上报可能让 RX 一直非空，重试和超时就会饿死。当前循环明确避免这种无限批处理。

示例中的默认参数是策略示范，不是所有产品通用参数：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| TX/RX 容量 | 各 2 帧 | 故意用较小队列展示等待和背压 |
| pending 容量 | 4 个 | 本次运行可登记的并行请求槽位 |
| 响应等待 | 200 ms | 每次成功投递后的等待窗口 |
| 最大重发次数 | 1 次 | 初次发送之外的成功重发 |
| TX 投递等待 | 0 ms | 保证 manager 不在发送队列上长期阻塞 |
| RX 入队单次等待 | 20 ms | 满时等待，超时后继续保留同一帧 |
| manager 空队列等待 | 10 ms | 兼顾接收响应与周期检查 |
| 单条连接运行期限 | 5 秒 | 示例的整体退出边界 |
| 持续上报时长 | 约 1 秒 | 验证定时处理不会被持续输入饿死 |
| 最大重连次数 | 2 次 | 包含初始连接，总共最多 3 条连接 |
| 重连等待 | 约 100 / 200 ms | 逐次退避，并允许信号中断 |

### 14.5 收发不是一条 send 对应一条 recv

send_bytes 按实际成功写出的字节数推进 offset，短写后继续发送剩余部分。recv 的返回值也只表示这次取得的字节数，不代表消息边界。

为了主动打破对边界的依赖，示例一次最多发送 3 字节、读取 7 字节。它处理 EINTR，并通过 MSG_NOSIGNAL 避免写入已断开的 socket 时由 SIGPIPE 直接终止进程。

一次 send_frame 失败，可能已经有部分字节进入线路。此时运行层结束连接，不在同一残缺字节流上盲目重发整帧。

### 14.6 RX 满时如何保留当前帧

Parser 已经消费完整候选后，接收线程在局部 frame 中持有独立副本。若 RX 入队返回 TIMEOUT，继续投递这个 frame，不调用下一次 receive_frame。

因此帧没有因这次等待超时自动丢失，读取会暂停，压力再通过 socket 缓冲逐步传到发送方。这就是当前示例的背压策略。

无流控 UART 无法保证发送方停下来，驱动 FIFO/DMA 缓冲可能继续被填满。正式项目必须根据驱动和线路条件定义溢出策略，不能把本地 socket 的行为直接当成串口保证。

### 14.7 停止必须先唤醒 再回收

关键函数：

- stop_with_kind：[/home/rain/communication_framework/examples/linux_runtime/main.c:124](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L124)。
- join_runtime：[/home/rain/communication_framework/examples/linux_runtime/main.c:574](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L574)。
- destroy_runtime：[/home/rain/communication_framework/examples/linux_runtime/main.c:583](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L583)。
- cancel_pending：[/home/rain/communication_framework/examples/linux_runtime/main.c:727](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L727)。

运行层按以下顺序结束连接：

1. 在停止锁下记录停止状态和首个停止原因。
2. close 两个帧队列，唤醒等待生产或消费的任务。
3. shutdown 两端 socket，唤醒阻塞的 recv/send，但暂不 close 描述符。
4. 主线程将连接状态标为 offline。
5. join 所有已经创建的工作线程。
6. 主线程统计并清除旧 pending，读取需要的结束快照。
7. 最后 close socket 描述符，destroy 队列及停止锁。

不先 join 就复用描述符或销毁锁，会让旧任务访问已经关闭、甚至已被系统复用的资源。

队列和 socket 的停止是中止语义，不保证旧帧全部发送完，也不保证所有 queued RX 被分发。框架将这种取消与真正的请求最终超时分开记录。

### 14.8 信号处理器不执行完整清理

SIGINT/SIGTERM 处理器只写一个 sig_atomic_t 标志。真正的 close、锁操作、日志和线程回收仍由主线程执行。

这也使重连退避可以被中断。不要把 pthread_join、printf 或 manager_reset 等复杂逻辑搬到信号处理器中。

## 15 线程安全与内存所有权速查

### 15.1 哪些对象可以跨线程共享

| 对象 | 所有者及生命周期 | 并发约束 |
| --- | --- | --- |
| 原始读写字节缓冲 | 对应收发调用或线程 | 不把临时地址留给异步使用者 |
| RingBuffer 和 scratch | 接收线程独占，至少活到线程退出 | 核心不加锁 |
| 核心帧队列 storage | 调用者提供，至少活到队列销毁 | 多任务只能经同步包装访问 |
| pthread 队列 | 运行层创建和销毁 | push/pop/close 和统计受内部锁保护；init/destroy 不可并发 |
| Channel | 非拥有型接口视图 | 后端及绑定必须覆盖全部使用过程 |
| Message Manager 和 pending | 固定所属任务 | 不可多任务并发调用，不可在回调中重入 |
| comm_message_event_t | manager 同步回调中的临时事件 | 延后使用必须复制 |
| appliance_model_t | Appliance Manager 持有 | 同一所属任务更新；给 UI 必须复制 |
| application_t 连接状态 | 主线程跨连接持有 | 当前示例没有给其他任务直接读取的无锁接口 |
| Parser 统计 | 接收任务持有 | 不可从其他任务直接无锁读取 |
| pthread 队列统计 | 外部提供存储，队列内部维护 | 使用 get_stats 获取独立快照 |
| Message Manager 统计 | 所属任务维护 | 由所属任务获取快照，再交给其他任务 |

### 15.2 不使用动态内存不等于没有内存成本

核心通过调用者提供的数组工作，内存规模可以预先估算。主要成本包括：

- TX 容量乘以 sizeof(comm_frame_t)。
- RX 容量乘以 sizeof(comm_frame_t)。
- pending 容量乘以 sizeof(comm_message_pending_t)，其中又含完整请求副本。
- 每个解析端的 RingBuffer 和 scratch，容量至少都是最大编码帧长。
- 当前帧、编码缓冲、临时模型和事件等调用栈对象。
- 平台队列同步对象和任务栈；这些由具体系统决定。

正式产品选择容量时，应同时考虑吞吐、允许积压、并行请求数和任务栈，不能只把 payload 上限调大而不检查队列及栈空间。现有实现不属于零拷贝框架。

### 15.3 最常见的生命周期错误

- 把回调里的 event 或 model 指针保存到全局，回调结束后再访问。
- Channel 仍在使用时，让它指向的队列 storage 离开作用域。
- 另一个任务直接读取正在变化的 manager、模型或裸统计结构。
- 只 close 队列就立即 destroy，等待者还没有真正退出。
- 将局部 payload 缓冲当作异步发送数据直接交给驱动，却没有确认驱动何时完成读取。

本项目帧队列通过复制解决了“原始帧变量被复用”的问题，但未来驱动是否复制发送缓冲仍要看驱动契约，不能把队列保证延伸到 UART DMA。

## 16 断线恢复与状态同步的具体语义

连接协调入口：[/home/rain/communication_framework/examples/linux_runtime/main.c:758](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L758) 的 run_sessions。

连接状态通知：[/home/rain/communication_framework/examples/linux_runtime/main.c:169](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L169)。

业务事件和同步判定：[/home/rain/communication_framework/examples/linux_runtime/main.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L489)。

### 16.1 连接状态与设备状态分别管理

| 连接状态 | 业务快照含义 |
| --- | --- |
| offline | 可以保留上次快照，但它只是离线缓存 |
| syncing | 新连接已建立，等待本次状态查询的有效结果 |
| online | 本次同步查询已经匹配并通过业务字段校验 |

断线不把进度强行变成 0，也不把“有缓存”当成“当前在线”。新连接收到任意一个 REPORT 不足以完成当前同步查询。

syncing 时的 REPORT 仍会被 manager 分发，但 Linux 示例在 message_event 中把它计入 ignored_reports，不交给 Appliance Manager。这是运行示例的同步策略，不是 core 禁止业务处理上报。

### 16.2 重连后的处理顺序

```text
旧连接断开
  → 停止旧收发与模拟设备任务
  → 主线程标记 offline，保留应用快照
  → join 旧线程
  → 取消旧 pending，放弃旧 TX/RX 及残留半帧
  → 释放旧连接资源
  → 有限退避
  → 初始化全新的连接对象，进入 syncing
  → 发起新的状态查询
  → 匹配响应且业务字段合法
  → 提交模型，再宣布 online
```

半帧被丢弃是因为旧接收任务和字节缓冲不再属于新连接；不是 Parser 把它自动“修好”了。

### 16.3 取消不是超时 也不证明设备未执行

断线时一个请求可能尚未发送，也可能已被设备执行、只是回复没回来。本地取消只表示不再沿旧连接等待这个事务。

因此 cancelled_pending 不增加 request_timeouts，也不自动重放旧业务命令。正式产品中的启动、停止、加热等控制命令，必须根据具体协议和设备状态确定重试及恢复语义，不能依赖“序号相同所以设备一定不会重复执行”。

当前 Message Manager 也没有替对端实现 REQUEST 去重。它收到 REQUEST 会交给业务处理，不会自动保证幂等或仅执行一次。

### 16.4 有限重连只属于示例策略

只在 reconnect 系列场景中，传输失败才会触发自动重建 socketpair。首次退避约 100 ms，第二次约 200 ms，之后再失败就退出。

初始化失败、线程创建失败、业务响应被拒绝和同步超时，都不会被转换成无限重连。匹配 ERROR 已经结束 pending，也必须直接进入失败处理。

单独运行 disconnect 场景仍然是“断开后退出”，并不会因为现在存在恢复示例就改变其含义。

### 16.5 旧数据隔离保证到哪里

当前实现保证旧任务、旧 socket、本地旧 TX/RX 队列及旧 RingBuffer 不会继续为新连接供数。session 是本地日志编号，没有写入协议帧。

新 manager 从 sequence=1 开始。若真实设备在新连接上重放一个与新请求同序号的旧响应，仅靠当前 sequence 无法辨别。需要协议会话字段、设备端配合或其他业务确认机制。

UART 也未必具有 socket 关闭后天然切断旧字节来源的效果。驱动残留字节、线路输入和设备复位策略，要在项目接入时另行处理。

## 17 统计为什么保留 以及怎样使用

统计的定位是**辅助已有的现象分析、日志和复现流程**。不要求把每个计数接入 UI，也不要求业务判断依赖它们。

比如“设备偶尔不更新”可以先看：Parser 有没有提帧、CRC 错误是否增长、RX 是否持续积压、manager 是未匹配回复还是最终超时。这些计数帮助缩小范围，再结合序号和时间日志追查具体事件。

### 17.1 四类统计的开启方式不同

| 位置 | 开启方式 | 读取约束 |
| --- | --- | --- |
| Parser | 调用 next_with_stats，先初始化统计对象 | 接收任务自己维护 |
| 核心帧队列 | stats_reset 后，对整个周期使用 push_with_stats/pop_with_stats | 与核心队列使用同样的外部同步 |
| pthread 队列 | init_with_stats；随后普通 push/pop 自动统计 | get_stats 在锁内复制 |
| Message Manager | init_with_stats；随后普通管理接口自动统计 | 所属任务串行读取 |

普通 init 或不带统计的解析、核心队列接口仍可使用。各个 with_stats 接口要求的统计存储不能随便传 NULL，应按头文件约定选择是否启用。

源码入口：

- [/home/rain/communication_framework/include/comm_parser.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_parser.h)。
- [/home/rain/communication_framework/include/comm_frame_queue.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame_queue.h)。
- [/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.h)。
- [/home/rain/communication_framework/include/comm_message_manager.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_message_manager.h)。

### 17.2 计数对应的是哪一个事件

| 计数 | 正确解释 | 容易误解的地方 |
| --- | --- | --- |
| Parser frames_ready | 成功提取并消费的帧数 | 不证明 RX 已消费或业务成功 |
| Parser discarded_bytes | 为重同步丢掉的字节数 | 不直接等于丢了多少条业务消息 |
| crc_errors | CRC 不匹配次数，是 decode_errors 的一部分 | 与版本、长度等错误不是同一概念 |
| 核心 push_full_count | 入队因为满而被拒绝的次数 | 不代表这些帧最后一定丢失 |
| pthread push_wait_count | 一次调用进入满等待的计数 | 不是每次虚假唤醒都加一次 |
| pthread push_timeout_count | 因满无法完成入队的超时次数 | 零等待失败也计数 |
| manager frames_sent | TX Channel 接受的帧数 | 不等于驱动实际完成发送的帧数 |
| requests_sent | 成功建立的新请求事务数 | 重发不算新事务 |
| retries_sent | 成功交给通道的重发数 | 投递失败不增加 |
| responses_matched | 匹配到 pending 的响应数 | 不代表 payload 被业务接受 |
| unmatched_replies | 未找到活动事务的回复数 | 可能是重复、迟到或未知序号 |
| request_timeouts | 重试用尽后结束的事务数 | 不包括队列超时和断线取消 |
| pending_peak | 本统计周期活动请求数的峰值 | 与 TX 队列深度不同 |

### 17.3 统计清零不等于业务复位

累计计数采用饱和递增，达到 UINT64_MAX 后不回到 0。

统计 reset 不清队列、不取消请求，也不修改已有 deadline。峰值从当前深度或当前 pending 数开始，因此“清零后峰值不为 0”是合理结果。

反过来，普通队列 reset 清队列但不清外部统计；manager reset 清 pending 和分配游标，但保留统计。等待或事务跨过统计清零时刻，开始和完成可能属于不同周期，不能要求周期内所有计数都一一配平。

### 17.4 与日志配合的方式

实际排查中优先保留时间、连接状态、方向、type、sequence、payload 业务 ID、处理结果。再按需取少量统计快照，回答“错误是否持续发生、哪个队列有积压、事务是否完成”。

当前运行示例中的 notifications、ignored_reports、cancelled_pending 等是示例局部记录，不都是核心提供的通用统计 API；不要把日志字段名直接当作框架接口。


## 18 现有运行场景与验证索引

这里整理的是已完成版本的验证资产，供复习和定位已有行为。当前阶段不再继续增加模拟测试；正式项目中的协议、时序及物理行为验证在接入目标设备后完成。

### 18.1 可直接运行的 11 个场景

编译入口：[/home/rain/communication_framework/examples/linux_runtime/Makefile](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/Makefile)。

在 Linux 服务器执行以下已有示例即可观察链路：

```sh
cd /home/rain/communication_framework
make -C examples/linux_runtime
./build/linux_runtime normal
./build/linux_runtime retry
./build/linux_runtime reconnect
```

| 参数 | 主要现象 | 预期退出码 |
| --- | --- | --- |
| normal | 同步响应到达，进度更新到 40，曾进入 online | 0 |
| retry | 对端不回复首次查询，同序号重发后成功 | 0 |
| timeout | 对端持续不回复，重发一次后最终超时，不更新模型 | 0 |
| rx-full | 等待 RX 满后恢复消费；16 条同步前上报被分发但不更新模型，随后响应更新到 40 | 0 |
| stop-full | RX 满等待时直接停止，取消未完成请求并回收 | 0 |
| disconnect | 对端收到查询后断开，检测 EOF，取消 pending 后退出 | 1 |
| idle | 不发查询，空闲约 200 ms 后结束 | 0 |
| stream | 已在线后持续上报约 1 秒，第二条查询重试并最终超时，末尾迟到回复不更新模型 | 0 |
| reconnect | 先同步到 40，后续响应收到一半时断开；新连接同步到 80 | 0 |
| reconnect-timeout | 新连接只发同步前上报和未知回复，不回复同步查询，缓存仍为 40 | 1 |
| reconnect-exhausted | 每条连接都半帧断开，两次重连耗尽后退出 | 1 |

这里部分退出码 1 是场景刻意制造的失败结果，不代表程序一定实现错误。SIGINT/SIGTERM 清理退出码分别为 130/143；错误命令参数返回 2。

rx-full 和更早的底层集成测试要分开理解：运行示例有同步前上报门控，底层 Appliance Manager 本身没有连接状态门控。

### 18.2 怎样看恢复日志

reconnect 场景应该体现以下因果顺序：

1. session=1 进入 syncing。
2. 有效响应产生 model progress=40，再进入 online。
3. 第二次查询的响应只收到一部分，对端断开。
4. session=1 进入 offline，cancelled_pending=1，timeouts=0。
5. 旧线程全部 join，资源释放。
6. 等待后创建 session=2，进入 syncing。
7. 进度 99 的同步前上报被忽略，未知序号 2 的回复记为 unmatched。
8. 新同步响应更新 model progress=80，session=2 进入 online。
9. 示例完成主动关闭，因此最后还会打印 offline。

模型变化日志应只有 40 和 80，不能因为半帧或未匹配回复出现 99。

stream 中 reports 应等于 peer_reports，notifications 为 reports 加上首次同步通知，retries=1、timeouts=1、unmatched=1。实际发送帧数受主机和调度影响，不是承诺的板级吞吐指标。

### 18.3 已有测试程序各自验证什么

| 测试源码完整路径 | 主要覆盖 |
| --- | --- |
| [/home/rain/communication_framework/tests/test_comm_codec.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_codec.c) | 编解码、字段、长度及 CRC 拒绝路径 |
| [/home/rain/communication_framework/tests/test_comm_ringbuffer.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_ringbuffer.c) | 满空、绕回、读写、peek/discard、失败时状态保持 |
| [/home/rain/communication_framework/tests/test_comm_parser.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_parser.c) | 拆包、粘包、垃圾字节、CRC 与长度错误后的重新同步、解析统计 |
| [/home/rain/communication_framework/tests/test_comm_frame_queue.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_frame_queue.c) | FIFO、整帧复制、容量、绕回及核心队列统计 |
| [/home/rain/communication_framework/tests/test_comm_frame_queue_pthread.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_frame_queue_pthread.c) | 超时、生产消费唤醒、关闭、排空和生命周期 |
| [/home/rain/communication_framework/tests/test_comm_frame_queue_pthread_stats.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_frame_queue_pthread_stats.c) | 等待结果计数、统计清零、饱和及并发快照 |
| [/home/rain/communication_framework/tests/test_comm_frame_channel.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_frame_channel.c) | 绑定、转发、参数及上下文传递 |
| [/home/rain/communication_framework/tests/test_comm_frame_channel_pthread.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_frame_channel_pthread.c) | pthread 后端结果映射、关闭和排空 |
| [/home/rain/communication_framework/tests/test_comm_message_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_message_manager.c) | pending、序号、发送失败、匹配、未匹配、超时及重试 |
| [/home/rain/communication_framework/tests/test_comm_message_manager_stats.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_comm_message_manager_stats.c) | 事务统计、部分重发失败、清零和饱和 |
| [/home/rain/communication_framework/tests/test_appliance_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_manager.c) | 模型字段校验、完整提交、通知去重、复位 |
| [/home/rain/communication_framework/tests/test_appliance_integration.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_integration.c) | 帧层面的状态查询、应答事件和模型更新 |
| [/home/rain/communication_framework/tests/test_appliance_stream_integration.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c) | 编码到字节流解析再到业务模型的整体交接 |

最后一个测试使用真实 pthread 队列，但各阶段由一个测试线程确定性推进，不等于多线程运行测试。多任务行为由 Linux 运行示例及此前的临时验证覆盖。

字节流集成测试里的重要用例：

- 每个两段拆分位置：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:239](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L239)。
- 逐字节到达及环形绕回：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:272](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L272)。
- 粘包、乱序和重复消息：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:300](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L300)。
- 坏流后的恢复：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:353](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L353)。
- 重发成功与最终超时后的迟到响应：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:387](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L387)。
- CRC 合法但业务 payload 非法：[/home/rain/communication_framework/tests/test_appliance_stream_integration.c:439](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/tests/test_appliance_stream_integration.c#L439)。

### 18.4 如何理解已有验证结论

基准版本完成时，13 个现有测试和 11 个运行场景通过。运行过程还检查过短读短写、EINTR、部分线程启动失败、socket 创建失败、阻塞发送停止、重连取消、旧队列隔离以及重复回收后的描述符数量，并经过 ASan、UBSan、TSan 检查。

临时故障注入和生命周期检查程序未纳入产品提交，不能误以为仓库中还有一套现成脚本可以重跑全部临时验证。仓库中可直接找到的是上表 13 个测试和 Linux 场景入口。

这些结果说明已覆盖的代码路径符合预期，并不证明所有调度组合、真实驱动、DMA、线路噪声或电源板时序都已经验证。项目上板时仍应结合现象与日志核实实际链路。

## 19 按现象定位问题

排查顺序可以沿“字节 → 帧 → 队列 → 事务 → 模型 → UI”逐层缩小，不必一开始就怀疑最上层页面。

| 现象 | 首先核对 | 接着定位 |
| --- | --- | --- |
| 完全看不到模型变化 | 是否真的收到线路字节，Parser 是否提取帧 | RX 是否入队和消费；事件是否支持；模型是否实际变化 |
| 字节在增长但 Parser 不出帧 | AA55、版本、type、长度、CRC 范围和大小端 | 是否仍在等待未完成候选；RingBuffer/scratch 容量是否足够 |
| CRC 错误持续增长 | 两端协议与校验参数是否相同 | 是否有漏字节、长度配置不一致或发送缓冲被复用 |
| send_request 返回失败 | 区分 pending 满、TX 满、已关闭和后端错误 | 不要从失败后未更新的 sequence 输出推断已发请求 |
| TX 出队了但始终超时 | 编码和实际驱动发送是否成功 | 对端是否执行并回复，回复是否到达 RX |
| 收到回复却显示 unmatched | sequence 与当前活动 pending 是否一致 | 是否重复、已经超时、旧连接残留或未知序号 |
| 没有回复也不重试 | process_timeouts 是否被周期调用 | 时基是否一致、所属任务是否被阻塞、TX 是否一直拒绝 |
| 队列满统计高但没有业务丢帧 | 当前策略是否保留同一帧等待 | 不要把“等待超时次数”直接当“丢帧条数” |
| responses_matched 增长但 UI 没变化 | 业务 ID、长度和字段是否合法 | 是否与旧模型相同而被通知去重 |
| 进度从 60 回到 40 | 是否是两个有效请求的乱序快照 | sequence 匹配不保证业务快照按新旧排序 |
| 重连后缓存有值但不在线 | 当前连接是否仍在 syncing | 是否只收到了 REPORT、未知回复或非法同步响应 |
| 停止时卡住 | 是否既唤醒了队列等待，也 shutdown 了阻塞 socket | 是否在 join 之前就销毁了资源，或有其他阻塞回调 |

### 19.1 常用断点和日志入口

| 想回答的问题 | 最直接的源码入口 |
| --- | --- |
| 收到的字节为什么没变成帧 | [/home/rain/communication_framework/src/comm_parser.c:41](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_parser.c#L41) |
| 当前是否真的进入 TX | [/home/rain/communication_framework/src/comm_message_manager.c:29](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L29) |
| 请求是否登记成功 | [/home/rain/communication_framework/src/comm_message_manager.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L489) |
| 为什么这个 sequence 未匹配 | [/home/rain/communication_framework/src/comm_message_manager.c:607](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L607) |
| 为什么没重试或未产生最终超时 | [/home/rain/communication_framework/src/comm_message_manager.c:685](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_message_manager.c#L685) |
| 哪个业务字段被拒绝 | [/home/rain/communication_framework/examples/appliance_manager/appliance_manager.c:140](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.c#L140) |
| 为什么没有变成 online | [/home/rain/communication_framework/examples/linux_runtime/main.c:489](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L489) |
| 为什么进入重连或没有重连 | [/home/rain/communication_framework/examples/linux_runtime/main.c:758](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L758) |
| 哪个等待者没有退出 | [/home/rain/communication_framework/examples/linux_runtime/main.c:124](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L124) 与 [/home/rain/communication_framework/ports/linux/comm_frame_queue_pthread.c:253](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/ports/linux/comm_frame_queue_pthread.c#L253) |

一次复现尽量保留相邻几次请求的序号、方向、时间和处理结果。只有“发生了 CRC 错误”或“通讯超时”一个结论，通常不足以区分解析问题、排队问题和业务拒绝。

## 20 正式项目怎样接入

这一节是后续项目接入时的职责清单，不表示当前阶段还要继续扩展模拟工程。LVGL 9 和页面导航将在独立仓库实现，再按项目需要引入通信能力。

### 20.1 先确定实际线路协议是否相同

如果电源板的外层帧头、长度、CRC、类型或应答关系与当前示例不同，先做协议映射，再决定哪些层可直接复用。

| 变化 | 主要涉及的代码 |
| --- | --- |
| 只新增业务命令或状态字段 | [/home/rain/communication_framework/examples/appliance_manager/appliance_protocol.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_protocol.h)、[/home/rain/communication_framework/examples/appliance_manager/appliance_client.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_client.c)、[/home/rain/communication_framework/examples/appliance_manager/appliance_manager.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/appliance_manager/appliance_manager.c) |
| 改外层帧格式、CRC 或 payload 上限 | [/home/rain/communication_framework/include/comm_frame.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame.h)、[/home/rain/communication_framework/src/comm_codec.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_codec.c)、必要时 [/home/rain/communication_framework/src/comm_parser.c](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/src/comm_parser.c) |
| 改队列平台 | [/home/rain/communication_framework/include/comm_frame_channel.h](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/include/comm_frame_channel.h) 约定及新平台后端 |
| 改实际字节收发 | 参考 [/home/rain/communication_framework/examples/linux_runtime/main.c:199](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L199) 与 [/home/rain/communication_framework/examples/linux_runtime/main.c:241](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L241) 的边界，接入目标驱动 |
| 改重试、连接恢复和同步策略 | 任务层调用顺序及 [/home/rain/communication_framework/examples/linux_runtime/main.c:758](https://github.com/LeanderPeng/communication_framework/blob/1ea827393746046450dfab424d99f9a85e229fd0/examples/linux_runtime/main.c#L758) 所展示的生命周期 |
| 接入 UI | 业务通知和连接状态通知的跨任务交接，不在 Parser 中调用 LVGL |

如果对端根本没有 sequence，不能简单把每帧都填 0 或填同一个固定值来套用当前匹配。应根据真实协议的应答规则设计事务映射，必要时调整这一层。

### 20.2 平台适配需要回答的问题

- TX/RX 帧队列如何在 RTOS 上实现复制、等待、超时与关闭唤醒。
- 哪个任务独占 manager 和模型，其他任务如何提交业务命令。
- 单调毫秒时钟从哪里获取，长时间运行和底层 tick 回绕如何转换。
- 串口是同步复制发送还是 DMA 异步读取，发送缓冲何时可以复用。
- 收包来自任务读取、DMA 回调还是中断，字节缓存由谁拥有。
- 无流控输入超过消费能力时，选择丢弃、重新同步还是报告连接故障。
- 停止时如何唤醒驱动读取、等待发送完成或中止 DMA。
- 任务退出、队列销毁及缓冲回收的先后关系。

这些问题应结合目标平台和设备解决。现有 pthread 代码可作为语义参考，不应让可移植 src 反过来依赖某个 RTOS。

### 20.3 GUI 与通信之间的边界

```text
UI 任务发起操作
  → 应用命令交接
  → 通信管理任务调用 Client / Message Manager
  → 设备通信与业务处理
  → 模型快照和连接状态交接
  → UI 任务更新页面
```

当前项目已经提供“业务模型变化回调”的出口，但没有完整实现上图两侧的应用命令队列和 UI 事件队列。

新 GUI 仓库应让 UI 决定页面展示，让业务层维护设备含义，让通信层处理线路和事务。页面不应解析 AA55 或 CRC，Parser 不应知道当前在哪个页面。

### 20.4 上板时优先验证的链路

先验证一次最小状态查询：真实发送 → 对端回复 → 解析 → 匹配 → 模型 → UI。确认数据闭环后，再观察周期上报、按键命令、超时、断线及重启行为。

遇到问题，按第 19 节的链路保留现场日志并复现。已有模拟验证减少的是软件基础问题的排查范围，不能代替实际设备的时序和状态确认。

## 21 复习问答

### 为什么不能收到一段数据就直接 decode

一段读取结果可能是半帧、多帧或带噪声的数据。Codec 只接受严格的一整帧，RingBuffer 和 Parser 负责保存与切分。

### 为什么 RingBuffer 和帧队列都要有 used

读下标等于写下标时，单靠下标无法区分空与满。used 允许使用全部容量。两者的单位不同，一个是字节，一个是帧。

### 为什么队列里复制整帧而不只存指针

为了让生产者在入队后立即复用自己的帧变量，并避免消费者引用已经离开作用域的内存。代价是固定大小的存储和复制成本。

### 为什么 TX 队列容量不等于 pending 容量

前者限制等待发送的积压帧，后者限制等待应答的事务。请求可以已经发送、TX 已空，但 pending 仍然占用。

### 为什么 manager 不自己加锁

当前设计让一个固定任务串行调用它，用线程安全队列交接外部输入，避免在事务状态和同步回调之间增加复杂锁关系。

### 为什么先投递 TX 再登记 pending

普通投递失败不应建立等待事务。单任务调用约束保证响应只会排进 RX，不能在 send_request 返回前由另一个任务抢先修改 manager。

### 为什么收到 RESPONSE 还可能不更新模型

可能未匹配 pending，也可能业务 ID、长度或字段不合法；即使合法，与旧模型完全相同也不会重复触发变化通知。

### 为什么非法业务响应不会继续触发原请求的超时

匹配成功已经结束通信事务。业务校验位于后续回调层，不会自动回滚 pending。

### 为什么 ERROR 不能继续等超时

匹配 ERROR 与匹配 RESPONSE 一样结束 pending。应该处理错误结果，不能等待已经不存在的活动事务到期。

### 为什么重发沿用原序号

重发仍属于同一个事务。原请求的慢回复与重发后的回复都可以完成该 pending，后来的重复回复再被归入 unmatched。

### 为什么重复请求不等于设备一定只执行一次

当前 manager 没有实现接收 REQUEST 的业务去重；对端执行幂等性取决于具体协议和业务，不由本端回复匹配自动保证。

### 为什么队列等待超时不能等同于请求最终超时

前者说明帧没能在等待窗口内完成入队或出队，后者说明一个活动事务用尽允许的重试仍没收到最终回复。

### 为什么重连保留模型却又显示离线

模型是最后已知数据，连接状态是当前这条连接是否已同步。缓存有值不代表它仍然新鲜或设备仍在线。

### 为什么相同快照也要通知连接恢复

模型变化通知可能被去重，而离线转在线本身是用户需要看到的状态变化，两种通知解决不同问题。

### 为什么 reset 之后旧响应仍可能有风险

reset 把序号游标恢复为 1，却没有清除物理线路上的所有历史数据，也没有在线路协议中增加会话号。新旧连接隔离必须由运行层和实际协议共同保证。

### 为什么统计清零后峰值可能不是零

新的统计周期从当前积压帧数或活动 pending 数起算，并没有清空业务状态。

### 为什么 close 后还要 join 才能 destroy

close 只是唤醒并拒绝新投递，等待线程尚未保证退出。join 确认线程不再访问对象后，才能销毁同步资源。

### 为什么 framework 完成后仍需上板验证

当前完成的是可复用通信链路和软件行为；驱动、物理线路、设备协议与产品状态机属于实际集成条件。两者是不同阶段的完成标准。

## 22 修改代码前的定位清单

| 想修改的需求 | 先读哪里 | 应保持的边界 |
| --- | --- | --- |
| 增加一个查询命令 | 第 11、12 节及 Appliance Client | 不绕过 manager 自己生成事务序号 |
| 扩展状态快照 | 第 12 节及 Appliance Manager | 先校验临时模型，再整体提交 |
| 调整重试次数或等待时间 | 第 11、14 节 | 区分通道等待和应答等待 |
| 调整队列容量 | 第 8、9、15 节 | 按帧计算容量，检查静态内存和积压 |
| 从 Linux 迁移到 RTOS | 第 9、10、20 节 | 平台适配放在端口层 |
| 接入 LVGL 页面 | 第 12、15、20 节 | UI 所属任务消费副本，不从通信回调直接操作页面 |
| 修改断线恢复 | 第 14、16 节 | 旧线程退出和旧数据隔离必须先于新连接使用 |
| 增加排查信息 | 第 17、19 节 | 保留事件发生层次，避免把统计含义扩大 |

维护本笔记时，先确认实现是否已经变化，再更新源码基准、对应链接及语义说明。不要只更新行号而继续保留已经不成立的行为描述。
