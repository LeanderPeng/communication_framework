#include "comm_frame_queue.h"

#include <stdio.h>
#include <string.h>

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

/* 验证初始化参数以及失败时不修改原状态的约定。 */
static int test_init_validation(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[4];

    queue.storage = storage;
    queue.capacity = 3u;
    queue.read_index = 1u;
    queue.write_index = 2u;
    queue.used = 1u;

    TEST_CHECK(comm_frame_queue_init(NULL, storage, 4u) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_init(&queue, NULL, 4u) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(queue.storage == storage);
    TEST_CHECK(queue.capacity == 3u);
    TEST_CHECK(queue.read_index == 1u);
    TEST_CHECK(queue.write_index == 2u);
    TEST_CHECK(queue.used == 1u);

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 0u) ==
               COMM_FRAME_QUEUE_INVALID_CAPACITY);
    TEST_CHECK(queue.capacity == 3u);
    TEST_CHECK(queue.used == 1u);

    return 0;
}

/* 验证初始化后的空状态以及全部可用槽位。 */
static int test_init_empty_state(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[4];

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 4u) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.storage == storage);
    TEST_CHECK(queue.capacity == 4u);
    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 0u);
    TEST_CHECK(comm_frame_queue_size(&queue) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 4u);

    return 0;
}

/* 验证 reset 能保留存储配置并修复损坏的运行状态。 */
static int test_reset(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[4];

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 4u) ==
               COMM_FRAME_QUEUE_OK);
    queue.read_index = 99u;
    queue.write_index = 88u;
    queue.used = 77u;

    TEST_CHECK(comm_frame_queue_reset(&queue) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.storage == storage);
    TEST_CHECK(queue.capacity == 4u);
    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 0u);

    TEST_CHECK(comm_frame_queue_reset(NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    queue.storage = NULL;
    TEST_CHECK(comm_frame_queue_reset(&queue) ==
               COMM_FRAME_QUEUE_INVALID_STATE);

    return 0;
}

/* 验证空、满、绕回以及损坏状态下的容量查询。 */
static int test_size_queries(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[4];

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 4u) ==
               COMM_FRAME_QUEUE_OK);

    queue.read_index = 2u;
    queue.write_index = 2u;
    queue.used = 4u;
    TEST_CHECK(comm_frame_queue_size(&queue) == 4u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 0u);

    queue.read_index = 3u;
    queue.write_index = 1u;
    queue.used = 2u;
    TEST_CHECK(comm_frame_queue_size(&queue) == 2u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 2u);

    queue.read_index = 1u;
    queue.write_index = 1u;
    queue.used = 2u;
    TEST_CHECK(comm_frame_queue_size(&queue) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 0u);

    queue.read_index = 3u;
    queue.write_index = 2u;
    queue.used = 2u;
    TEST_CHECK(comm_frame_queue_size(&queue) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 0u);

    queue.read_index = 4u;
    TEST_CHECK(comm_frame_queue_size(&queue) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 0u);

    TEST_CHECK(comm_frame_queue_size(NULL) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(NULL) == 0u);

    return 0;
}

/* 使用不同字节填充一帧，便于检查结构体是否被完整复制。 */
static void fill_frame(comm_frame_t *frame,
                       uint16_t sequence,
                       uint8_t payload_value)
{
    frame->version = COMM_FRAME_VERSION;
    frame->type = COMM_FRAME_TYPE_REPORT;
    frame->sequence = sequence;
    frame->payload_length = 3u;
    memset(frame->payload, payload_value, sizeof(frame->payload));
}

/* 逐字段比较帧，避免依赖结构体填充字节的具体内容。 */
static int frames_are_equal(const comm_frame_t *left,
                            const comm_frame_t *right)
{
    return (left->version == right->version) &&
           (left->type == right->type) &&
           (left->sequence == right->sequence) &&
           (left->payload_length == right->payload_length) &&
           (memcmp(left->payload,
                   right->payload,
                   sizeof(left->payload)) == 0);
}

/* 验证普通入队会复制整帧，而不是保存调用方的帧地址。 */
static int test_push_copies_frame(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[3];
    comm_frame_t input;
    comm_frame_t expected;

    fill_frame(&input, 0x1234u, 0x5Au);
    expected = input;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 3u) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push(&queue, &input) ==
               COMM_FRAME_QUEUE_OK);

    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 1u);
    TEST_CHECK(queue.used == 1u);
    TEST_CHECK(comm_frame_queue_size(&queue) == 1u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 2u);
    TEST_CHECK(storage[0].version == expected.version);
    TEST_CHECK(storage[0].type == expected.type);
    TEST_CHECK(storage[0].sequence == expected.sequence);
    TEST_CHECK(storage[0].payload_length == expected.payload_length);
    TEST_CHECK(memcmp(storage[0].payload,
                      expected.payload,
                      sizeof(expected.payload)) == 0);

    input.sequence = 0xFFFFu;
    input.payload[0] = 0u;
    TEST_CHECK(storage[0].sequence == expected.sequence);
    TEST_CHECK(storage[0].payload[0] == expected.payload[0]);

    return 0;
}

/* 验证全部槽位均可使用，并能形成 read == write 的满状态。 */
static int test_push_fills_capacity(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[3];
    comm_frame_t frames[3];
    size_t index;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 3u) ==
               COMM_FRAME_QUEUE_OK);

    for (index = 0u; index < 3u; ++index) {
        fill_frame(&frames[index],
                   (uint16_t)(index + 1u),
                   (uint8_t)(0x10u + index));
        TEST_CHECK(comm_frame_queue_push(&queue, &frames[index]) ==
                   COMM_FRAME_QUEUE_OK);
    }

    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 3u);
    TEST_CHECK(comm_frame_queue_size(&queue) == 3u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 0u);
    TEST_CHECK(storage[0].sequence == 1u);
    TEST_CHECK(storage[1].sequence == 2u);
    TEST_CHECK(storage[2].sequence == 3u);

    return 0;
}

/* 验证写下标到达数组末尾后会从索引 0 继续入队。 */
static int test_push_wraps(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[4];
    comm_frame_t first_frame;
    comm_frame_t second_frame;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 4u) ==
               COMM_FRAME_QUEUE_OK);
    queue.read_index = 2u;
    queue.write_index = 3u;
    queue.used = 1u;

    fill_frame(&first_frame, 0x0102u, 0x11u);
    fill_frame(&second_frame, 0x0304u, 0x22u);

    TEST_CHECK(comm_frame_queue_push(&queue, &first_frame) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 2u);
    TEST_CHECK(storage[3].sequence == first_frame.sequence);

    TEST_CHECK(comm_frame_queue_push(&queue, &second_frame) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.read_index == 2u);
    TEST_CHECK(queue.write_index == 1u);
    TEST_CHECK(queue.used == 3u);
    TEST_CHECK(storage[0].sequence == second_frame.sequence);

    return 0;
}

/* 验证参数、状态或满队列错误不会产生部分入队。 */
static int test_push_rejects_invalid_request(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[2];
    comm_frame_t original_storage[2];
    comm_frame_t input;

    memset(storage, 0xA5, sizeof(storage));
    fill_frame(&input, 0x1234u, 0x5Au);
    TEST_CHECK(comm_frame_queue_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_OK);

    TEST_CHECK(comm_frame_queue_push(NULL, &input) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_push(&queue, NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 0u);

    queue.read_index = 1u;
    queue.write_index = 1u;
    queue.used = 2u;
    memcpy(original_storage, storage, sizeof(storage));

    TEST_CHECK(comm_frame_queue_push(&queue, &input) ==
               COMM_FRAME_QUEUE_FULL);
    TEST_CHECK(queue.read_index == 1u);
    TEST_CHECK(queue.write_index == 1u);
    TEST_CHECK(queue.used == 2u);
    TEST_CHECK(memcmp(storage, original_storage, sizeof(storage)) == 0);

    queue.write_index = 0u;
    TEST_CHECK(comm_frame_queue_push(&queue, &input) ==
               COMM_FRAME_QUEUE_INVALID_STATE);
    TEST_CHECK(memcmp(storage, original_storage, sizeof(storage)) == 0);

    return 0;
}

/* 验证出队按先进先出顺序返回完整的独立帧副本。 */
static int test_pop_preserves_fifo_order(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[3];
    comm_frame_t input[3];
    comm_frame_t output;
    size_t index;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 3u) ==
               COMM_FRAME_QUEUE_OK);

    for (index = 0u; index < 3u; ++index) {
        fill_frame(&input[index],
                   (uint16_t)(0x1000u + index),
                   (uint8_t)(0x20u + index));
        TEST_CHECK(comm_frame_queue_push(&queue, &input[index]) ==
                   COMM_FRAME_QUEUE_OK);
    }

    for (index = 0u; index < 3u; ++index) {
        TEST_CHECK(comm_frame_queue_pop(&queue, &output) ==
                   COMM_FRAME_QUEUE_OK);
        TEST_CHECK(frames_are_equal(&output, &input[index]));
        TEST_CHECK(queue.used == (2u - index));
    }

    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 0u);
    TEST_CHECK(comm_frame_queue_size(&queue) == 0u);
    TEST_CHECK(comm_frame_queue_free_space(&queue) == 3u);

    output.sequence = 0xFFFFu;
    TEST_CHECK(storage[2].sequence == input[2].sequence);

    return 0;
}

/* 验证出入队交错并绕回数组后，逻辑顺序仍保持不变。 */
static int test_push_pop_wrap_cycle(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[3];
    comm_frame_t input[5];
    comm_frame_t output;
    size_t index;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 3u) ==
               COMM_FRAME_QUEUE_OK);

    for (index = 0u; index < 5u; ++index) {
        fill_frame(&input[index],
                   (uint16_t)(index + 1u),
                   (uint8_t)(0x30u + index));
    }

    for (index = 0u; index < 3u; ++index) {
        TEST_CHECK(comm_frame_queue_push(&queue, &input[index]) ==
                   COMM_FRAME_QUEUE_OK);
    }
    for (index = 0u; index < 2u; ++index) {
        TEST_CHECK(comm_frame_queue_pop(&queue, &output) ==
                   COMM_FRAME_QUEUE_OK);
        TEST_CHECK(frames_are_equal(&output, &input[index]));
    }

    TEST_CHECK(comm_frame_queue_push(&queue, &input[3]) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push(&queue, &input[4]) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.read_index == 2u);
    TEST_CHECK(queue.write_index == 2u);
    TEST_CHECK(queue.used == 3u);

    for (index = 2u; index < 5u; ++index) {
        TEST_CHECK(comm_frame_queue_pop(&queue, &output) ==
                   COMM_FRAME_QUEUE_OK);
        TEST_CHECK(frames_are_equal(&output, &input[index]));
    }

    TEST_CHECK(queue.read_index == 2u);
    TEST_CHECK(queue.write_index == 2u);
    TEST_CHECK(queue.used == 0u);

    return 0;
}

/* 验证参数、状态或空队列错误不会修改输出帧或队列状态。 */
static int test_pop_rejects_invalid_request(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[2];
    comm_frame_t output;
    comm_frame_t expected_output;

    TEST_CHECK(comm_frame_queue_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_OK);
    fill_frame(&output, 0x1234u, 0x5Au);
    expected_output = output;

    TEST_CHECK(comm_frame_queue_pop(NULL, &output) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pop(&queue, NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pop(&queue, &output) ==
               COMM_FRAME_QUEUE_EMPTY);
    TEST_CHECK(frames_are_equal(&output, &expected_output));
    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 0u);
    TEST_CHECK(queue.used == 0u);

    queue.write_index = 1u;
    TEST_CHECK(comm_frame_queue_pop(&queue, &output) ==
               COMM_FRAME_QUEUE_INVALID_STATE);
    TEST_CHECK(frames_are_equal(&output, &expected_output));
    TEST_CHECK(queue.read_index == 0u);
    TEST_CHECK(queue.write_index == 1u);
    TEST_CHECK(queue.used == 0u);

    return 0;
}

static int stats_are_equal(const comm_frame_queue_stats_t *left,
                            const comm_frame_queue_stats_t *right)
{
    return (left->frames_pushed == right->frames_pushed) &&
           (left->frames_popped == right->frames_popped) &&
           (left->push_full_count == right->push_full_count) &&
           (left->peak_size == right->peak_size);
}

/* 满队列拒绝不等于丢帧；消费后可以再次投递，并且仍然保持复制和 FIFO。 */
static int test_stats_fifo_and_full_retries(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[2];
    comm_frame_t first;
    comm_frame_t second;
    comm_frame_t third;
    comm_frame_t output;
    comm_frame_t expected_output;
    comm_frame_queue_stats_t stats;
    comm_frame_queue_stats_t expected_stats;

    fill_frame(&first, 1u, 0x11u);
    fill_frame(&second, 2u, 0x22u);
    fill_frame(&third, 3u, 0x33u);
    TEST_CHECK(comm_frame_queue_init(&queue, storage, 2u) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(stats.frames_pushed == 0u && stats.frames_popped == 0u);
    TEST_CHECK(stats.push_full_count == 0u && stats.peak_size == 0u);

    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &first, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(stats.frames_pushed == 1u && stats.peak_size == 1u);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &second, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &third, &stats) ==
               COMM_FRAME_QUEUE_FULL);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &third, &stats) ==
               COMM_FRAME_QUEUE_FULL);
    TEST_CHECK(stats.frames_pushed == 2u && stats.frames_popped == 0u);
    TEST_CHECK(stats.push_full_count == 2u && stats.peak_size == 2u);
    TEST_CHECK(queue.used == 2u);

    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(frames_are_equal(&output, &first));
    TEST_CHECK(stats.frames_popped == 1u && stats.peak_size == 2u);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &third, &stats) ==
               COMM_FRAME_QUEUE_OK);
    expected_output = third;
    fill_frame(&third, 99u, 0x99u);
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(frames_are_equal(&output, &second));
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(frames_are_equal(&output, &expected_output));
    TEST_CHECK(stats.frames_pushed == 3u && stats.frames_popped == 3u);
    TEST_CHECK(stats.push_full_count == 2u && stats.peak_size == 2u);

    expected_stats = stats;
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_EMPTY);
    TEST_CHECK(frames_are_equal(&output, &expected_output));
    TEST_CHECK(stats_are_equal(&stats, &expected_stats));
    TEST_CHECK(queue.used == 0u);
    return 0;
}

/* 在非空队列开始统计时，以当前深度作为峰值；清统计和清队列是独立操作。 */
static int test_stats_reset_preserves_queued_frames(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[2];
    comm_frame_t input;
    comm_frame_t output;
    comm_frame_queue_stats_t stats;
    comm_frame_queue_stats_t expected;

    fill_frame(&input, 7u, 0x77u);
    TEST_CHECK(comm_frame_queue_init(&queue, storage, 2u) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push(&queue, &input) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push(&queue, &input) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(stats.frames_pushed == 0u && stats.frames_popped == 0u);
    TEST_CHECK(stats.push_full_count == 0u && stats.peak_size == 2u);
    TEST_CHECK(queue.used == 2u && queue.read_index == 0u && queue.write_index == 0u);
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_OK);
    TEST_CHECK(frames_are_equal(&output, &input));
    TEST_CHECK(stats.frames_popped == 1u && stats.peak_size == 2u);

    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(stats.frames_pushed == 0u && stats.frames_popped == 0u);
    TEST_CHECK(stats.push_full_count == 0u && stats.peak_size == 1u);
    TEST_CHECK(queue.used == 1u && queue.read_index == 1u && queue.write_index == 0u);
    TEST_CHECK(frames_are_equal(&storage[1], &input));
    expected = stats;
    TEST_CHECK(comm_frame_queue_reset(&queue) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(queue.used == 0u);
    TEST_CHECK(stats_are_equal(&stats, &expected));
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(stats.peak_size == 0u);
    return 0;
}

/* 参数和状态错误不得留下统计、输出帧或队列的半更新。 */
static int test_stats_validation_is_transactional(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[2];
    comm_frame_t input;
    comm_frame_t output;
    comm_frame_t expected_output;
    comm_frame_queue_stats_t stats = {11u, 12u, 13u, 0u};
    comm_frame_queue_stats_t expected = stats;

    fill_frame(&input, 1u, 0x11u);
    fill_frame(&output, 9u, 0x99u);
    expected_output = output;
    TEST_CHECK(comm_frame_queue_init(&queue, storage, 2u) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_push(&queue, &input) == COMM_FRAME_QUEUE_OK);

    TEST_CHECK(comm_frame_queue_stats_reset(NULL, &stats) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_push_with_stats(NULL, &input, &stats) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pop_with_stats(NULL, &output, &stats) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, NULL, &stats) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, NULL, &stats) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &input, NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, NULL) ==
               COMM_FRAME_QUEUE_NULL_ARGUMENT);
    TEST_CHECK(stats_are_equal(&stats, &expected));
    TEST_CHECK(queue.used == 1u && queue.read_index == 0u && queue.write_index == 1u);

    queue.write_index = queue.capacity;
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) ==
               COMM_FRAME_QUEUE_INVALID_STATE);
    TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &input, &stats) ==
               COMM_FRAME_QUEUE_INVALID_STATE);
    TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
               COMM_FRAME_QUEUE_INVALID_STATE);
    TEST_CHECK(stats_are_equal(&stats, &expected));
    TEST_CHECK(frames_are_equal(&output, &expected_output));
    TEST_CHECK(frames_are_equal(&storage[0], &input));
    TEST_CHECK(queue.used == 1u && queue.read_index == 0u);
    TEST_CHECK(queue.write_index == queue.capacity);
    return 0;
}

/* 三项累计值分别在边界饱和，同时队列仍可正常入队、拒绝、出队。 */
static int test_stats_counters_saturate(void)
{
    comm_frame_queue_t queue;
    comm_frame_t storage[1];
    comm_frame_t input;
    comm_frame_t output;
    comm_frame_queue_stats_t stats;
    size_t index;

    fill_frame(&input, 1u, 0x11u);
    TEST_CHECK(comm_frame_queue_init(&queue, storage, 1u) == COMM_FRAME_QUEUE_OK);
    TEST_CHECK(comm_frame_queue_stats_reset(&queue, &stats) == COMM_FRAME_QUEUE_OK);
    stats.frames_pushed = UINT64_MAX - 1u;
    stats.frames_popped = UINT64_MAX - 1u;
    stats.push_full_count = UINT64_MAX - 1u;

    for (index = 0u; index < 3u; ++index) {
        TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &input, &stats) ==
                   COMM_FRAME_QUEUE_OK);
        TEST_CHECK(comm_frame_queue_push_with_stats(&queue, &input, &stats) ==
                   COMM_FRAME_QUEUE_FULL);
        TEST_CHECK(comm_frame_queue_pop_with_stats(&queue, &output, &stats) ==
                   COMM_FRAME_QUEUE_OK);
        TEST_CHECK(frames_are_equal(&output, &input));
        TEST_CHECK(stats.frames_pushed == UINT64_MAX);
        TEST_CHECK(stats.frames_popped == UINT64_MAX);
        TEST_CHECK(stats.push_full_count == UINT64_MAX);
        TEST_CHECK(stats.peak_size == 1u);
    }
    return 0;
}

int main(void)
{
    if (test_stats_fifo_and_full_retries() != 0) {
        return 1;
    }
    if (test_stats_reset_preserves_queued_frames() != 0) {
        return 1;
    }
    if (test_stats_validation_is_transactional() != 0) {
        return 1;
    }
    if (test_stats_counters_saturate() != 0) {
        return 1;
    }
    if (test_init_validation() != 0) {
        return 1;
    }
    if (test_init_empty_state() != 0) {
        return 1;
    }
    if (test_reset() != 0) {
        return 1;
    }
    if (test_size_queries() != 0) {
        return 1;
    }
    if (test_push_copies_frame() != 0) {
        return 1;
    }
    if (test_push_fills_capacity() != 0) {
        return 1;
    }
    if (test_push_wraps() != 0) {
        return 1;
    }
    if (test_push_rejects_invalid_request() != 0) {
        return 1;
    }
    if (test_pop_preserves_fifo_order() != 0) {
        return 1;
    }
    if (test_push_pop_wrap_cycle() != 0) {
        return 1;
    }
    if (test_pop_rejects_invalid_request() != 0) {
        return 1;
    }

    puts("test_comm_frame_queue: all tests passed");
    return 0;
}
