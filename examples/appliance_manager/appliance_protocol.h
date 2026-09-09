#ifndef APPLIANCE_PROTOCOL_H
#define APPLIANCE_PROTOCOL_H

/* 当前示例定义的业务消息 ID；请求和对应响应使用不同 ID，避免方向含义混淆。 */
#define APPLIANCE_MESSAGE_STATUS_SNAPSHOT        0x01u
#define APPLIANCE_MESSAGE_QUERY_STATUS           0x02u

/*
 * QUERY_STATUS 不携带查询参数，payload 只有一个业务消息 ID：
 *
 *   偏移  大小  含义
 *   0     1     业务消息 ID，固定为 QUERY_STATUS
 *
 * 外层 REQUEST 的 sequence 由 comm_message_manager 分配，不放进业务 payload。
 */
#define APPLIANCE_QUERY_STATUS_MESSAGE_ID_OFFSET 0u
#define APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE      1u

/*
 * STATUS_SNAPSHOT 的 payload 字节布局：
 *
 *   偏移  大小  含义
 *   0     1     业务消息 ID，固定为 STATUS_SNAPSHOT
 *   1     1     运行状态 appliance_run_state_t 的协议值
 *   2     1     洗涤程序 appliance_program_t 的协议值
 *   3     1     完成进度，范围 0..100
 *   4     2     剩余分钟数，无符号 16 位大端序
 *   6     1     门锁状态，只允许 0 或 1
 *   7     2     故障码，无符号 16 位大端序，0 表示无故障
 *
 * 这些宏描述的是 comm_frame_t.payload 内部的业务协议，不包含外层通信帧的
 * 同步字、版本、type、sequence、payload_length 和 CRC。
 */
#define APPLIANCE_STATUS_MESSAGE_ID_OFFSET       0u
#define APPLIANCE_STATUS_RUN_STATE_OFFSET        1u
#define APPLIANCE_STATUS_PROGRAM_OFFSET          2u
#define APPLIANCE_STATUS_PROGRESS_OFFSET         3u
#define APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET 4u
#define APPLIANCE_STATUS_DOOR_LOCKED_OFFSET      6u
#define APPLIANCE_STATUS_FAULT_CODE_OFFSET       7u
#define APPLIANCE_STATUS_PAYLOAD_SIZE            9u

#endif /* APPLIANCE_PROTOCOL_H */
