# 学习与实现路线

## 阶段 1：协议帧与编解码

- 定义帧头、版本、消息类型、序号、长度、负载和 CRC16。
- 实现完整帧编码与解码。
- 验证大小端、非法长度和 CRC 错误。

产物：`comm_frame.h`、`comm_codec.c`、`test_comm_codec.c`。

## 阶段 2：RingBuffer

- 实现固定内存环形缓冲区。
- 明确定义空、满、有效长度和溢出策略。
- 验证索引绕回、边界容量和批量读写。

产物：`comm_ringbuffer.h`、`comm_ringbuffer.c`、对应测试。

## 阶段 3：增量协议解析器

- 允许每次输入任意数量的字节。
- 处理半帧、连续多帧、垃圾前缀和损坏帧。
- CRC 错误后能够重新同步到下一帧。

产物：`comm_parser.h`、`comm_parser.c`、字节流测试。

## 阶段 4：有界消息队列与平台适配

- 核心层只依赖抽象接口。
- Linux 端使用 pthread mutex/condition variable。
- RTOS 端映射到目标平台队列与任务 API。
- 定义队列满、等待超时和退出策略。

## 阶段 5：消息管理器

- 统一请求、应答、主动上报和错误消息。
- 使用 sequence 匹配请求与应答。
- 实现 pending、超时、有限重试和旧回复过滤。

## 阶段 6：Appliance Manager 示例

- 把完整通讯消息转换成洗衣机业务事件。
- 更新业务模型，但不直接操作 LVGL。
- 通过 UI 平台适配接口投递刷新请求。

## 阶段 7：工程化验证

- 模拟高频输入、队列满、乱序、重复消息和断线重连。
- 统计收发帧数、CRC 错误、溢出、超时及队列峰值。
- 使用日志和测试复现问题，而不是依赖现场猜测。

当前已完成：Parser 可选诊断统计，以及核心帧队列可选诊断统计。

队列统计使用 `comm_frame_queue_stats_reset(&queue, &stats)` 开始一个周期，
之后统一通过 `comm_frame_queue_push_with_stats` / `comm_frame_queue_pop_with_stats`
记录成功入队数、成功出队数、满队列拒绝次数和历史积压峰值。统计清零不清空
队列，峰值从当前深度开始；普通队列 reset 不算出队，也不清除外部统计。

下一小步：在 pthread 包装层接入受 mutex 保护的统计读取和更新，单独定义
遇满、等待与最终超时的口径。当前 pthread/Channel 路径仍使用无统计核心接口，
不能用核心 FULL 次数代替阻塞队列的超时次数。
