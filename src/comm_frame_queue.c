#include "comm_frame_queue.h"

/* 与 Parser 一样使用饱和计数，避免长期运行后累计值绕回零。 */
static void comm_frame_queue_increment_counter(uint64_t *counter)
{
    if (*counter != UINT64_MAX) {
        *counter += 1u;
    }
}

/* 只观察当前积压量；一次出队不能让历史峰值下降。 */
static void comm_frame_queue_observe_size(comm_frame_queue_stats_t *stats,
                                          size_t used)
{
    if ((stats != NULL) && (used > stats->peak_size)) {
        stats->peak_size = used;
    }
}

/* 判断帧队列是否仍持有可用的底层存储配置。 */
static int comm_frame_queue_has_storage(const comm_frame_queue_t *queue)
{
    return (queue != NULL) &&
           (queue->storage != NULL) &&
           (queue->capacity > 0u);
}

/* 在不产生 size_t 加法溢出的前提下，将下标沿队列向前移动。 */
static size_t comm_frame_queue_advance_index(size_t index,
                                             size_t amount,
                                             size_t capacity)
{
    size_t distance_to_end = capacity - index;

    if (amount < distance_to_end) {
        return index + amount;
    }

    return amount - distance_to_end;
}

/* 检查队列正常运行时必须始终满足的状态不变量。 */
static int comm_frame_queue_state_is_valid(const comm_frame_queue_t *queue)
{
    size_t expected_write_index;

    if (!comm_frame_queue_has_storage(queue)) {
        return 0;
    }

    if ((queue->read_index >= queue->capacity) ||
        (queue->write_index >= queue->capacity) ||
        (queue->used > queue->capacity)) {
        return 0;
    }

    expected_write_index = comm_frame_queue_advance_index(
        queue->read_index,
        queue->used,
        queue->capacity);

    return queue->write_index == expected_write_index;
}

comm_frame_queue_result_t comm_frame_queue_init(comm_frame_queue_t *queue,
                                                comm_frame_t *storage,
                                                size_t capacity)
{
    if ((queue == NULL) || (storage == NULL)) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    if (capacity == 0u) {
        return COMM_FRAME_QUEUE_INVALID_CAPACITY;
    }

    queue->storage = storage;
    queue->capacity = capacity;
    queue->read_index = 0u;
    queue->write_index = 0u;
    queue->used = 0u;

    return COMM_FRAME_QUEUE_OK;
}

comm_frame_queue_result_t comm_frame_queue_reset(comm_frame_queue_t *queue)
{
    if (queue == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    /* reset 可以修复运行状态，但底层帧数组配置必须仍然有效。 */
    if (!comm_frame_queue_has_storage(queue)) {
        return COMM_FRAME_QUEUE_INVALID_STATE;
    }

    queue->read_index = 0u;
    queue->write_index = 0u;
    queue->used = 0u;

    return COMM_FRAME_QUEUE_OK;
}

size_t comm_frame_queue_size(const comm_frame_queue_t *queue)
{
    if (!comm_frame_queue_state_is_valid(queue)) {
        return 0u;
    }

    return queue->used;
}

size_t comm_frame_queue_free_space(const comm_frame_queue_t *queue)
{
    if (!comm_frame_queue_state_is_valid(queue)) {
        return 0u;
    }

    return queue->capacity - queue->used;
}

comm_frame_queue_result_t comm_frame_queue_stats_reset(
    const comm_frame_queue_t *queue,
    comm_frame_queue_stats_t *stats)
{
    if ((queue == NULL) || (stats == NULL)) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    if (!comm_frame_queue_state_is_valid(queue)) {
        return COMM_FRAME_QUEUE_INVALID_STATE;
    }

    stats->frames_pushed = 0u;
    stats->frames_popped = 0u;
    stats->push_full_count = 0u;
    stats->peak_size = queue->used;

    return COMM_FRAME_QUEUE_OK;
}

/* 原接口与带统计接口共享所有校验和队列操作；NULL 表示不记录统计。 */
static comm_frame_queue_result_t comm_frame_queue_push_internal(
    comm_frame_queue_t *queue,
    const comm_frame_t *frame,
    comm_frame_queue_stats_t *stats)
{
    if (queue == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    if (!comm_frame_queue_state_is_valid(queue)) {
        return COMM_FRAME_QUEUE_INVALID_STATE;
    }

    if (frame == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    comm_frame_queue_observe_size(stats, queue->used);
    if (queue->used == queue->capacity) {
        if (stats != NULL) {
            comm_frame_queue_increment_counter(&stats->push_full_count);
        }
        return COMM_FRAME_QUEUE_FULL;
    }

    queue->storage[queue->write_index] = *frame;
    queue->write_index = comm_frame_queue_advance_index(
        queue->write_index,
        1u,
        queue->capacity);
    queue->used += 1u;

    if (stats != NULL) {
        comm_frame_queue_increment_counter(&stats->frames_pushed);
        comm_frame_queue_observe_size(stats, queue->used);
    }

    return COMM_FRAME_QUEUE_OK;
}

static comm_frame_queue_result_t comm_frame_queue_pop_internal(
    comm_frame_queue_t *queue,
    comm_frame_t *frame,
    comm_frame_queue_stats_t *stats)
{
    if (queue == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    if (!comm_frame_queue_state_is_valid(queue)) {
        return COMM_FRAME_QUEUE_INVALID_STATE;
    }

    if (frame == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    comm_frame_queue_observe_size(stats, queue->used);
    if (queue->used == 0u) {
        return COMM_FRAME_QUEUE_EMPTY;
    }

    *frame = queue->storage[queue->read_index];
    queue->read_index = comm_frame_queue_advance_index(
        queue->read_index,
        1u,
        queue->capacity);
    queue->used -= 1u;

    if (stats != NULL) {
        comm_frame_queue_increment_counter(&stats->frames_popped);
    }

    return COMM_FRAME_QUEUE_OK;
}

comm_frame_queue_result_t comm_frame_queue_push(comm_frame_queue_t *queue,
                                                const comm_frame_t *frame)
{
    return comm_frame_queue_push_internal(queue, frame, NULL);
}

comm_frame_queue_result_t comm_frame_queue_pop(comm_frame_queue_t *queue,
                                               comm_frame_t *frame)
{
    return comm_frame_queue_pop_internal(queue, frame, NULL);
}

comm_frame_queue_result_t comm_frame_queue_push_with_stats(
    comm_frame_queue_t *queue,
    const comm_frame_t *frame,
    comm_frame_queue_stats_t *stats)
{
    if (stats == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    return comm_frame_queue_push_internal(queue, frame, stats);
}

comm_frame_queue_result_t comm_frame_queue_pop_with_stats(
    comm_frame_queue_t *queue,
    comm_frame_t *frame,
    comm_frame_queue_stats_t *stats)
{
    if (stats == NULL) {
        return COMM_FRAME_QUEUE_NULL_ARGUMENT;
    }

    return comm_frame_queue_pop_internal(queue, frame, stats);
}
