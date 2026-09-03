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

    puts("test_comm_ringbuffer: all tests passed");
    return 0;
}
