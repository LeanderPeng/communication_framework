#include "comm_frame_channel_pthread.h"

#include <stddef.h>

/*
 * 把 pthread 后端结果压缩为平台无关结果。
 *
 * TIMEOUT 和 CLOSED 是消息管理器可以正常处理的运行状态，因此原样保留。
 * 参数、容量、内部状态和 pthread 系统调用错误说明后端没有按约定工作，
 * 对平台无关层统一表现为 BACKEND_ERROR，避免上层依赖 pthread 错误细节。
 */
static comm_frame_channel_result_t comm_frame_channel_pthread_map_result(
    comm_frame_queue_pthread_result_t result)
{
    switch (result) {
        case COMM_FRAME_QUEUE_PTHREAD_OK:
            return COMM_FRAME_CHANNEL_OK;

        case COMM_FRAME_QUEUE_PTHREAD_TIMEOUT:
            return COMM_FRAME_CHANNEL_TIMEOUT;

        case COMM_FRAME_QUEUE_PTHREAD_CLOSED:
            return COMM_FRAME_CHANNEL_CLOSED;

        case COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT:
        case COMM_FRAME_QUEUE_PTHREAD_INVALID_CAPACITY:
        case COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE:
        case COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR:
        default:
            return COMM_FRAME_CHANNEL_BACKEND_ERROR;
    }
}

/*
 * 通道只认识 void *，本适配函数知道 context 实际指向 pthread 队列，
 * 因此在这里恢复具体类型并调用真正的后端操作。
 */
static comm_frame_channel_result_t comm_frame_channel_pthread_push(
    void *context,
    const comm_frame_t *frame,
    uint32_t timeout_ms)
{
    comm_frame_queue_pthread_t *queue =
        (comm_frame_queue_pthread_t *)context;

    return comm_frame_channel_pthread_map_result(
        comm_frame_queue_pthread_push(queue, frame, timeout_ms));
}

static comm_frame_channel_result_t comm_frame_channel_pthread_pop(
    void *context,
    comm_frame_t *frame,
    uint32_t timeout_ms)
{
    comm_frame_queue_pthread_t *queue =
        (comm_frame_queue_pthread_t *)context;

    return comm_frame_channel_pthread_map_result(
        comm_frame_queue_pthread_pop(queue, frame, timeout_ms));
}

static comm_frame_channel_result_t comm_frame_channel_pthread_close(
    void *context)
{
    comm_frame_queue_pthread_t *queue =
        (comm_frame_queue_pthread_t *)context;

    return comm_frame_channel_pthread_map_result(
        comm_frame_queue_pthread_close(queue));
}

comm_frame_channel_result_t comm_frame_channel_pthread_bind(
    comm_frame_channel_t *channel,
    comm_frame_queue_pthread_t *queue)
{
    if ((channel == NULL) || (queue == NULL)) {
        return COMM_FRAME_CHANNEL_NULL_ARGUMENT;
    }

    /*
     * channel 不负责初始化后端。这里提前拒绝未初始化队列，避免创建一个
     * 形式上函数指针齐全、实际第一次使用就失败的无效绑定。
     * queue 的销毁不能和本函数或通道操作并发，生命周期由调用方协调。
     */
    if (queue->initialized != 1) {
        return COMM_FRAME_CHANNEL_INVALID_STATE;
    }

    return comm_frame_channel_bind(channel,
                                   queue,
                                   comm_frame_channel_pthread_push,
                                   comm_frame_channel_pthread_pop,
                                   comm_frame_channel_pthread_close);
}
