#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "comm_frame_queue_pthread.h"

#include <errno.h>
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

/*
 * 核心队列的 size/free_space 在状态无效时都会返回 0。有效且容量大于 0
 * 的队列不可能同时得到两个 0，因此可以用二者之和复核内部状态。
 */
static int comm_frame_queue_pthread_core_is_valid(
    const comm_frame_queue_pthread_t *queue)
{
    size_t used;
    size_t free_space;

    if ((queue->core.storage == NULL) || (queue->core.capacity == 0u)) {
        return 0;
    }

    used = comm_frame_queue_size(&queue->core);
    free_space = comm_frame_queue_free_space(&queue->core);

    return (used + free_space) == queue->core.capacity;
}

/*
 * 所有已加锁的返回路径都通过这里解锁，避免新增错误分支时漏掉 unlock。
 * 如果解锁本身失败，队列是否仍被锁住已经无法确定，应优先报告系统错误。
 */
static comm_frame_queue_pthread_result_t
comm_frame_queue_pthread_unlock_and_return(
    comm_frame_queue_pthread_t *queue,
    comm_frame_queue_pthread_result_t result)
{
    if (pthread_mutex_unlock(&queue->mutex) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    return result;
}

/*
 * 把相对毫秒数转换为 pthread_cond_timedwait 要求的绝对截止时间。
 * deadline 只计算一次，后续即使发生虚假唤醒也继续使用同一截止时间，
 * 从而保证多次等待不会各自重新获得一整段 timeout_ms。
 */
static int comm_frame_queue_pthread_make_deadline(
    uint32_t timeout_ms,
    struct timespec *deadline)
{
    long additional_nanoseconds;

    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
        return 0;
    }

    deadline->tv_sec += (time_t)(timeout_ms / 1000u);
    additional_nanoseconds =
        (long)((timeout_ms % 1000u) * 1000000u);
    deadline->tv_nsec += additional_nanoseconds;

    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += (time_t)1;
        deadline->tv_nsec -= 1000000000L;
    }

    return 1;
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

comm_frame_queue_pthread_result_t comm_frame_queue_pthread_push(
    comm_frame_queue_pthread_t *queue,
    const comm_frame_t *frame,
    uint32_t timeout_ms)
{
    struct timespec deadline;
    int wait_result;
    int signal_result;
    comm_frame_queue_result_t core_result;

    if ((queue == NULL) || (frame == NULL)) {
        return COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT;
    }

    if (queue->initialized != 1) {
        return COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE;
    }

    /*
     * 有限等待的截止时间在加锁前计算，使 mutex 上消耗的时间也尽量计入
     * 调用总时长。pthread_mutex_lock 本身没有单调时钟超时接口，因此在
     * 极端调度情况下，实际返回时间仍可能略晚于 deadline。
     */
    if ((timeout_ms != 0u) &&
        (timeout_ms != COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER) &&
        !comm_frame_queue_pthread_make_deadline(timeout_ms, &deadline)) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
    }

    if (!comm_frame_queue_pthread_core_is_valid(queue)) {
        return comm_frame_queue_pthread_unlock_and_return(
            queue,
            COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    }

    /*
     * 必须使用 while，不能使用 if：条件变量允许虚假唤醒；即使确实由一次
     * 出队唤醒，多个生产者也可能竞争同一个空槽位。线程重新获得 mutex
     * 后，只有再次检查队列状态，才能确认自己真的可以入队。
     */
    while ((queue->closed == 0) &&
           (queue->core.used == queue->core.capacity)) {
        if (timeout_ms == 0u) {
            return comm_frame_queue_pthread_unlock_and_return(
                queue,
                COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
        }

        if (timeout_ms == COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER) {
            wait_result = pthread_cond_wait(&queue->not_full,
                                            &queue->mutex);
        } else {
            wait_result = pthread_cond_timedwait(&queue->not_full,
                                                 &queue->mutex,
                                                 &deadline);
        }

        /*
         * wait 返回前会重新获得 mutex，所以这里可以安全读取 closed。
         * 若关闭与超时几乎同时发生，优先返回 CLOSED，让调用方进入退出流程。
         */
        if (wait_result == ETIMEDOUT) {
            if (queue->closed != 0) {
                return comm_frame_queue_pthread_unlock_and_return(
                    queue,
                    COMM_FRAME_QUEUE_PTHREAD_CLOSED);
            }

            return comm_frame_queue_pthread_unlock_and_return(
                queue,
                COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
        }

        if (wait_result != 0) {
            return comm_frame_queue_pthread_unlock_and_return(
                queue,
                COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR);
        }

        if (!comm_frame_queue_pthread_core_is_valid(queue)) {
            return comm_frame_queue_pthread_unlock_and_return(
                queue,
                COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
        }
    }

    /* close 后即使仍有空槽位也禁止生产新帧。 */
    if (queue->closed != 0) {
        return comm_frame_queue_pthread_unlock_and_return(
            queue,
            COMM_FRAME_QUEUE_PTHREAD_CLOSED);
    }

    core_result = comm_frame_queue_push(&queue->core, frame);
    if (core_result != COMM_FRAME_QUEUE_OK) {
        return comm_frame_queue_pthread_unlock_and_return(
            queue,
            COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    }

    /*
     * 一次入队只新增一帧，最多能满足一个消费者，因此 signal 一个等待者
     * 即可。状态修改和 signal 都发生在 mutex 内，消费者醒来后必须等本线程
     * 解锁，随后才能观察到完整写入的帧。
     */
    signal_result = pthread_cond_signal(&queue->not_empty);
    if (signal_result != 0) {
        return comm_frame_queue_pthread_unlock_and_return(
            queue,
            COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR);
    }

    return comm_frame_queue_pthread_unlock_and_return(
        queue,
        COMM_FRAME_QUEUE_PTHREAD_OK);
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
