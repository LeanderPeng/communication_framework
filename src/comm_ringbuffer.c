#include "comm_ringbuffer.h"

/* 判断 RingBuffer 是否仍持有可用的底层存储配置。 */
static int comm_ringbuffer_has_storage(const comm_ringbuffer_t *ringbuffer)
{
    return (ringbuffer != NULL) &&
           (ringbuffer->storage != NULL) &&
           (ringbuffer->capacity > 0u);
}

/* 检查模块正常运行时必须始终满足的状态不变量。 */
static int comm_ringbuffer_state_is_valid(
    const comm_ringbuffer_t *ringbuffer)
{
    size_t distance_to_end;
    size_t expected_write_index;

    if (!comm_ringbuffer_has_storage(ringbuffer)) {
        return 0;
    }

    if ((ringbuffer->read_index >= ringbuffer->capacity) ||
        (ringbuffer->write_index >= ringbuffer->capacity) ||
        (ringbuffer->used > ringbuffer->capacity)) {
        return 0;
    }

    if ((ringbuffer->used == 0u) ||
        (ringbuffer->used == ringbuffer->capacity)) {
        return ringbuffer->read_index == ringbuffer->write_index;
    }

    distance_to_end = ringbuffer->capacity - ringbuffer->read_index;
    if (ringbuffer->used < distance_to_end) {
        expected_write_index = ringbuffer->read_index + ringbuffer->used;
    } else {
        expected_write_index = ringbuffer->used - distance_to_end;
    }

    return ringbuffer->write_index == expected_write_index;
}

comm_ringbuffer_result_t comm_ringbuffer_init(comm_ringbuffer_t *ringbuffer,
                                              uint8_t *storage,
                                              size_t capacity)
{
    if ((ringbuffer == NULL) || (storage == NULL)) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    if (capacity == 0u) {
        return COMM_RINGBUFFER_INVALID_CAPACITY;
    }

    ringbuffer->storage = storage;
    ringbuffer->capacity = capacity;
    ringbuffer->read_index = 0u;
    ringbuffer->write_index = 0u;
    ringbuffer->used = 0u;

    return COMM_RINGBUFFER_OK;
}

comm_ringbuffer_result_t comm_ringbuffer_reset(
    comm_ringbuffer_t *ringbuffer)
{
    if (ringbuffer == NULL) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    /* reset 允许修复下标和 used，但底层存储配置必须仍然有效。 */
    if (!comm_ringbuffer_has_storage(ringbuffer)) {
        return COMM_RINGBUFFER_INVALID_STATE;
    }

    ringbuffer->read_index = 0u;
    ringbuffer->write_index = 0u;
    ringbuffer->used = 0u;

    return COMM_RINGBUFFER_OK;
}

size_t comm_ringbuffer_size(const comm_ringbuffer_t *ringbuffer)
{
    if (!comm_ringbuffer_state_is_valid(ringbuffer)) {
        return 0u;
    }

    return ringbuffer->used;
}

size_t comm_ringbuffer_free_space(const comm_ringbuffer_t *ringbuffer)
{
    if (!comm_ringbuffer_state_is_valid(ringbuffer)) {
        return 0u;
    }

    return ringbuffer->capacity - ringbuffer->used;
}
