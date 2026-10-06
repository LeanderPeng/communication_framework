#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "comm_frame_channel_pthread.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "test failed: %s (%s:%d)\n",                    \
                    #condition, __FILE__, __LINE__);                          \
            return 1;                                                        \
        }                                                                    \
    } while (0)
#define CHECK_OK(call) CHECK((call) == COMM_FRAME_QUEUE_PTHREAD_OK)

static int stats_equal(const comm_frame_queue_pthread_stats_t *a,
                       const comm_frame_queue_pthread_stats_t *b)
{
    return a->frames_pushed == b->frames_pushed &&
           a->frames_popped == b->frames_popped &&
           a->push_full_encounters == b->push_full_encounters &&
           a->pop_empty_encounters == b->pop_empty_encounters &&
           a->push_wait_count == b->push_wait_count &&
           a->pop_wait_count == b->pop_wait_count &&
           a->push_timeout_count == b->push_timeout_count &&
           a->pop_timeout_count == b->pop_timeout_count &&
           a->peak_size == b->peak_size;
}

static int test_validation_and_optional_stats(void)
{
    comm_frame_queue_pthread_t queue = {0};
    comm_frame_t storage[1];
    comm_frame_t frame = {0};
    comm_frame_queue_pthread_stats_t stats = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    comm_frame_queue_pthread_stats_t expected = stats;
    comm_frame_queue_pthread_stats_t output = stats;
    comm_frame_queue_pthread_stats_t before;

    CHECK(comm_frame_queue_pthread_init_with_stats(NULL, storage, 1u, &stats) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_init_with_stats(&queue, NULL, 1u, &stats) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 1u, NULL) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 0u, &stats) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_CAPACITY);
    CHECK(stats_equal(&stats, &expected));
    CHECK(queue.initialized == 0 && queue.stats == NULL);
    CHECK(comm_frame_queue_pthread_get_stats(NULL, &output) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_get_stats(&queue, NULL) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_get_stats(&queue, &output) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(comm_frame_queue_pthread_reset_stats(NULL) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_reset_stats(&queue) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(stats_equal(&output, &expected));

    CHECK_OK(comm_frame_queue_pthread_init(&queue, storage, 1u));
    CHECK(queue.stats == NULL);
    CHECK(comm_frame_queue_pthread_get_stats(&queue, &output) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(comm_frame_queue_pthread_reset_stats(&queue) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(stats_equal(&output, &expected));
    CHECK_OK(comm_frame_queue_pthread_push(&queue, &frame, 0u));
    CHECK_OK(comm_frame_queue_pthread_pop(&queue, &frame, 0u));
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));

    CHECK_OK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 1u, &stats));
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &before));
    CHECK(comm_frame_queue_pthread_push(&queue, NULL, 0u) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    CHECK(comm_frame_queue_pthread_pop(&queue, NULL, 0u) ==
          COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    /* 测试专用损坏注入，没有并发使用者；错误不得覆盖统计或输出。 */
    queue.core.write_index = queue.core.capacity;
    CHECK(comm_frame_queue_pthread_get_stats(&queue, &output) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(comm_frame_queue_pthread_reset_stats(&queue) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(comm_frame_queue_pthread_push(&queue, &frame, 0u) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(comm_frame_queue_pthread_pop(&queue, &frame, 0u) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(stats_equal(&output, &expected));
    queue.core.write_index = 0u;
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &output));
    CHECK(stats_equal(&before, &output));
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));
    CHECK(queue.stats == NULL);
    CHECK(comm_frame_queue_pthread_get_stats(&queue, &output) ==
          COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    CHECK(stats_equal(&before, &output));
    return 0;
}

/* 通过 Channel 也自动计数；零等待失败与实际等待后超时分开计量。 */
static int test_timeouts_reset_and_channel(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_channel_t channel;
    comm_frame_t storage[1];
    comm_frame_t frame = {0};
    comm_frame_queue_pthread_stats_t stats;
    comm_frame_queue_pthread_stats_t snapshot;
    comm_frame_queue_pthread_stats_t expected = {0};

    frame.sequence = 42u;
    CHECK_OK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 1u, &stats));
    CHECK(comm_frame_channel_pthread_bind(&channel, &queue) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_channel_push(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_channel_push(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_TIMEOUT);
    CHECK(comm_frame_channel_push(&channel, &frame, 20u) == COMM_FRAME_CHANNEL_TIMEOUT);
    CHECK(comm_frame_channel_pop(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    CHECK(frame.sequence == 42u);
    CHECK(comm_frame_channel_pop(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_TIMEOUT);
    CHECK(comm_frame_channel_pop(&channel, &frame, 20u) == COMM_FRAME_CHANNEL_TIMEOUT);
    CHECK(frame.sequence == 42u);
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    expected.frames_pushed = 1u;
    expected.frames_popped = 1u;
    expected.push_full_encounters = 2u;
    expected.pop_empty_encounters = 2u;
    expected.push_wait_count = 1u;
    expected.pop_wait_count = 1u;
    expected.push_timeout_count = 2u;
    expected.pop_timeout_count = 2u;
    expected.peak_size = 1u;
    CHECK(stats_equal(&snapshot, &expected));

    CHECK(comm_frame_channel_push(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    CHECK_OK(comm_frame_queue_pthread_reset_stats(&queue));
    memset(&expected, 0, sizeof(expected));
    expected.peak_size = 1u;
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK(stats_equal(&snapshot, &expected));
    CHECK(comm_frame_channel_close(&channel) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_channel_push(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_CLOSED);
    CHECK(comm_frame_channel_pop(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    CHECK(frame.sequence == 42u);
    CHECK(comm_frame_channel_pop(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_CLOSED);
    expected.frames_popped = 1u;
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK(stats_equal(&snapshot, &expected));
    CHECK_OK(comm_frame_queue_pthread_reset_stats(&queue));
    memset(&expected, 0, sizeof(expected));
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK(stats_equal(&snapshot, &expected));
    CHECK(comm_frame_channel_push(&channel, &frame, 0u) == COMM_FRAME_CHANNEL_CLOSED);
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));
    return 0;
}

typedef struct {
    comm_frame_queue_pthread_t *queue;
    int push;
    comm_frame_queue_pthread_result_t result;
    comm_frame_t frame;
} waiter_t;

static void *waiter_main(void *argument)
{
    waiter_t *waiter = (waiter_t *)argument;
    waiter->result = waiter->push
        ? comm_frame_queue_pthread_push(waiter->queue, &waiter->frame,
                                       COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER)
        : comm_frame_queue_pthread_pop(waiter->queue, &waiter->frame,
                                      COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER);
    return NULL;
}

/* 读取到 wait_count 后，worker 已释放 mutex 进入等待，不依靠固定长延时猜测。 */
static int await_waiter(comm_frame_queue_pthread_t *queue, int push)
{
    size_t attempt;
    struct timespec pause = {0, 1000000L};
    for (attempt = 0u; attempt < 3000u; ++attempt) {
        comm_frame_queue_pthread_stats_t snapshot;
        CHECK_OK(comm_frame_queue_pthread_get_stats(queue, &snapshot));
        if ((push ? snapshot.push_wait_count : snapshot.pop_wait_count) == 1u) {
            return 0;
        }
        CHECK(nanosleep(&pause, NULL) == 0);
    }
    return 1;
}

/* 覆盖等待后成功、关闭唤醒、无条件广播、等待期间切换统计周期。 */
static int test_wait_outcome(int push, int close_waiter, int reset_while_waiting)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t frame = {0};
    comm_frame_queue_pthread_stats_t stats;
    comm_frame_queue_pthread_stats_t snapshot;
    comm_frame_queue_pthread_stats_t expected = {0};
    waiter_t waiter = {0};
    pthread_t thread;
    size_t index;
    int ready;
    struct timespec pause = {0, 1000000L};

    frame.sequence = 7u;
    CHECK_OK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 1u, &stats));
    if (push) {
        CHECK_OK(comm_frame_queue_pthread_push(&queue, &frame, 0u));
    }
    waiter.queue = &queue;
    waiter.push = push;
    waiter.frame.sequence = 8u;
    CHECK(pthread_create(&thread, NULL, waiter_main, &waiter) == 0);
    ready = await_waiter(&queue, push);
    if (ready != 0) {
        (void)comm_frame_queue_pthread_close(&queue);
        (void)pthread_join(thread, NULL);
        CHECK(ready == 0);
    }
    for (index = 0u; index < 3u; ++index) {
        CHECK(pthread_mutex_lock(&queue.mutex) == 0);
        CHECK(pthread_cond_broadcast(push ? &queue.not_full : &queue.not_empty) == 0);
        CHECK(pthread_mutex_unlock(&queue.mutex) == 0);
        CHECK(nanosleep(&pause, NULL) == 0);
    }
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK((push ? snapshot.push_full_encounters : snapshot.pop_empty_encounters) == 1u);
    CHECK((push ? snapshot.push_wait_count : snapshot.pop_wait_count) == 1u);
    if (reset_while_waiting) {
        CHECK_OK(comm_frame_queue_pthread_reset_stats(&queue));
    }
    if (close_waiter) {
        CHECK_OK(comm_frame_queue_pthread_close(&queue));
    } else if (push) {
        CHECK_OK(comm_frame_queue_pthread_pop(&queue, &frame, 0u));
        CHECK(frame.sequence == 7u);
    } else {
        CHECK_OK(comm_frame_queue_pthread_push(&queue, &frame, 0u));
    }
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.result == (close_waiter ? COMM_FRAME_QUEUE_PTHREAD_CLOSED
                                        : COMM_FRAME_QUEUE_PTHREAD_OK));
    if (!close_waiter && !push) {
        CHECK(waiter.frame.sequence == 7u);
    }
    if (!reset_while_waiting) {
        expected.frames_pushed = push ? 1u : 0u;
        expected.push_full_encounters = push ? 1u : 0u;
        expected.pop_empty_encounters = push ? 0u : 1u;
        expected.push_wait_count = push ? 1u : 0u;
        expected.pop_wait_count = push ? 0u : 1u;
    }
    if (!close_waiter) {
        expected.frames_pushed += 1u;
        expected.frames_popped = 1u;
    }
    expected.peak_size = (push || !close_waiter) ? 1u : 0u;
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK(stats_equal(&snapshot, &expected));
    if (push) {
        CHECK_OK(comm_frame_queue_pthread_pop(&queue, &frame, 0u));
        CHECK(frame.sequence == (close_waiter ? 7u : 8u));
    }
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));
    return 0;
}

static int test_saturation(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t frame = {0};
    comm_frame_queue_pthread_stats_t stats;
    comm_frame_queue_pthread_stats_t snapshot;
    comm_frame_queue_pthread_stats_t expected = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, 1u
    };
    size_t index;

    CHECK_OK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 1u, &stats));
    /* 测试专用边界注入，遵守同一把 mutex，生产代码只通过公开接口访问。 */
    CHECK(pthread_mutex_lock(&queue.mutex) == 0);
    stats = expected;
    stats.frames_pushed -= 1u;
    stats.frames_popped -= 1u;
    stats.push_full_encounters -= 1u;
    stats.pop_empty_encounters -= 1u;
    stats.push_wait_count -= 1u;
    stats.pop_wait_count -= 1u;
    stats.push_timeout_count -= 1u;
    stats.pop_timeout_count -= 1u;
    CHECK(pthread_mutex_unlock(&queue.mutex) == 0);
    for (index = 0u; index < 2u; ++index) {
        CHECK_OK(comm_frame_queue_pthread_push(&queue, &frame, 0u));
        CHECK(comm_frame_queue_pthread_push(&queue, &frame, 1u) ==
              COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
        CHECK_OK(comm_frame_queue_pthread_pop(&queue, &frame, 0u));
        CHECK(comm_frame_queue_pthread_pop(&queue, &frame, 1u) ==
              COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
        CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
        CHECK(stats_equal(&snapshot, &expected));
    }
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));
    return 0;
}

typedef struct {
    comm_frame_queue_pthread_t *queue;
    int push;
    uint16_t first_sequence;
    unsigned int sum;
    int failed;
} traffic_t;

static void *traffic_main(void *argument)
{
    traffic_t *traffic = (traffic_t *)argument;
    unsigned int index;
    for (index = 0u; index < 500u; ++index) {
        comm_frame_t frame = {0};
        comm_frame_queue_pthread_result_t result;
        frame.sequence = (uint16_t)(traffic->first_sequence + index);
        result = traffic->push
            ? comm_frame_queue_pthread_push(traffic->queue, &frame, 2000u)
            : comm_frame_queue_pthread_pop(traffic->queue, &frame, 2000u);
        if (result != COMM_FRAME_QUEUE_PTHREAD_OK) {
            traffic->failed = 1;
            break;
        }
        if (!traffic->push) {
            traffic->sum += frame.sequence;
        }
    }
    return NULL;
}

/* 多生产者/消费者同时运行时读取快照，检查计数与深度关系不会撕裂。 */
static int test_concurrent_snapshots(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[4];
    comm_frame_queue_pthread_stats_t stats;
    comm_frame_queue_pthread_stats_t snapshot = {0};
    traffic_t traffic[4] = {{0}};
    pthread_t threads[4];
    size_t index;
    size_t attempt;
    int invalid_snapshot = 0;
    struct timespec pause = {0, 1000000L};

    CHECK_OK(comm_frame_queue_pthread_init_with_stats(&queue, storage, 4u, &stats));
    for (index = 0u; index < 4u; ++index) {
        traffic[index].queue = &queue;
        traffic[index].push = index < 2u;
        traffic[index].first_sequence = (uint16_t)(1u + index * 500u);
        CHECK(pthread_create(&threads[index], NULL, traffic_main, &traffic[index]) == 0);
    }
    for (attempt = 0u; attempt < 5000u; ++attempt) {
        CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
        if ((snapshot.frames_popped > snapshot.frames_pushed) ||
            (snapshot.frames_pushed - snapshot.frames_popped > 4u) ||
            (snapshot.peak_size > 4u) ||
            (snapshot.frames_pushed - snapshot.frames_popped > snapshot.peak_size)) {
            invalid_snapshot = 1;
            break;
        }
        if (snapshot.frames_popped == 1000u) {
            break;
        }
        CHECK(nanosleep(&pause, NULL) == 0);
    }
    CHECK_OK(comm_frame_queue_pthread_close(&queue));
    for (index = 0u; index < 4u; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(invalid_snapshot == 0);
    CHECK_OK(comm_frame_queue_pthread_get_stats(&queue, &snapshot));
    CHECK(snapshot.frames_pushed == 1000u && snapshot.frames_popped == 1000u);
    CHECK(snapshot.push_timeout_count == 0u && snapshot.pop_timeout_count == 0u);
    CHECK(traffic[2].sum + traffic[3].sum == 500500u);
    for (index = 0u; index < 4u; ++index) {
        CHECK(traffic[index].failed == 0);
    }
    CHECK_OK(comm_frame_queue_pthread_destroy(&queue));
    return 0;
}

int main(void)
{
    if (test_validation_and_optional_stats() != 0 ||
        test_timeouts_reset_and_channel() != 0 ||
        test_wait_outcome(1, 0, 0) != 0 ||
        test_wait_outcome(0, 0, 0) != 0 ||
        test_wait_outcome(1, 1, 0) != 0 ||
        test_wait_outcome(0, 1, 0) != 0 ||
        test_wait_outcome(1, 0, 1) != 0 ||
        test_wait_outcome(0, 0, 1) != 0 ||
        test_wait_outcome(0, 1, 1) != 0 ||
        test_saturation() != 0 ||
        test_concurrent_snapshots() != 0) {
        return 1;
    }
    puts("test_comm_frame_queue_pthread_stats: all tests passed");
    return 0;
}
