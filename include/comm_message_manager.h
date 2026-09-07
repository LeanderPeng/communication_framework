#ifndef COMM_MESSAGE_MANAGER_H
#define COMM_MESSAGE_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "comm_frame_channel.h"

/* 消息管理器接口的执行结果，成功固定为 0。 */
typedef enum {
    COMM_MESSAGE_MANAGER_OK = 0,
    COMM_MESSAGE_MANAGER_NULL_ARGUMENT,
    COMM_MESSAGE_MANAGER_INVALID_CONFIG,
    COMM_MESSAGE_MANAGER_INVALID_STATE,
    COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE,
    COMM_MESSAGE_MANAGER_INVALID_SEQUENCE,
    COMM_MESSAGE_MANAGER_PENDING_FULL,
    COMM_MESSAGE_MANAGER_SEQUENCE_EXHAUSTED,
    COMM_MESSAGE_MANAGER_UNSUPPORTED_FRAME_TYPE,
    COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT,
    COMM_MESSAGE_MANAGER_CHANNEL_CLOSED,
    COMM_MESSAGE_MANAGER_CHANNEL_ERROR
} comm_message_manager_result_t;

/* 消息管理器向上层报告的事件类型。 */
typedef enum {
    COMM_MESSAGE_EVENT_REQUEST_RECEIVED = 0,
    COMM_MESSAGE_EVENT_RESPONSE_RECEIVED,
    COMM_MESSAGE_EVENT_REPORT_RECEIVED,
    COMM_MESSAGE_EVENT_ERROR_RECEIVED,
    COMM_MESSAGE_EVENT_REQUEST_TIMEOUT,
    COMM_MESSAGE_EVENT_UNMATCHED_REPLY
} comm_message_event_type_t;

/*
 * 一次同步事件回调携带的数据。
 *
 * REQUEST/REPORT 事件中的 frame 是刚收到的帧。
 * RESPONSE/ERROR 事件中的 frame 是与 pending 请求匹配的回复或错误帧。
 * REQUEST_TIMEOUT 事件中的 frame 是最终超时的原始请求帧。
 * UNMATCHED_REPLY 事件中的 frame 是找不到活动 sequence 的回复或错误帧，
 * 通常表示迟到回复、重复回复或对端发送了未知 sequence。
 *
 * retries_done 表示该请求在初次发送后已经重发的次数。与请求匹配无关的
 * REQUEST、REPORT 和 UNMATCHED_REPLY 事件中，此值为 0。
 */
typedef struct {
    comm_message_event_type_t type;
    comm_frame_t frame;
    uint8_t retries_done;
} comm_message_event_t;

/*
 * 事件回调在消息管理器当前调用栈中同步执行。
 * event 只在回调期间有效；若上层需要延后处理，必须复制其中的数据。
 * 回调应尽快返回，并且不应从回调内部再次调用同一个 manager。
 */
typedef void (*comm_message_event_fn)(void *context,
                                      const comm_message_event_t *event);

/*
 * 调用方提供的一个 pending 请求槽位。
 *
 * request 保存初次发送的完整请求帧，重试时保持相同 sequence 原样发送。
 * deadline_ms 使用调用方提供的单调毫秒时间，与具体 Linux/RTOS 时钟无关。
 * retries_done 已重试次数
 * active 标记该 pending 是否仍然有效。
 * 所有字段由消息管理器维护，调用方不应直接修改。
 */
typedef struct {
    comm_frame_t request;
    uint64_t deadline_ms;
    uint8_t retries_done;
    int active;
} comm_message_pending_t;

/* 消息管理器的固定策略配置。 */
typedef struct {
    uint32_t response_timeout_ms; /* 每次发送后等待回复的时间，必须大于 0 */
    uint32_t send_timeout_ms;     /* 向发送通道投递一帧时的最大等待时间 */
    uint8_t max_retries;          /* 初次发送后允许的最大重发次数 */
} comm_message_manager_config_t;

/*
 * 消息管理器运行状态。
 *
 * tx_channel      只负责发送完整帧，manager 不拥有也不关闭该通道。
 * pending_storage 由调用方提供，容量决定最多同时等待多少个请求回复。
 * next_sequence   保存下一次优先尝试的请求序号；0 永远不分配给请求。
 *
 * manager 本身不加锁，应该由一个固定任务串行调用。接收任务可以先把完整帧
 * 投递到线程安全队列，再由 manager 所属任务调用 handle_frame。这样 pending
 * 数组和 sequence 分配无需再次加锁。
 */
typedef struct {
    const comm_frame_channel_t *tx_channel;
    comm_message_pending_t *pending_storage;
    size_t pending_capacity;
    comm_message_manager_config_t config;
    comm_message_event_fn event_callback;
    void *event_context;
    uint16_t next_sequence;
    int initialized;
} comm_message_manager_t;

/*
 * 初始化消息管理器并清空调用方提供的 pending 数组。
 * tx_channel 必须已经绑定；pending_capacity 必须大于 0；response_timeout_ms
 * 必须大于 0。event_callback 不能为空，event_context 允许为空。
 *
 * manager 和 pending_storage 的生命周期必须覆盖全部使用过程。本函数不取得
 * tx_channel、pending_storage 或 event_context 的所有权。
 */
comm_message_manager_result_t comm_message_manager_init(
    comm_message_manager_t *manager,
    const comm_frame_channel_t *tx_channel,
    comm_message_pending_t *pending_storage,
    size_t pending_capacity,
    const comm_message_manager_config_t *config,
    comm_message_event_fn event_callback,
    void *event_context);

/*
 * 清除所有 pending 请求并把下一个请求序号恢复为 1。
 * 不关闭或重置发送通道，也不会为被清除的请求产生超时回调。
 */
comm_message_manager_result_t comm_message_manager_reset(
    comm_message_manager_t *manager);

/*
 * 构造并发送 REQUEST 帧，同时登记一个 pending 请求。
 *
 * now_ms 必须来自调用方选择的同一个单调递增毫秒时基。请求 sequence 从
 * 1..UINT16_MAX 中选择，并避开仍处于 pending 状态的序号；0 保留给无需
 * 请求匹配的消息。成功时通过 sequence 返回分配结果。
 *
 * payload_length 为 0 时 payload 允许为空。发送或登记失败时不产生 pending，
 * 也不修改 sequence 指向的输出值。
 */
comm_message_manager_result_t comm_message_manager_send_request(
    comm_message_manager_t *manager,
    const uint8_t *payload,
    size_t payload_length,
    uint64_t now_ms,
    uint16_t *sequence);

/* 使用指定的非零请求序号构造并发送 RESPONSE 帧。 */
comm_message_manager_result_t comm_message_manager_send_response(
    comm_message_manager_t *manager,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_length);

/* 构造并发送 sequence 固定为 0 的主动 REPORT 帧。 */
comm_message_manager_result_t comm_message_manager_send_report(
    comm_message_manager_t *manager,
    const uint8_t *payload,
    size_t payload_length);

/* 使用指定的非零请求序号构造并发送 ERROR 帧。 */
comm_message_manager_result_t comm_message_manager_send_error(
    comm_message_manager_t *manager,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_length);

/*
 * 处理一帧已经由 Parser 完成校验和解码的接收帧。
 *
 * REQUEST 和 REPORT 直接产生对应事件。RESPONSE 和 ERROR 使用 sequence
 * 查找 pending：找到后移除 pending 并产生匹配事件；找不到则产生
 * UNMATCHED_REPLY，用于识别迟到、重复或未知回复。
 */
comm_message_manager_result_t comm_message_manager_handle_frame(
    comm_message_manager_t *manager,
    const comm_frame_t *frame);

/*
 * 使用 now_ms 检查全部 pending 请求。
 *
 * 到达截止时间且仍可重试时，以相同 sequence 重发原请求并更新截止时间；
 * 已达到 max_retries 时移除 pending，并产生 REQUEST_TIMEOUT 事件。
 * max_retries 只计算初次发送之后的重发次数。
 */
comm_message_manager_result_t comm_message_manager_process_timeouts(
    comm_message_manager_t *manager,
    uint64_t now_ms);

#endif /* COMM_MESSAGE_MANAGER_H */
