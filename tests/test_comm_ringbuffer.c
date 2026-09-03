#include "comm_ringbuffer.h"

#include <stdio.h>

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

    puts("test_comm_ringbuffer: all tests passed");
    return 0;
}
