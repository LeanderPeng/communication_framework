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

/* 返回非零请求序号的下一个值，并在 UINT16_MAX 后绕回 1。 */
static uint16_t comm_message_manager_next_sequence(uint16_t sequence)
{
    if (sequence == UINT16_MAX) {
        return 1u;
    }

    return (uint16_t)(sequence + 1u);
}

/*
 * 查找某个请求序号是否已经被活动 pending 占用。
 *
 * next_sequence 只是分配游标，真正代表未完成事务的是活动 pending 中保存的
 * request.sequence。新请求必须避开这些值，防止两个请求无法区分回复归属。
 */
static int comm_message_manager_sequence_is_active(
    const comm_message_manager_t *manager,
    uint16_t sequence)
{
    size_t index;

    for (index = 0u; index < manager->pending_capacity; ++index) {
        if ((manager->pending_storage[index].active == 1) &&
            (manager->pending_storage[index].request.sequence == sequence)) {
            return 1;
        }
    }

    return 0;
}

/*
 * 使用回复帧携带的 sequence 查找对应请求事务。
 *
 * RESPONSE/ERROR 不需要保存 pending 数组下标。只要双方在整个请求事务中保持
 * sequence 不变，回复到达时就能在多个并行请求中找到正确的一项。
 */
static comm_message_pending_t *comm_message_manager_find_pending_by_sequence(
    comm_message_manager_t *manager,
    uint16_t sequence)
{
    size_t index;

    for (index = 0u; index < manager->pending_capacity; ++index) {
        if ((manager->pending_storage[index].active == 1) &&
            (manager->pending_storage[index].request.sequence == sequence)) {
            return &manager->pending_storage[index];
        }
    }

    return NULL;
}

/* 返回第一个空闲 pending 槽位；全部占用时返回 NULL。 */
static comm_message_pending_t *comm_message_manager_find_free_pending(
    comm_message_manager_t *manager)
{
    size_t index;

    for (index = 0u; index < manager->pending_capacity; ++index) {
        if (manager->pending_storage[index].active == 0) {
            return &manager->pending_storage[index];
        }
    }

    return NULL;
}

/*
 * 检查所有活动 pending 是否符合 manager 自己能够产生的状态。
 * 除字段范围外还检查 sequence 唯一性，否则收到回复时无法确定应移除哪一项。
 */
static int comm_message_manager_has_valid_pending_state(
    const comm_message_manager_t *manager)
{
    size_t index;
    size_t other_index;
    const comm_message_pending_t *pending;

    if (!comm_message_manager_has_valid_configuration(manager) ||
        (manager->next_sequence == 0u)) {
        return 0;
    }

    for (index = 0u; index < manager->pending_capacity; ++index) {
        pending = &manager->pending_storage[index];

        if ((pending->active != 0) && (pending->active != 1)) {
            return 0;
        }

        if (pending->active == 0) {
            /* active=0 时，其余内容都属于无效旧数据，不参与状态检查。 */
            continue;
        }

        if ((pending->request.version != COMM_FRAME_VERSION) ||
            (pending->request.type != COMM_FRAME_TYPE_REQUEST) ||
            (pending->request.sequence == 0u) ||
            (pending->request.payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) ||
            (pending->retries_done > manager->config.max_retries)) {
            return 0;
        }

        /* 活动 sequence 不能重复，否则无法判断回复属于哪一个请求。 */
        for (other_index = index + 1u;
             other_index < manager->pending_capacity;
             ++other_index) {
            if ((manager->pending_storage[other_index].active == 1) &&
                (manager->pending_storage[other_index].request.sequence ==
                 pending->request.sequence)) {
                return 0;
            }
        }
    }

    return 1;
}

/*
 * 计算回复截止时间，并在 uint64_t 加法将溢出时饱和到 UINT64_MAX。
 * 正常单调毫秒时钟几乎不可能走到该边界，但显式处理可避免整数绕回后把
 * 新请求误判为已经超时。
 */
static uint64_t comm_message_manager_make_deadline(
    uint64_t now_ms,
    uint32_t timeout_ms)
{
    if (now_ms > (UINT64_MAX - (uint64_t)timeout_ms)) {
        return UINT64_MAX;
    }

    return now_ms + (uint64_t)timeout_ms;
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

comm_message_manager_result_t comm_message_manager_send_request(
    comm_message_manager_t *manager,
    const uint8_t *payload,
    size_t payload_length,
    uint64_t now_ms,
    uint16_t *sequence)
{
    comm_message_pending_t *pending;
    comm_frame_t frame;
    comm_frame_channel_result_t channel_result;
    uint16_t candidate;
    uint32_t attempts;

    if ((manager == NULL) || (sequence == NULL)) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if ((payload_length > 0u) && (payload == NULL)) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if (!comm_message_manager_has_valid_pending_state(manager)) {
        return COMM_MESSAGE_MANAGER_INVALID_STATE;
    }

    if (payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
        return COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE;
    }

    pending = comm_message_manager_find_free_pending(manager);
    if (pending == NULL) {
        return COMM_MESSAGE_MANAGER_PENDING_FULL;
    }

    /*
     * 从 next_sequence 开始寻找未被占用的非零序号。最多检查 UINT16_MAX
     * 个候选值；若全部都处于 pending，说明当前没有可安全分配的序号。
     */
    candidate = manager->next_sequence;
    for (attempts = 0u; attempts < (uint32_t)UINT16_MAX; ++attempts) {
        if (!comm_message_manager_sequence_is_active(manager, candidate)) {
            break;
        }

        candidate = comm_message_manager_next_sequence(candidate);
    }

    if (attempts == (uint32_t)UINT16_MAX) {
        return COMM_MESSAGE_MANAGER_SEQUENCE_EXHAUSTED;
    }

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_REQUEST;
    frame.sequence = candidate;
    frame.payload_length = (uint16_t)payload_length;
    if (payload_length > 0u) {
        memcpy(frame.payload, payload, payload_length);
    }

    channel_result = comm_frame_channel_push(
        manager->tx_channel,
        &frame,
        manager->config.send_timeout_ms);
    if (channel_result != COMM_FRAME_CHANNEL_OK) {
        return comm_message_manager_map_channel_result(channel_result);
    }

    /*
     * 通道确认接收后才提交 pending。active 最后写入，使槽位在完整字段就绪前
     * 始终保持无效。manager 由单一任务调用，因此这里不需要额外互斥锁。
     */
    pending->request = frame;
    pending->deadline_ms = comm_message_manager_make_deadline(
        now_ms,
        manager->config.response_timeout_ms);
    pending->retries_done = 0u;
    pending->active = 1;

    manager->next_sequence = comm_message_manager_next_sequence(candidate);
    *sequence = candidate;

    return COMM_MESSAGE_MANAGER_OK;
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

comm_message_manager_result_t comm_message_manager_handle_frame(
    comm_message_manager_t *manager,
    const comm_frame_t *frame)
{
    comm_message_event_t event;
    comm_message_pending_t *pending = NULL;

    if ((manager == NULL) || (frame == NULL)) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    /*
     * handle_frame 会读取和修改 pending，因此除了长期配置外，还必须确认活动
     * pending 的内部状态有效。传入帧的版本、类型和长度已经由 Parser/Codec
     * 校验，这里只处理请求、回复、上报和错误之间的消息语义。
     */
    if (!comm_message_manager_has_valid_pending_state(manager)) {
        return COMM_MESSAGE_MANAGER_INVALID_STATE;
    }

    event.frame = *frame;
    event.retries_done = 0u;

    switch (frame->type) {
        case COMM_FRAME_TYPE_REQUEST:
            /*
             * 对端请求不属于本端已经发出的事务，因此不查 pending，直接交给
             * 上层业务处理。上层可使用同一个 sequence 发送 RESPONSE/ERROR。
             */
            event.type = COMM_MESSAGE_EVENT_REQUEST_RECEIVED;
            break;

        case COMM_FRAME_TYPE_REPORT:
            /* 主动上报不要求请求与回复匹配，也不改变任何 pending。 */
            event.type = COMM_MESSAGE_EVENT_REPORT_RECEIVED;
            break;

        case COMM_FRAME_TYPE_RESPONSE:
        case COMM_FRAME_TYPE_ERROR:
            /*
             * sequence 是收到的回复与未完成请求之间的匹配键。可能存在多个
             * 活动 pending，但有效状态保证它们的 sequence 互不重复。
             */
            pending = comm_message_manager_find_pending_by_sequence(
                manager,
                frame->sequence);

            if (pending == NULL) {
                /*
                 * 未找到通常意味着回复迟到、重复到达，或者对端使用了本端
                 * 从未分配的 sequence。保留原帧通知上层，便于诊断和统计。
                 */
                event.type = COMM_MESSAGE_EVENT_UNMATCHED_REPLY;
                break;
            }

            event.retries_done = pending->retries_done;
            event.type = (frame->type == COMM_FRAME_TYPE_RESPONSE)
                             ? COMM_MESSAGE_EVENT_RESPONSE_RECEIVED
                             : COMM_MESSAGE_EVENT_ERROR_RECEIVED;

            /*
             * 回复已经结束该请求事务。只需清除 active：其余字段成为旧数据，
             * 后续新请求复用此槽位时会完整覆盖。先释放再回调，保证上层观察
             * 到的 manager 状态与“已收到最终回复”一致。
             */
            pending->active = 0;
            break;

        default:
            /* 正常 Parser 不会输出未知类型，保留检查防止接口被错误调用。 */
            return COMM_MESSAGE_MANAGER_UNSUPPORTED_FRAME_TYPE;
    }

    manager->event_callback(manager->event_context, &event);
    return COMM_MESSAGE_MANAGER_OK;
}

comm_message_manager_result_t comm_message_manager_process_timeouts(
    comm_message_manager_t *manager,
    uint64_t now_ms)
{
    comm_message_manager_result_t overall_result = COMM_MESSAGE_MANAGER_OK;
    comm_message_manager_result_t retry_result;
    comm_frame_channel_result_t channel_result;
    comm_message_pending_t *pending;
    comm_message_event_t event;
    size_t index;

    if (manager == NULL) {
        return COMM_MESSAGE_MANAGER_NULL_ARGUMENT;
    }

    if (!comm_message_manager_has_valid_pending_state(manager)) {
        return COMM_MESSAGE_MANAGER_INVALID_STATE;
    }

    for (index = 0u; index < manager->pending_capacity; ++index) {
        pending = &manager->pending_storage[index];

        /*
         * inactive 槽位没有正在等待的请求；now_ms 小于 deadline_ms 则说明
         * 回复等待窗口尚未结束。两种情况都不能修改槽位。
         *
         * 使用 now_ms >= deadline_ms 判断到期，因此正好走到截止时刻就会处理。
         * deadline_ms 由饱和加法生成，不会因 uint64_t 溢出绕回到很小的值。
         */
        if ((pending->active == 0) || (now_ms < pending->deadline_ms)) {
            continue;
        }

        if (pending->retries_done >= manager->config.max_retries) {
            /*
             * 初次发送不计入 retries_done。当已重发次数达到 max_retries 后，
             * 本次到期不再尝试发送，而是结束事务并报告最终超时。
             */
            event.type = COMM_MESSAGE_EVENT_REQUEST_TIMEOUT;
            event.frame = pending->request;
            event.retries_done = pending->retries_done;

            /*
             * 与收到匹配回复时相同，先清除 active 再调用同步回调。回调看到
             * REQUEST_TIMEOUT 时，对应 sequence 已经不再占用 pending 槽位。
             */
            pending->active = 0;
            manager->event_callback(manager->event_context, &event);
            continue;
        }

        /*
         * 重发保存下来的原始 REQUEST，尤其不能分配新 sequence。对端可能只是
         * 回复较慢；保持 sequence 不变，迟到回复和本次重发的回复都能结束同一
         * 个 pending 请求事务。
         */
        channel_result = comm_frame_channel_push(
            manager->tx_channel,
            &pending->request,
            manager->config.send_timeout_ms);
        retry_result = comm_message_manager_map_channel_result(channel_result);

        if (retry_result != COMM_MESSAGE_MANAGER_OK) {
            /*
             * 通道没有接收帧，所以这不算一次成功重发：不增加次数、不刷新
             * deadline，也不释放 pending。继续检查其他槽位，避免它们被阻挡。
             */
            if (overall_result == COMM_MESSAGE_MANAGER_OK) {
                overall_result = retry_result;
            }
            continue;
        }

        /*
         * 通道确认接收后才提交新的重试状态。新的等待窗口从本次调用提供的
         * now_ms 开始，而不是从旧 deadline 延伸，避免任务调度较晚时连续补发。
         */
        pending->retries_done = (uint8_t)(pending->retries_done + 1u);
        pending->deadline_ms = comm_message_manager_make_deadline(
            now_ms,
            manager->config.response_timeout_ms);
    }

    return overall_result;
}
