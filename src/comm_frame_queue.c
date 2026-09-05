#include "comm_frame_queue.h"

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
