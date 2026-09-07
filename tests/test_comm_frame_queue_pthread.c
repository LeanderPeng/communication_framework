#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "comm_frame_queue_pthread.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_CHECK(condition)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr,                                                  \
                    "test failed: %s (%s:%d)\n",                            \
                    #condition,                                              \
                    __FILE__,                                                \
                    __LINE__);                                               \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* 创建一帧固定格式的测试数据。 */
static void fill_frame(comm_frame_t *frame,
                       uint16_t sequence,
                       uint8_t payload_value)
{
    frame->version = COMM_FRAME_VERSION;
    frame->type = COMM_FRAME_TYPE_REPORT;
    frame->sequence = sequence;
    frame->payload_length = 1u;
    memset(frame->payload, payload_value, sizeof(frame->payload));
}

/* 将单调时钟时间转换为毫秒，供超时测试比较经过时间。 */
static uint64_t time_in_milliseconds(const struct timespec *time_value)
{
    return ((uint64_t)time_value->tv_sec * 1000u) +
           ((uint64_t)time_value->tv_nsec / 1000000u);
}

/* 等待中的生产线程及其启动握手状态。 */
typedef struct {
    comm_frame_queue_pthread_t *queue;
    comm_frame_t frame;
    comm_frame_queue_pthread_result_t result;
    pthread_mutex_t start_mutex;
    pthread_cond_t start_condition;
    int started;
} waiting_producer_context_t;

/* 等待中的消费线程及其启动握手状态。 */
typedef struct {
    comm_frame_queue_pthread_t *queue;
    comm_frame_t frame;
    comm_frame_queue_pthread_result_t result;
    pthread_mutex_t start_mutex;
    pthread_cond_t start_condition;
    int started;
} waiting_consumer_context_t;

/*
 * 先通知主线程“生产线程已经运行”，再尝试向满队列入队。
 * start_mutex 只用于测试握手，不参与被测队列内部的同步。
 */
static void *waiting_producer_main(void *argument)
{
    waiting_producer_context_t *context =
        (waiting_producer_context_t *)argument;

    if (pthread_mutex_lock(&context->start_mutex) != 0) {
        context->result = COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
        return NULL;
    }

    context->started = 1;
    (void)pthread_cond_signal(&context->start_condition);
    (void)pthread_mutex_unlock(&context->start_mutex);

    context->result = comm_frame_queue_pthread_push(context->queue,
                                                    &context->frame,
                                                    2000u);
    return NULL;
}

/*
 * 先通知主线程“消费线程已经运行”，再尝试从空队列等待一帧。
 * 2 秒超时只用于防止测试错误时永久挂起，正常情况下会被 push 或 close
 * 提前唤醒。
 */
static void *waiting_consumer_main(void *argument)
{
    waiting_consumer_context_t *context =
        (waiting_consumer_context_t *)argument;

    if (pthread_mutex_lock(&context->start_mutex) != 0) {
        context->result = COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR;
        return NULL;
    }

    context->started = 1;
    (void)pthread_cond_signal(&context->start_condition);
    (void)pthread_mutex_unlock(&context->start_mutex);

    context->result = comm_frame_queue_pthread_pop(context->queue,
                                                   &context->frame,
                                                   2000u);
    return NULL;
}

/* 验证初始化参数错误不会修改调用方传入的对象。 */
static int test_init_validation(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_queue_pthread_t original_queue;
    comm_frame_t storage[2];

    memset(&queue, 0xA5, sizeof(queue));
    memcpy(&original_queue, &queue, sizeof(queue));

    TEST_CHECK(comm_frame_queue_pthread_init(NULL, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pthread_init(&queue, NULL, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(memcmp(&queue, &original_queue, sizeof(queue)) == 0);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_CAPACITY);
    TEST_CHECK(memcmp(&queue, &original_queue, sizeof(queue)) == 0);

    return 0;
}

/* 验证成功初始化后，核心队列和生命周期标记处于正确状态。 */
static int test_init_and_destroy(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[3];

    memset(storage, 0x5A, sizeof(storage));

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 3u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.core.storage == storage);
    TEST_CHECK(queue.core.capacity == 3u);
    TEST_CHECK(queue.core.read_index == 0u);
    TEST_CHECK(queue.core.write_index == 0u);
    TEST_CHECK(queue.core.used == 0u);
    TEST_CHECK(queue.initialized == 1);
    TEST_CHECK(queue.closed == 0);
    TEST_CHECK(storage[0].payload[0] == 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.core.storage == NULL);
    TEST_CHECK(queue.core.capacity == 0u);
    TEST_CHECK(queue.initialized == 0);
    TEST_CHECK(queue.closed == 0);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);

    return 0;
}

/* 验证 destroy 后，同一个包装对象可以使用原存储区重新初始化。 */
static int test_reinitialize_after_destroy(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[2];

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.core.storage == storage);
    TEST_CHECK(queue.core.capacity == 2u);
    TEST_CHECK(queue.initialized == 1);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证空指针和明确的未初始化状态会被 destroy 拒绝。 */
static int test_destroy_validation(void)
{
    comm_frame_queue_pthread_t queue;

    memset(&queue, 0, sizeof(queue));

    TEST_CHECK(comm_frame_queue_pthread_destroy(NULL) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);

    return 0;
}

/* 验证 close 可重复调用，并且不会丢弃关闭前已入队的帧。 */
static int test_close_is_idempotent_and_preserves_frames(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[2];
    comm_frame_t input;

    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    /* 并发接口尚未实现，测试内部直接预置一帧以验证 close 不清空 core。 */
    TEST_CHECK(comm_frame_queue_push(&queue.core, &input) ==
               COMM_FRAME_QUEUE_OK);

    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.closed == 1);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(queue.core.read_index == 0u);
    TEST_CHECK(queue.core.write_index == 1u);
    TEST_CHECK(storage[0].sequence == input.sequence);

    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.closed == 1);
    TEST_CHECK(queue.core.used == 1u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证 close 会拒绝空指针和明确的未初始化对象。 */
static int test_close_validation(void)
{
    comm_frame_queue_pthread_t queue;

    memset(&queue, 0, sizeof(queue));

    TEST_CHECK(comm_frame_queue_pthread_close(NULL) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);

    return 0;
}

/* 验证非阻塞 push 成功复制完整帧并更新核心队列状态。 */
static int test_push_without_waiting(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[2];
    comm_frame_t input;

    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &input, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(queue.core.read_index == 0u);
    TEST_CHECK(queue.core.write_index == 1u);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(storage[0].sequence == input.sequence);
    TEST_CHECK(storage[0].payload[0] == input.payload[0]);

    input.sequence = 0xFFFFu;
    input.payload[0] = 0u;
    TEST_CHECK(storage[0].sequence == 0x1234u);
    TEST_CHECK(storage[0].payload[0] == 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证满队列上的零等待和有限等待都会返回 TIMEOUT。 */
static int test_push_timeout(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t first_frame;
    comm_frame_t second_frame;
    struct timespec start_time;
    struct timespec end_time;
    uint64_t elapsed_ms;

    fill_frame(&first_frame, 1u, 0x11u);
    fill_frame(&second_frame, 2u, 0x22u);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &first_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &second_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(storage[0].sequence == first_frame.sequence);

    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &second_frame, 30u) ==
               COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &end_time) == 0);

    elapsed_ms = time_in_milliseconds(&end_time) -
                 time_in_milliseconds(&start_time);
    TEST_CHECK(elapsed_ms >= 20u);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(storage[0].sequence == first_frame.sequence);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证关闭、空指针、未初始化和损坏状态下不会入队。 */
static int test_push_rejects_invalid_state(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_queue_pthread_t uninitialized_queue;
    comm_frame_t storage[2];
    comm_frame_t input;

    memset(&uninitialized_queue, 0, sizeof(uninitialized_queue));
    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_push(NULL, &input, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pthread_push(&uninitialized_queue,
                                             &input,
                                             0u) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, NULL, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);

    queue.core.write_index = 1u;
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &input, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    TEST_CHECK(queue.core.used == 0u);
    TEST_CHECK(comm_frame_queue_reset(&queue.core) == COMM_FRAME_QUEUE_OK);

    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &input, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_CLOSED);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证 close 的广播会立即唤醒正在等待空槽位的生产线程。 */
static int test_close_wakes_waiting_producer(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t first_frame;
    waiting_producer_context_t context;
    pthread_t producer_thread;
    struct timespec short_delay;
    struct timespec close_time;
    struct timespec joined_time;
    uint64_t close_to_join_ms;

    fill_frame(&first_frame, 1u, 0x11u);
    memset(&context, 0, sizeof(context));
    fill_frame(&context.frame, 2u, 0x22u);
    context.queue = &queue;

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &first_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_mutex_init(&context.start_mutex, NULL) == 0);
    TEST_CHECK(pthread_cond_init(&context.start_condition, NULL) == 0);
    TEST_CHECK(pthread_create(&producer_thread,
                              NULL,
                              waiting_producer_main,
                              &context) == 0);

    TEST_CHECK(pthread_mutex_lock(&context.start_mutex) == 0);
    while (context.started == 0) {
        TEST_CHECK(pthread_cond_wait(&context.start_condition,
                                     &context.start_mutex) == 0);
    }
    TEST_CHECK(pthread_mutex_unlock(&context.start_mutex) == 0);

    /* 给生产线程时间进入队列内部的 not_full 等待。 */
    short_delay.tv_sec = 0;
    short_delay.tv_nsec = 50000000L;
    TEST_CHECK(nanosleep(&short_delay, NULL) == 0);

    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &close_time) == 0);
    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_join(producer_thread, NULL) == 0);
    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &joined_time) == 0);

    close_to_join_ms = time_in_milliseconds(&joined_time) -
                       time_in_milliseconds(&close_time);
    TEST_CHECK(context.result == COMM_FRAME_QUEUE_PTHREAD_CLOSED);
    TEST_CHECK(close_to_join_ms < 1000u);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(storage[0].sequence == first_frame.sequence);

    TEST_CHECK(pthread_cond_destroy(&context.start_condition) == 0);
    TEST_CHECK(pthread_mutex_destroy(&context.start_mutex) == 0);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证非阻塞 pop 按 FIFO 顺序返回帧，并更新核心队列状态。 */
static int test_pop_without_waiting(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[2];
    comm_frame_t first_frame;
    comm_frame_t second_frame;
    comm_frame_t output;

    fill_frame(&first_frame, 1u, 0x11u);
    fill_frame(&second_frame, 2u, 0x22u);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &first_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &second_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(output.sequence == first_frame.sequence);
    TEST_CHECK(output.payload[0] == first_frame.payload[0]);
    TEST_CHECK(queue.core.read_index == 1u);
    TEST_CHECK(queue.core.write_index == 0u);
    TEST_CHECK(queue.core.used == 1u);

    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(output.sequence == second_frame.sequence);
    TEST_CHECK(output.payload[0] == second_frame.payload[0]);
    TEST_CHECK(queue.core.read_index == 0u);
    TEST_CHECK(queue.core.write_index == 0u);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证空队列上的零等待和有限等待超时都不会修改输出帧。 */
static int test_pop_timeout(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t output;
    comm_frame_t expected_output;
    struct timespec start_time;
    struct timespec end_time;
    uint64_t elapsed_ms;

    fill_frame(&output, 0x1234u, 0x5Au);
    expected_output = output;

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
    TEST_CHECK(output.sequence == expected_output.sequence);
    TEST_CHECK(output.payload[0] == expected_output.payload[0]);

    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 30u) ==
               COMM_FRAME_QUEUE_PTHREAD_TIMEOUT);
    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &end_time) == 0);

    elapsed_ms = time_in_milliseconds(&end_time) -
                 time_in_milliseconds(&start_time);
    TEST_CHECK(elapsed_ms >= 20u);
    TEST_CHECK(output.sequence == expected_output.sequence);
    TEST_CHECK(output.payload[0] == expected_output.payload[0]);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证关闭后先排空已有帧，随后才返回 CLOSED。 */
static int test_pop_drains_closed_queue(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[2];
    comm_frame_t first_frame;
    comm_frame_t second_frame;
    comm_frame_t output;

    fill_frame(&first_frame, 1u, 0x11u);
    fill_frame(&second_frame, 2u, 0x22u);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &first_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &second_frame, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(output.sequence == first_frame.sequence);
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(output.sequence == second_frame.sequence);

    output.sequence = 0xA5A5u;
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_CLOSED);
    TEST_CHECK(output.sequence == 0xA5A5u);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证 push 会唤醒正在空队列上等待的消费者。 */
static int test_push_wakes_waiting_consumer(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    comm_frame_t input;
    waiting_consumer_context_t context;
    pthread_t consumer_thread;
    struct timespec short_delay;

    fill_frame(&input, 0x1234u, 0x5Au);
    memset(&context, 0, sizeof(context));
    context.queue = &queue;

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_mutex_init(&context.start_mutex, NULL) == 0);
    TEST_CHECK(pthread_cond_init(&context.start_condition, NULL) == 0);
    TEST_CHECK(pthread_create(&consumer_thread,
                              NULL,
                              waiting_consumer_main,
                              &context) == 0);

    TEST_CHECK(pthread_mutex_lock(&context.start_mutex) == 0);
    while (context.started == 0) {
        TEST_CHECK(pthread_cond_wait(&context.start_condition,
                                     &context.start_mutex) == 0);
    }
    TEST_CHECK(pthread_mutex_unlock(&context.start_mutex) == 0);

    short_delay.tv_sec = 0;
    short_delay.tv_nsec = 50000000L;
    TEST_CHECK(nanosleep(&short_delay, NULL) == 0);

    TEST_CHECK(comm_frame_queue_pthread_push(&queue, &input, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_join(consumer_thread, NULL) == 0);
    TEST_CHECK(context.result == COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(context.frame.sequence == input.sequence);
    TEST_CHECK(context.frame.payload[0] == input.payload[0]);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(pthread_cond_destroy(&context.start_condition) == 0);
    TEST_CHECK(pthread_mutex_destroy(&context.start_mutex) == 0);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证 close 会唤醒正在空队列上等待的消费者。 */
static int test_close_wakes_waiting_consumer(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_t storage[1];
    waiting_consumer_context_t context;
    pthread_t consumer_thread;
    struct timespec short_delay;
    struct timespec close_time;
    struct timespec joined_time;
    uint64_t close_to_join_ms;

    memset(&context, 0, sizeof(context));
    context.queue = &queue;

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_mutex_init(&context.start_mutex, NULL) == 0);
    TEST_CHECK(pthread_cond_init(&context.start_condition, NULL) == 0);
    TEST_CHECK(pthread_create(&consumer_thread,
                              NULL,
                              waiting_consumer_main,
                              &context) == 0);

    TEST_CHECK(pthread_mutex_lock(&context.start_mutex) == 0);
    while (context.started == 0) {
        TEST_CHECK(pthread_cond_wait(&context.start_condition,
                                     &context.start_mutex) == 0);
    }
    TEST_CHECK(pthread_mutex_unlock(&context.start_mutex) == 0);

    short_delay.tv_sec = 0;
    short_delay.tv_nsec = 50000000L;
    TEST_CHECK(nanosleep(&short_delay, NULL) == 0);

    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &close_time) == 0);
    TEST_CHECK(comm_frame_queue_pthread_close(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(pthread_join(consumer_thread, NULL) == 0);
    TEST_CHECK(clock_gettime(CLOCK_MONOTONIC, &joined_time) == 0);

    close_to_join_ms = time_in_milliseconds(&joined_time) -
                       time_in_milliseconds(&close_time);
    TEST_CHECK(context.result == COMM_FRAME_QUEUE_PTHREAD_CLOSED);
    TEST_CHECK(close_to_join_ms < 1000u);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(pthread_cond_destroy(&context.start_condition) == 0);
    TEST_CHECK(pthread_mutex_destroy(&context.start_mutex) == 0);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证空指针、未初始化和损坏状态下不会修改输出帧。 */
static int test_pop_rejects_invalid_state(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_queue_pthread_t uninitialized_queue;
    comm_frame_t storage[1];
    comm_frame_t output;

    memset(&uninitialized_queue, 0, sizeof(uninitialized_queue));
    fill_frame(&output, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_pop(NULL, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pthread_pop(&uninitialized_queue,
                                            &output,
                                            0u) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, NULL, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT);

    queue.core.write_index = 1u;
    TEST_CHECK(comm_frame_queue_pthread_pop(&queue, &output, 0u) ==
               COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE);
    TEST_CHECK(output.sequence == 0x1234u);
    TEST_CHECK(output.payload[0] == 0x5Au);
    TEST_CHECK(comm_frame_queue_reset(&queue.core) == COMM_FRAME_QUEUE_OK);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

int main(void)
{
    if (test_init_validation() != 0) {
        return 1;
    }
    if (test_init_and_destroy() != 0) {
        return 1;
    }
    if (test_reinitialize_after_destroy() != 0) {
        return 1;
    }
    if (test_destroy_validation() != 0) {
        return 1;
    }
    if (test_close_is_idempotent_and_preserves_frames() != 0) {
        return 1;
    }
    if (test_close_validation() != 0) {
        return 1;
    }
    if (test_push_without_waiting() != 0) {
        return 1;
    }
    if (test_push_timeout() != 0) {
        return 1;
    }
    if (test_push_rejects_invalid_state() != 0) {
        return 1;
    }
    if (test_close_wakes_waiting_producer() != 0) {
        return 1;
    }
    if (test_pop_without_waiting() != 0) {
        return 1;
    }
    if (test_pop_timeout() != 0) {
        return 1;
    }
    if (test_pop_drains_closed_queue() != 0) {
        return 1;
    }
    if (test_push_wakes_waiting_consumer() != 0) {
        return 1;
    }
    if (test_close_wakes_waiting_consumer() != 0) {
        return 1;
    }
    if (test_pop_rejects_invalid_state() != 0) {
        return 1;
    }

    puts("test_comm_frame_queue_pthread: all tests passed");
    return 0;
}
