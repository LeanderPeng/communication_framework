#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "comm_frame_queue_pthread.h"

#include <time.h>

/* 将对象恢复为不持有任何可用资源的状态。 */
static void comm_frame_queue_pthread_mark_uninitialized(
    comm_frame_queue_pthread_t *queue)
{
    queue->core.storage = NULL;
    queue->core.capacity = 0u;
    queue->core.read_index = 0u;
    queue->core.write_index = 0u;
    queue->core.used = 0u;
    queue->initialized = 0;
    queue->closed = 0;
}

comm_frame_queue_pthread_result_t comm_frame_queue_pthread_init(
    comm_frame_queue_pthread_t *queue,
    comm_frame_t *storage,
    size_t capacity)
{
    pthread_condattr_t condition_attribute;

    if ((queue == NULL) || (storage == NULL)) {
        return COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT;
    }

    if (capacity == 0u) {
        return COMM_FRAME_QUEUE_PTHREAD_INVALID_CAPACITY;
    }

    comm_frame_queue_pthread_mark_uninitialized(queue);

    if (comm_frame_queue_init(&queue->core, storage, capacity) !=
        COMM_FRAME_QUEUE_OK) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_condattr_init(&condition_attribute) != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_condattr_setclock(&condition_attribute, CLOCK_MONOTONIC) != 0) {
        (void)pthread_condattr_destroy(&condition_attribute);
        (void)pthread_mutex_destroy(&queue->mutex);
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_cond_init(&queue->not_empty, &condition_attribute) != 0) {
        (void)pthread_condattr_destroy(&condition_attribute);
        (void)pthread_mutex_destroy(&queue->mutex);
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_cond_init(&queue->not_full, &condition_attribute) != 0) {
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_condattr_destroy(&condition_attribute);
        (void)pthread_mutex_destroy(&queue->mutex);
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_condattr_destroy(&condition_attribute) != 0) {
        (void)pthread_cond_destroy(&queue->not_full);
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->mutex);
        comm_frame_queue_pthread_mark_uninitialized(queue);
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    queue->initialized = 1;
    queue->closed = 0;

    return COMM_FRAME_QUEUE_PTHREAD_OK;
}

comm_frame_queue_pthread_result_t comm_frame_queue_pthread_close(
    comm_frame_queue_pthread_t *queue)
{
    int not_empty_result;
    int not_full_result;
    int unlock_result;

    if (queue == NULL) {
        return COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT;
    }

    /*
     * initialized 不由 mutex 保护，因为 destroy 的调用前提本来就是：
     * 调用方已经停止并回收所有使用该队列的线程。也就是说 close 与
     * destroy 不能并发执行，生命周期由更外层代码负责协调。
     */
    if (queue->initialized != 1) {
        return COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    /*
     * 条件变量本身不保存“关闭事件”，真正持久的状态是 closed。
     * push/pop 将在同一把 mutex 下检查 closed 和队列满/空条件，并使用
     * while 循环等待。因此这里必须先在锁内写入 closed，再执行广播，
     * 避免线程在“检查条件”和“进入等待”之间漏掉关闭通知。
     */
    queue->closed = 1;

    /*
     * close 需要唤醒所有等待者，所以使用 broadcast 而不是 signal：
     * not_empty 上可能睡着多个消费者，not_full 上也可能睡着多个生产者。
     * 被唤醒的线程不会立刻并行访问队列，它们必须等这里释放 mutex 后，
     * 逐个重新获得锁并检查 closed。
     *
     * 即使重复 close，也再次广播。这样 close 保持幂等，同时第一次广播
     * 若遇到罕见系统错误，调用方仍可以再次 close 尝试唤醒等待线程。
     */
    not_empty_result = pthread_cond_broadcast(&queue->not_empty);
    not_full_result = pthread_cond_broadcast(&queue->not_full);

    /* 广播失败也必须释放锁，否则其他线程会永久阻塞在 mutex 上。 */
    unlock_result = pthread_mutex_unlock(&queue->mutex);

    if ((not_empty_result != 0) ||
        (not_full_result != 0) ||
        (unlock_result != 0)) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    return COMM_FRAME_QUEUE_PTHREAD_OK;
}

comm_frame_queue_pthread_result_t comm_frame_queue_pthread_destroy(
    comm_frame_queue_pthread_t *queue)
{
    if (queue == NULL) {
        return COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT;
    }

    if (queue->initialized != 1) {
        return COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE;
    }

    if (pthread_cond_destroy(&queue->not_full) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_cond_destroy(&queue->not_empty) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_mutex_destroy(&queue->mutex) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    comm_frame_queue_pthread_mark_uninitialized(queue);

    return COMM_FRAME_QUEUE_PTHREAD_OK;
}
