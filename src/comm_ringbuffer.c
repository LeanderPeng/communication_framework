#include "comm_ringbuffer.h"

#include <string.h>

/* 判断 RingBuffer 是否仍持有可用的底层存储配置。 */
static int comm_ringbuffer_has_storage(const comm_ringbuffer_t *ringbuffer)
{
    return (ringbuffer != NULL) &&
           (ringbuffer->storage != NULL) &&
           (ringbuffer->capacity > 0u);
}

/* 在不产生 size_t 加法溢出的前提下，将下标沿环向前移动。 */
static size_t comm_ringbuffer_advance_index(size_t index,
                                            size_t amount,
                                            size_t capacity)
{
    size_t distance_to_end = capacity - index;

    if (amount < distance_to_end) {
        return index + amount;
    }

    return amount - distance_to_end;
}

/* 检查模块正常运行时必须始终满足的状态不变量。 */
static int comm_ringbuffer_state_is_valid(
    const comm_ringbuffer_t *ringbuffer)
{
    size_t expected_write_index;

    if (!comm_ringbuffer_has_storage(ringbuffer)) {
        return 0;
    }

    if ((ringbuffer->read_index >= ringbuffer->capacity) ||
        (ringbuffer->write_index >= ringbuffer->capacity) ||
        (ringbuffer->used > ringbuffer->capacity)) {
        return 0;
    }

    expected_write_index = comm_ringbuffer_advance_index(
        ringbuffer->read_index,
        ringbuffer->used,
        ringbuffer->capacity);

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

comm_ringbuffer_result_t comm_ringbuffer_write(
    comm_ringbuffer_t *ringbuffer,
    const uint8_t *data,
    size_t length)
{
    size_t first_part_size;
    size_t second_part_size;
    size_t free_space;

    if (ringbuffer == NULL) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    if (!comm_ringbuffer_state_is_valid(ringbuffer)) {
        return COMM_RINGBUFFER_INVALID_STATE;
    }

    if (length == 0u) {
        return COMM_RINGBUFFER_OK;
    }

    if (data == NULL) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    free_space = ringbuffer->capacity - ringbuffer->used;
    if (length > free_space) {
        return COMM_RINGBUFFER_INSUFFICIENT_SPACE;
    }

    first_part_size = ringbuffer->capacity - ringbuffer->write_index;
    if (first_part_size > length) {
        first_part_size = length;
    }
    second_part_size = length - first_part_size;

    memcpy(&ringbuffer->storage[ringbuffer->write_index],
           data,
           first_part_size);
    if (second_part_size > 0u) {
        memcpy(ringbuffer->storage,
               &data[first_part_size],
               second_part_size);
    }

    ringbuffer->write_index = comm_ringbuffer_advance_index(
        ringbuffer->write_index,
        length,
        ringbuffer->capacity);
    ringbuffer->used += length;

    return COMM_RINGBUFFER_OK;
}

comm_ringbuffer_result_t comm_ringbuffer_read(comm_ringbuffer_t *ringbuffer,
                                              uint8_t *output,
                                              size_t length)
{
    size_t first_part_size;
    size_t second_part_size;

    if (ringbuffer == NULL) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    if (!comm_ringbuffer_state_is_valid(ringbuffer)) {
        return COMM_RINGBUFFER_INVALID_STATE;
    }

    if (length == 0u) {
        return COMM_RINGBUFFER_OK;
    }

    if (output == NULL) {
        return COMM_RINGBUFFER_NULL_ARGUMENT;
    }

    if (length > ringbuffer->used) {
        return COMM_RINGBUFFER_INSUFFICIENT_DATA;
    }

    first_part_size = ringbuffer->capacity - ringbuffer->read_index;
    if (first_part_size > length) {
        first_part_size = length;
    }
    second_part_size = length - first_part_size;

    memcpy(output,
           &ringbuffer->storage[ringbuffer->read_index],
           first_part_size);
    if (second_part_size > 0u) {
        memcpy(&output[first_part_size],
               ringbuffer->storage,
               second_part_size);
    }

    ringbuffer->read_index = comm_ringbuffer_advance_index(
        ringbuffer->read_index,
        length,
        ringbuffer->capacity);
    ringbuffer->used -= length;

    return COMM_RINGBUFFER_OK;
}
