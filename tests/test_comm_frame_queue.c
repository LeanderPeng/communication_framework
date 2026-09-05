#include "comm_frame_queue.h"

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

    puts("test_comm_frame_queue: all tests passed");
    return 0;
}
