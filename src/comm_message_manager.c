#include "comm_message_manager.h"

#include <stddef.h>
#include <string.h>

/*
 * manager 只保存通道地址，不拥有通道。初始化时必须确认这个地址当前指向
 * 一组完整绑定，避免直到第一次发送时才发现回调函数缺失。
 */
static int comm_message_manager_channel_is_bound(
    const comm_frame_channel_t *channel)
{
    return (channel != NULL) &&
           (channel->context != NULL) &&
           (channel->push != NULL) &&
           (channel->pop != NULL) &&
           (channel->close != NULL);
}

/*
 * 检查 reset 以及后续运行所依赖的长期配置。
 *
 * 此处不检查 next_sequence 和每个 pending 的运行值，因为 reset 的职责正是
 * 清除或修复这些状态；但存储区、策略、通道和回调一旦丢失，reset 无法知道
 * 应该恢复成什么，因此把它们视为不可修复的 INVALID_STATE。
 */
static int comm_message_manager_has_valid_configuration(
    const comm_message_manager_t *manager)
{
    return (manager != NULL) &&
           (manager->initialized == 1) &&
           comm_message_manager_channel_is_bound(manager->tx_channel) &&
           (manager->pending_storage != NULL) &&
           (manager->pending_capacity > 0u) &&
           (manager->config.response_timeout_ms > 0u) &&
           (manager->event_callback != NULL);
}

/*
 * 只清除 active，而不擦除 request、deadline_ms 和 retries_done。
 * inactive 槽位中的其余字节不再属于有效状态，新请求启用槽位前会完整写入。
 * 这样 reset 的成本与槽位数量成正比，而不是与最大帧负载字节数成正比。
 */
static void comm_message_manager_clear_pending(
    comm_message_pending_t *pending_storage,
    size_t pending_capacity)
{
    size_t index;

    for (index = 0u; index < pending_capacity; ++index) {
        pending_storage[index].active = 0;
    }
}

/* 将平台无关通道结果转换成消息管理器自己的结果空间。 */
static comm_message_manager_result_t comm_message_manager_map_channel_result(
    comm_frame_channel_result_t result)
{
    switch (result) {
        case COMM_FRAME_CHANNEL_OK:
            return COMM_MESSAGE_MANAGER_OK;

        case COMM_FRAME_CHANNEL_TIMEOUT:
            return COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT;

        case COMM_FRAME_CHANNEL_CLOSED:
            return COMM_MESSAGE_MANAGER_CHANNEL_CLOSED;

        case COMM_FRAME_CHANNEL_NULL_ARGUMENT:
        case COMM_FRAME_CHANNEL_INVALID_STATE:
        case COMM_FRAME_CHANNEL_BACKEND_ERROR:
        default:
            return COMM_MESSAGE_MANAGER_CHANNEL_ERROR;
    }
}

/*
 * 构造并发送一帧不需要登记 pending 的消息。
 *
 * 局部 frame 必须整体清零后再填写字段。虽然只有 payload_length 个负载字节
 * 具有协议意义，但帧队列会复制整个 comm_frame_t；清零可以避免把栈中未初始
 * 化的 payload 尾部字节带入队列，也让不同平台上的测试结果保持确定。
 */
static comm_message_manager_result_t comm_message_manager_send_frame(
    comm_message_manager_t *manager,
    uint8_t type,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_length)
{
    comm_frame_t frame;
    comm_frame_channel_result_t channel_result;

    if (manager == NULL) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if ((payload_length > 0u) && (payload == NULL)) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if (!comm_message_manager_has_valid_configuration(manager)) {
        return COMM_MESSAGE_MANAGER_INVALID_STATE;
    }

    if (payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
        return COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE;
    }

    if (((type == COMM_FRAME_TYPE_RESPONSE) ||
         (type == COMM_FRAME_TYPE_ERROR)) &&
        (sequence == 0u)) {
        return COMM_MESSAGE_MANAGER_INVALID_SEQUENCE;
    }

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = type;
    frame.sequence = sequence;
    frame.payload_length = (uint16_t)payload_length;

    if (payload_length > 0u) {
        memcpy(frame.payload, payload, payload_length);
    }

    channel_result = comm_frame_channel_push(
        manager->tx_channel,
        &frame,
        manager->config.send_timeout_ms);

    return comm_message_manager_map_channel_result(channel_result);
}

comm_message_manager_result_t comm_message_manager_init(
    comm_message_manager_t *manager,
    const comm_frame_channel_t *tx_channel,
    comm_message_pending_t *pending_storage,
    size_t pending_capacity,
    const comm_message_manager_config_t *config,
    comm_message_event_fn event_callback,
    void *event_context)
{
    /*
     * event_context 允许为空，因为有些回调不需要外部对象。其余指针都是完成
     * manager 基本职责所必需的。全部验证发生在写入 manager 和 pending 前，
     * 因此参数错误不会留下部分初始化状态。
     */
    if ((manager == NULL) ||
        (tx_channel == NULL) ||
        (pending_storage == NULL) ||
        (config == NULL) ||
        (event_callback == NULL)) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if ((pending_capacity == 0u) ||
        (config->response_timeout_ms == 0u) ||
        !comm_message_manager_channel_is_bound(tx_channel)) {
        return COMM_MESSAGE_MANAGER_INVALID_CONFIG;
    }

    manager->tx_channel = tx_channel;
    manager->pending_storage = pending_storage;
    manager->pending_capacity = pending_capacity;
    manager->config = *config;
    manager->event_callback = event_callback;
    manager->event_context = event_context;
    manager->next_sequence = 1u;

    comm_message_manager_clear_pending(pending_storage, pending_capacity);

    /* 所有字段和外部存储都准备完成后，最后发布 initialized 状态。 */
    manager->initialized = 1;

    return COMM_MESSAGE_MANAGER_OK;
}

comm_message_manager_result_t comm_message_manager_reset(
    comm_message_manager_t *manager)
{
    if (manager == NULL) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if (!comm_message_manager_has_valid_configuration(manager)) {
        return COMM_MESSAGE_MANAGER_INVALID_STATE;
    }

    comm_message_manager_clear_pending(manager->pending_storage,
                                       manager->pending_capacity);
    manager->next_sequence = 1u;

    return COMM_MESSAGE_MANAGER_OK;
}

comm_message_manager_result_t comm_message_manager_send_response(
    comm_message_manager_t *manager,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_length)
{
    return comm_message_manager_send_frame(manager,
                                           COMM_FRAME_TYPE_RESPONSE,
                                           sequence,
                                           payload,
                                           payload_length);
}

comm_message_manager_result_t comm_message_manager_send_report(
    comm_message_manager_t *manager,
    const uint8_t *payload,
    size_t payload_length)
{
    return comm_message_manager_send_frame(manager,
                                           COMM_FRAME_TYPE_REPORT,
                                           0u,
                                           payload,
                                           payload_length);
}

comm_message_manager_result_t comm_message_manager_send_error(
    comm_message_manager_t *manager,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_length)
{
    return comm_message_manager_send_frame(manager,
                                           COMM_FRAME_TYPE_ERROR,
                                           sequence,
                                           payload,
                                           payload_length);
}
