#include "comm_frame_channel_pthread.h"

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

/* 验证绑定参数及未初始化后端不会留下部分绑定。 */
static int test_bind_validation(void)
{
    comm_frame_channel_t channel;
    comm_frame_channel_t original_channel;
    comm_frame_queue_pthread_t queue;

    memset(&channel, 0xA5, sizeof(channel));
    memcpy(&original_channel, &channel, sizeof(channel));
    memset(&queue, 0, sizeof(queue));

    TEST_CHECK(comm_frame_channel_pthread_bind(NULL, &queue) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, NULL) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, &queue) ==
               COMM_FRAME_CHANNEL_INVALID_STATE);
    TEST_CHECK(memcmp(&channel, &original_channel, sizeof(channel)) == 0);

    return 0;
}

/* 验证通道可以通过 pthread 后端完成真实的入队和出队。 */
static int test_push_and_pop_through_channel(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_channel_t channel;
    comm_frame_t storage[2];
    comm_frame_t input;
    comm_frame_t output;

    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 2u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, &queue) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(channel.context == &queue);

    TEST_CHECK(comm_frame_channel_push(&channel, &input, 0u) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 0u) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(output.version == input.version);
    TEST_CHECK(output.type == input.type);
    TEST_CHECK(output.sequence == input.sequence);
    TEST_CHECK(output.payload_length == input.payload_length);
    TEST_CHECK(output.payload[0] == input.payload[0]);
    TEST_CHECK(queue.core.used == 0u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证 pthread 的满、空超时会被转换为通道 TIMEOUT。 */
static int test_timeout_mapping(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_channel_t channel;
    comm_frame_t storage[1];
    comm_frame_t first_frame;
    comm_frame_t second_frame;
    comm_frame_t output;

    fill_frame(&first_frame, 1u, 0x11u);
    fill_frame(&second_frame, 2u, 0x22u);

    TEST_CHECK(COMM_FRAME_CHANNEL_WAIT_FOREVER ==
               COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER);
    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, &queue) ==
               COMM_FRAME_CHANNEL_OK);

    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 0u) ==
               COMM_FRAME_CHANNEL_TIMEOUT);
    TEST_CHECK(comm_frame_channel_push(&channel, &first_frame, 0u) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_push(&channel, &second_frame, 0u) ==
               COMM_FRAME_CHANNEL_TIMEOUT);
    TEST_CHECK(queue.core.used == 1u);
    TEST_CHECK(storage[0].sequence == first_frame.sequence);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证通道保持 pthread 后端的关闭和排空语义。 */
static int test_close_mapping_and_drain(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_channel_t channel;
    comm_frame_t storage[1];
    comm_frame_t input;
    comm_frame_t output;

    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, &queue) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_push(&channel, &input, 0u) ==
               COMM_FRAME_CHANNEL_OK);

    TEST_CHECK(comm_frame_channel_close(&channel) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_push(&channel, &input, 0u) ==
               COMM_FRAME_CHANNEL_CLOSED);
    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 0u) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(output.sequence == input.sequence);

    output.sequence = 0xA5A5u;
    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 0u) ==
               COMM_FRAME_CHANNEL_CLOSED);
    TEST_CHECK(output.sequence == 0xA5A5u);

    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    return 0;
}

/* 验证后端被销毁后，通道会把其状态错误隐藏为 BACKEND_ERROR。 */
static int test_backend_error_mapping(void)
{
    comm_frame_queue_pthread_t queue;
    comm_frame_channel_t channel;
    comm_frame_t storage[1];
    comm_frame_t input;

    fill_frame(&input, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_queue_pthread_init(&queue, storage, 1u) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);
    TEST_CHECK(comm_frame_channel_pthread_bind(&channel, &queue) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_queue_pthread_destroy(&queue) ==
               COMM_FRAME_QUEUE_PTHREAD_OK);

    TEST_CHECK(comm_frame_channel_push(&channel, &input, 0u) ==
               COMM_FRAME_CHANNEL_BACKEND_ERROR);

    return 0;
}

int main(void)
{
    if (test_bind_validation() != 0) {
        return 1;
    }
    if (test_push_and_pop_through_channel() != 0) {
        return 1;
    }
    if (test_timeout_mapping() != 0) {
        return 1;
    }
    if (test_close_mapping_and_drain() != 0) {
        return 1;
    }
    if (test_backend_error_mapping() != 0) {
        return 1;
    }

    puts("test_comm_frame_channel_pthread: all tests passed");
    return 0;
}
