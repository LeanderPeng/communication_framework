#include "comm_frame_queue_pthread.h"

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

    memset(&input, 0, sizeof(input));
    input.version = COMM_FRAME_VERSION;
    input.type = COMM_FRAME_TYPE_REPORT;
    input.sequence = 0x1234u;
    input.payload_length = 1u;
    input.payload[0] = 0x5Au;

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

    puts("test_comm_frame_queue_pthread: all tests passed");
    return 0;
}
