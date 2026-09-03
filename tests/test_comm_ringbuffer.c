#include "comm_ringbuffer.h"

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
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    ringbuffer.storage = storage;
    ringbuffer.capacity = 7u;
    ringbuffer.read_index = 1u;
    ringbuffer.write_index = 4u;
    ringbuffer.used = 3u;

    TEST_CHECK(comm_ringbuffer_init(NULL, storage, sizeof(storage)) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer, NULL, sizeof(storage)) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);
    TEST_CHECK(ringbuffer.storage == storage);
    TEST_CHECK(ringbuffer.capacity == 7u);
    TEST_CHECK(ringbuffer.read_index == 1u);
    TEST_CHECK(ringbuffer.write_index == 4u);
    TEST_CHECK(ringbuffer.used == 3u);

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer, storage, 0u) ==
               COMM_RINGBUFFER_INVALID_CAPACITY);
    TEST_CHECK(ringbuffer.capacity == 7u);
    TEST_CHECK(ringbuffer.used == 3u);

    return 0;
}

/* 验证初始化后的空状态和完整可用容量。 */
static int test_init_empty_state(void)
{
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(ringbuffer.storage == storage);
    TEST_CHECK(ringbuffer.capacity == sizeof(storage));
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == 0u);
    TEST_CHECK(ringbuffer.used == 0u);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == sizeof(storage));

    return 0;
}

/* 验证 reset 能保留存储配置并修复损坏的运行状态。 */
static int test_reset(void)
{
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    ringbuffer.read_index = 99u;
    ringbuffer.write_index = 88u;
    ringbuffer.used = 77u;

    TEST_CHECK(comm_ringbuffer_reset(&ringbuffer) == COMM_RINGBUFFER_OK);
    TEST_CHECK(ringbuffer.storage == storage);
    TEST_CHECK(ringbuffer.capacity == sizeof(storage));
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == 0u);
    TEST_CHECK(ringbuffer.used == 0u);

    TEST_CHECK(comm_ringbuffer_reset(NULL) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);
    ringbuffer.storage = NULL;
    TEST_CHECK(comm_ringbuffer_reset(&ringbuffer) ==
               COMM_RINGBUFFER_INVALID_STATE);

    return 0;
}

/* 验证空、满、绕回以及损坏状态下的容量查询。 */
static int test_size_queries(void)
{
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);

    ringbuffer.read_index = 3u;
    ringbuffer.write_index = 3u;
    ringbuffer.used = sizeof(storage);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == sizeof(storage));
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 0u);

    ringbuffer.read_index = 6u;
    ringbuffer.write_index = 2u;
    ringbuffer.used = 4u;
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 4u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 4u);

    ringbuffer.read_index = 2u;
    ringbuffer.write_index = 2u;
    ringbuffer.used = 4u;
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 0u);

    ringbuffer.read_index = 6u;
    ringbuffer.write_index = 3u;
    ringbuffer.used = 4u;
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 0u);

    ringbuffer.read_index = sizeof(storage);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 0u);

    TEST_CHECK(comm_ringbuffer_size(NULL) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(NULL) == 0u);

    return 0;
}

/* 验证普通连续写入和零长度写入。 */
static int test_write_contiguous(void)
{
    static const uint8_t data[] = {0x10u, 0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    memset(storage, 0xA5, sizeof(storage));
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer, NULL, 0u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);

    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == sizeof(data));
    TEST_CHECK(ringbuffer.used == sizeof(data));
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == sizeof(data));
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) ==
               sizeof(storage) - sizeof(data));
    TEST_CHECK(memcmp(storage, data, sizeof(data)) == 0);
    TEST_CHECK(storage[sizeof(data)] == 0xA5u);

    return 0;
}

/* 验证写到数组末尾后会从索引 0 继续写入。 */
static int test_write_wraps(void)
{
    static const uint8_t data[] = {0x10u, 0x20u, 0x30u, 0x40u};
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    memset(storage, 0xA5, sizeof(storage));
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    ringbuffer.read_index = 6u;
    ringbuffer.write_index = 6u;

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(storage[6] == 0x10u);
    TEST_CHECK(storage[7] == 0x20u);
    TEST_CHECK(storage[0] == 0x30u);
    TEST_CHECK(storage[1] == 0x40u);
    TEST_CHECK(ringbuffer.read_index == 6u);
    TEST_CHECK(ringbuffer.write_index == 2u);
    TEST_CHECK(ringbuffer.used == sizeof(data));

    return 0;
}

/* 验证全部容量均可使用，并能形成 read == write 的满状态。 */
static int test_write_fills_capacity(void)
{
    static const uint8_t data[] = {
        0x00u, 0x01u, 0x02u, 0x03u,
        0x04u, 0x05u, 0x06u, 0x07u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[sizeof(data)];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(memcmp(storage, data, sizeof(data)) == 0);
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == 0u);
    TEST_CHECK(ringbuffer.used == sizeof(storage));
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == sizeof(storage));
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == 0u);

    return 0;
}

/* 验证参数、状态或空间错误不会产生部分写入。 */
static int test_write_rejects_invalid_request(void)
{
    static const uint8_t data[] = {
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t original_storage[sizeof(storage)];

    memset(storage, 0xA5, sizeof(storage));
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer, NULL, 1u) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);
    TEST_CHECK(ringbuffer.write_index == 0u);
    TEST_CHECK(ringbuffer.used == 0u);

    ringbuffer.read_index = 2u;
    ringbuffer.write_index = 6u;
    ringbuffer.used = 4u;
    memcpy(original_storage, storage, sizeof(storage));
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) ==
               COMM_RINGBUFFER_INSUFFICIENT_SPACE);
    TEST_CHECK(ringbuffer.read_index == 2u);
    TEST_CHECK(ringbuffer.write_index == 6u);
    TEST_CHECK(ringbuffer.used == 4u);
    TEST_CHECK(memcmp(storage, original_storage, sizeof(storage)) == 0);

    ringbuffer.write_index = 7u;
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer, data, 1u) ==
               COMM_RINGBUFFER_INVALID_STATE);
    TEST_CHECK(memcmp(storage, original_storage, sizeof(storage)) == 0);

    TEST_CHECK(comm_ringbuffer_write(NULL, data, sizeof(data)) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);

    return 0;
}

/* 验证普通连续读取只移除指定数量的数据。 */
static int test_read_contiguous(void)
{
    static const uint8_t data[] = {
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u
    };
    static const uint8_t expected[] = {0x10u, 0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t output[sizeof(expected)];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    output,
                                    sizeof(output)) == COMM_RINGBUFFER_OK);

    TEST_CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    TEST_CHECK(ringbuffer.read_index == sizeof(output));
    TEST_CHECK(ringbuffer.write_index == sizeof(data));
    TEST_CHECK(ringbuffer.used == sizeof(data) - sizeof(output));
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) ==
               sizeof(data) - sizeof(output));

    return 0;
}

/* 验证读取跨过数组末尾后，会从索引 0 继续读取。 */
static int test_read_wraps(void)
{
    static const uint8_t expected[] = {0x10u, 0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t output[sizeof(expected)];
    uint8_t last_byte = 0u;

    memset(storage, 0xA5, sizeof(storage));
    storage[6] = 0x10u;
    storage[7] = 0x20u;
    storage[0] = 0x30u;
    storage[1] = 0x40u;

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    ringbuffer.read_index = 6u;
    ringbuffer.write_index = 2u;
    ringbuffer.used = 4u;

    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    output,
                                    sizeof(output)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    TEST_CHECK(ringbuffer.read_index == 1u);
    TEST_CHECK(ringbuffer.write_index == 2u);
    TEST_CHECK(ringbuffer.used == 1u);

    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    &last_byte,
                                    1u) == COMM_RINGBUFFER_OK);
    TEST_CHECK(last_byte == 0x40u);
    TEST_CHECK(ringbuffer.read_index == 2u);
    TEST_CHECK(ringbuffer.write_index == 2u);
    TEST_CHECK(ringbuffer.used == 0u);

    return 0;
}

/* 验证一次可以读出全部容量，并恢复 read == write 的空状态。 */
static int test_read_empties_capacity(void)
{
    static const uint8_t data[] = {
        0x00u, 0x01u, 0x02u, 0x03u,
        0x04u, 0x05u, 0x06u, 0x07u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[sizeof(data)];
    uint8_t output[sizeof(data)];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    output,
                                    sizeof(output)) == COMM_RINGBUFFER_OK);

    TEST_CHECK(memcmp(output, data, sizeof(data)) == 0);
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == 0u);
    TEST_CHECK(ringbuffer.used == 0u);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);
    TEST_CHECK(comm_ringbuffer_free_space(&ringbuffer) == sizeof(storage));

    return 0;
}

/* 验证参数、状态或数据量错误不会产生部分读取。 */
static int test_read_rejects_invalid_request(void)
{
    static const uint8_t data[] = {0x10u, 0x20u, 0x30u};
    static const uint8_t untouched_output[] = {
        0xA5u, 0xA5u, 0xA5u, 0xA5u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t output[4];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_ringbuffer_read(&ringbuffer, NULL, 0u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer, NULL, 1u) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.used == sizeof(data));

    memset(output, 0xA5, sizeof(output));
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    output,
                                    sizeof(output)) ==
               COMM_RINGBUFFER_INSUFFICIENT_DATA);
    TEST_CHECK(memcmp(output,
                      untouched_output,
                      sizeof(output)) == 0);
    TEST_CHECK(ringbuffer.read_index == 0u);
    TEST_CHECK(ringbuffer.write_index == sizeof(data));
    TEST_CHECK(ringbuffer.used == sizeof(data));

    ringbuffer.write_index = 4u;
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer, output, 1u) ==
               COMM_RINGBUFFER_INVALID_STATE);
    TEST_CHECK(output[0] == 0xA5u);

    TEST_CHECK(comm_ringbuffer_read(NULL, output, sizeof(output)) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);

    return 0;
}

/* 验证多次写读交错后，逻辑数据顺序仍保持不变。 */
static int test_write_read_cycle(void)
{
    static const uint8_t first_data[] = {
        0xA0u, 0xB0u, 0xC0u, 0xD0u, 0xE0u, 0xF0u
    };
    static const uint8_t second_data[] = {
        0x10u, 0x20u, 0x30u, 0x40u
    };
    static const uint8_t expected_remaining[] = {
        0xE0u, 0xF0u, 0x10u, 0x20u, 0x30u, 0x40u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t discarded[4];
    uint8_t remaining[sizeof(expected_remaining)];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     first_data,
                                     sizeof(first_data)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    discarded,
                                    sizeof(discarded)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     second_data,
                                     sizeof(second_data)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    remaining,
                                    sizeof(remaining)) ==
               COMM_RINGBUFFER_OK);

    TEST_CHECK(memcmp(remaining,
                      expected_remaining,
                      sizeof(remaining)) == 0);
    TEST_CHECK(ringbuffer.read_index == 2u);
    TEST_CHECK(ringbuffer.write_index == 2u);
    TEST_CHECK(ringbuffer.used == 0u);

    return 0;
}

/* 验证 peek 可以按偏移查看数据，且不会改变 RingBuffer 状态。 */
static int test_peek(void)
{
    static const uint8_t first_data[] = {
        0xA0u, 0xB0u, 0xC0u, 0xD0u, 0xE0u, 0xF0u
    };
    static const uint8_t second_data[] = {
        0x10u, 0x20u, 0x30u, 0x40u
    };
    static const uint8_t expected[] = {
        0xF0u, 0x10u, 0x20u, 0x30u
    };
    static const uint8_t untouched_output[] = {
        0xA5u, 0xA5u, 0xA5u, 0xA5u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];
    uint8_t discarded[4];
    uint8_t output[sizeof(expected)];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     first_data,
                                     sizeof(first_data)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_read(&ringbuffer,
                                    discarded,
                                    sizeof(discarded)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     second_data,
                                     sizeof(second_data)) ==
               COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_ringbuffer_peek(&ringbuffer,
                                    1u,
                                    output,
                                    sizeof(output)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(memcmp(output, expected, sizeof(expected)) == 0);
    TEST_CHECK(ringbuffer.read_index == 4u);
    TEST_CHECK(ringbuffer.write_index == 2u);
    TEST_CHECK(ringbuffer.used == 6u);

    TEST_CHECK(comm_ringbuffer_peek(&ringbuffer,
                                    ringbuffer.used,
                                    NULL,
                                    0u) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_peek(&ringbuffer,
                                    ringbuffer.used + 1u,
                                    NULL,
                                    0u) ==
               COMM_RINGBUFFER_INSUFFICIENT_DATA);
    TEST_CHECK(comm_ringbuffer_peek(&ringbuffer, 0u, NULL, 1u) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);

    memset(output, 0xA5, sizeof(output));
    TEST_CHECK(comm_ringbuffer_peek(&ringbuffer,
                                    3u,
                                    output,
                                    sizeof(output)) ==
               COMM_RINGBUFFER_INSUFFICIENT_DATA);
    TEST_CHECK(memcmp(output,
                      untouched_output,
                      sizeof(output)) == 0);

    return 0;
}

/* 验证 discard 只推进状态，不需要提供无用的输出数组。 */
static int test_discard(void)
{
    static const uint8_t data[] = {
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u
    };
    comm_ringbuffer_t ringbuffer;
    uint8_t storage[8];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     data,
                                     sizeof(data)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_discard(&ringbuffer, 0u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_discard(&ringbuffer, 4u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(ringbuffer.read_index == 4u);
    TEST_CHECK(ringbuffer.write_index == 6u);
    TEST_CHECK(ringbuffer.used == 2u);

    TEST_CHECK(comm_ringbuffer_discard(&ringbuffer, 3u) ==
               COMM_RINGBUFFER_INSUFFICIENT_DATA);
    TEST_CHECK(ringbuffer.read_index == 4u);
    TEST_CHECK(ringbuffer.used == 2u);

    TEST_CHECK(comm_ringbuffer_discard(&ringbuffer, 2u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(ringbuffer.read_index == 6u);
    TEST_CHECK(ringbuffer.write_index == 6u);
    TEST_CHECK(ringbuffer.used == 0u);

    ringbuffer.write_index = 7u;
    TEST_CHECK(comm_ringbuffer_discard(&ringbuffer, 0u) ==
               COMM_RINGBUFFER_INVALID_STATE);
    TEST_CHECK(comm_ringbuffer_discard(NULL, 0u) ==
               COMM_RINGBUFFER_NULL_ARGUMENT);

    return 0;
}

int main(void)
{
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
    if (test_write_contiguous() != 0) {
        return 1;
    }
    if (test_write_wraps() != 0) {
        return 1;
    }
    if (test_write_fills_capacity() != 0) {
        return 1;
    }
    if (test_write_rejects_invalid_request() != 0) {
        return 1;
    }
    if (test_read_contiguous() != 0) {
        return 1;
    }
    if (test_read_wraps() != 0) {
        return 1;
    }
    if (test_read_empties_capacity() != 0) {
        return 1;
    }
    if (test_read_rejects_invalid_request() != 0) {
        return 1;
    }
    if (test_write_read_cycle() != 0) {
        return 1;
    }
    if (test_peek() != 0) {
        return 1;
    }
    if (test_discard() != 0) {
        return 1;
    }

    puts("test_comm_ringbuffer: all tests passed");
    return 0;
}
