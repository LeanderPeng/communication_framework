#include "comm_frame_channel.h"

#include <stddef.h>

/*
 * 判断通道是否持有一组完整绑定。
 *
 * 只检查当前转发层拥有的信息：后端对象地址和三个操作函数。后端对象本身
 * 是否已经正确初始化，由对应的平台适配函数在真正执行操作时负责检查。
 */
static int comm_frame_channel_is_bound(const comm_frame_channel_t *channel)
{
    return (channel != NULL) &&
           (channel->context != NULL) &&
           (channel->push != NULL) &&
           (channel->pop != NULL) &&
           (channel->close != NULL);
}

comm_frame_channel_result_t comm_frame_channel_bind(
    comm_frame_channel_t *channel,
    void *context,
    comm_frame_channel_push_fn push,
    comm_frame_channel_pop_fn pop,
    comm_frame_channel_close_fn close)
{
    /*
     * 要求一次提供完整绑定，而不是允许“只支持 push”之类的半成品通道。
     * 所有参数验证都在写 channel 之前完成，因此失败时不会留下部分更新。
     */
    if ((channel == NULL) ||
        (context == NULL) ||
        (push == NULL) ||
        (pop == NULL) ||
        (close == NULL)) {
        return COMM_FRAME_CHANNEL_NULL_ARGUMENT;
    }

    channel->context = context;
    channel->push = push;
    channel->pop = pop;
    channel->close = close;

    return COMM_FRAME_CHANNEL_OK;
}

comm_frame_channel_result_t comm_frame_channel_push(
    const comm_frame_channel_t *channel,
    const comm_frame_t *frame,
    uint32_t timeout_ms)
{
    if ((channel == NULL) || (frame == NULL)) {
        return COMM_FRAME_CHANNEL_NULL_ARGUMENT;
    }

    if (!comm_frame_channel_is_bound(channel)) {
        return COMM_FRAME_CHANNEL_INVALID_STATE;
    }

    /*
     * context 的真实类型在此处有意保持未知。平台回调会把它转换回自己的
     * 后端类型，例如 comm_frame_queue_pthread_t，然后执行具体入队操作。
     */
    return channel->push(channel->context, frame, timeout_ms);
}

comm_frame_channel_result_t comm_frame_channel_pop(
    const comm_frame_channel_t *channel,
    comm_frame_t *frame,
    uint32_t timeout_ms)
{
    if ((channel == NULL) || (frame == NULL)) {
        return COMM_FRAME_CHANNEL_NULL_ARGUMENT;
    }

    if (!comm_frame_channel_is_bound(channel)) {
        return COMM_FRAME_CHANNEL_INVALID_STATE;
    }

    return channel->pop(channel->context, frame, timeout_ms);
}

comm_frame_channel_result_t comm_frame_channel_close(
    const comm_frame_channel_t *channel)
{
    if (channel == NULL) {
        return COMM_FRAME_CHANNEL_NULL_ARGUMENT;
    }

    if (!comm_frame_channel_is_bound(channel)) {
        return COMM_FRAME_CHANNEL_INVALID_STATE;
    }

    return channel->close(channel->context);
}
