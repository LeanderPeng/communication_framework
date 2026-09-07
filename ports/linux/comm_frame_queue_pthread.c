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
