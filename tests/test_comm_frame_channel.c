#include "comm_frame_channel.h"

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

/*
 * 不依赖任何操作系统的假后端。
 * 它记录转发参数并返回测试指定的结果，用来验证通道层而不是队列算法。
 */
typedef struct {
    comm_frame_channel_result_t push_result;
    comm_frame_channel_result_t pop_result;
    comm_frame_channel_result_t close_result;
    comm_frame_t pushed_frame;
    comm_frame_t frame_to_pop;
    uint32_t push_timeout_ms;
    uint32_t pop_timeout_ms;
    size_t push_count;
    size_t pop_count;
    size_t close_count;
} fake_channel_backend_t;

static comm_frame_channel_result_t fake_push(void *context,
                                             const comm_frame_t *frame,
                                             uint32_t timeout_ms)
{
    fake_channel_backend_t *backend = (fake_channel_backend_t *)context;

    backend->push_count += 1u;
    backend->push_timeout_ms = timeout_ms;
    backend->pushed_frame = *frame;

    return backend->push_result;
}

static comm_frame_channel_result_t fake_pop(void *context,
                                            comm_frame_t *frame,
                                            uint32_t timeout_ms)
{
    fake_channel_backend_t *backend = (fake_channel_backend_t *)context;

    backend->pop_count += 1u;
    backend->pop_timeout_ms = timeout_ms;

    /* 模拟真实后端约定：只有成功时才修改输出帧。 */
    if (backend->pop_result == COMM_FRAME_CHANNEL_OK) {
        *frame = backend->frame_to_pop;
    }

    return backend->pop_result;
}

static comm_frame_channel_result_t fake_close(void *context)
{
    fake_channel_backend_t *backend = (fake_channel_backend_t *)context;

    backend->close_count += 1u;
    return backend->close_result;
}

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

/* 验证 bind 要求完整参数，并且失败时不产生部分绑定。 */
static int test_bind_validation(void)
{
    comm_frame_channel_t channel;
    comm_frame_channel_t original_channel;
    fake_channel_backend_t backend;

    memset(&channel, 0xA5, sizeof(channel));
    memcpy(&original_channel, &channel, sizeof(channel));
    memset(&backend, 0, sizeof(backend));

    TEST_CHECK(comm_frame_channel_bind(NULL,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       NULL,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       NULL,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       NULL,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       NULL) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(memcmp(&channel, &original_channel, sizeof(channel)) == 0);

    return 0;
}

/* 验证成功绑定只保存地址，不复制或初始化后端对象。 */
static int test_bind_success(void)
{
    comm_frame_channel_t channel;
    fake_channel_backend_t backend;

    memset(&backend, 0, sizeof(backend));

    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(channel.context == &backend);
    TEST_CHECK(channel.push == fake_push);
    TEST_CHECK(channel.pop == fake_pop);
    TEST_CHECK(channel.close == fake_close);

    return 0;
}

/* 验证 push 将上下文、帧和超时原样交给后端，并透传后端结果。 */
static int test_push_forwarding(void)
{
    comm_frame_channel_t channel;
    fake_channel_backend_t backend;
    comm_frame_t frame;

    memset(&backend, 0, sizeof(backend));
    fill_frame(&frame, 0x1234u, 0x5Au);
    backend.push_result = COMM_FRAME_CHANNEL_TIMEOUT;

    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_push(&channel, &frame, 25u) ==
               COMM_FRAME_CHANNEL_TIMEOUT);
    TEST_CHECK(backend.push_count == 1u);
    TEST_CHECK(backend.push_timeout_ms == 25u);
    TEST_CHECK(backend.pushed_frame.sequence == frame.sequence);
    TEST_CHECK(backend.pushed_frame.payload[0] == frame.payload[0]);

    backend.push_result = COMM_FRAME_CHANNEL_CLOSED;
    TEST_CHECK(comm_frame_channel_push(
                   &channel,
                   &frame,
                   COMM_FRAME_CHANNEL_WAIT_FOREVER) ==
               COMM_FRAME_CHANNEL_CLOSED);
    TEST_CHECK(backend.push_count == 2u);
    TEST_CHECK(backend.push_timeout_ms ==
               COMM_FRAME_CHANNEL_WAIT_FOREVER);

    return 0;
}

/* 验证 pop 转发参数、成功输出和失败时保持输出不变的约定。 */
static int test_pop_forwarding(void)
{
    comm_frame_channel_t channel;
    fake_channel_backend_t backend;
    comm_frame_t output;

    memset(&backend, 0, sizeof(backend));
    fill_frame(&backend.frame_to_pop, 0x1234u, 0x5Au);
    backend.pop_result = COMM_FRAME_CHANNEL_OK;

    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 30u) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(backend.pop_count == 1u);
    TEST_CHECK(backend.pop_timeout_ms == 30u);
    TEST_CHECK(output.sequence == backend.frame_to_pop.sequence);
    TEST_CHECK(output.payload[0] == backend.frame_to_pop.payload[0]);

    fill_frame(&output, 0xABCDu, 0x11u);
    backend.pop_result = COMM_FRAME_CHANNEL_TIMEOUT;
    TEST_CHECK(comm_frame_channel_pop(&channel, &output, 0u) ==
               COMM_FRAME_CHANNEL_TIMEOUT);
    TEST_CHECK(output.sequence == 0xABCDu);
    TEST_CHECK(output.payload[0] == 0x11u);

    return 0;
}

/* 验证 close 被转发到同一个后端对象，并透传关闭结果。 */
static int test_close_forwarding(void)
{
    comm_frame_channel_t channel;
    fake_channel_backend_t backend;

    memset(&backend, 0, sizeof(backend));
    backend.close_result = COMM_FRAME_CHANNEL_BACKEND_ERROR;

    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &backend,
                                       fake_push,
                                       fake_pop,
                                       fake_close) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(comm_frame_channel_close(&channel) ==
               COMM_FRAME_CHANNEL_BACKEND_ERROR);
    TEST_CHECK(backend.close_count == 1u);

    return 0;
}

/* 验证参数错误和不完整绑定不会调用任何后端函数。 */
static int test_operation_validation(void)
{
    comm_frame_channel_t channel;
    fake_channel_backend_t backend;
    comm_frame_t frame;

    memset(&channel, 0, sizeof(channel));
    memset(&backend, 0, sizeof(backend));
    fill_frame(&frame, 0x1234u, 0x5Au);

    TEST_CHECK(comm_frame_channel_push(NULL, &frame, 0u) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_push(&channel, &frame, 0u) ==
               COMM_FRAME_CHANNEL_INVALID_STATE);
    TEST_CHECK(comm_frame_channel_pop(NULL, &frame, 0u) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_pop(&channel, &frame, 0u) ==
               COMM_FRAME_CHANNEL_INVALID_STATE);
    TEST_CHECK(comm_frame_channel_close(NULL) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_close(&channel) ==
               COMM_FRAME_CHANNEL_INVALID_STATE);

    channel.context = &backend;
    channel.push = fake_push;
    channel.pop = fake_pop;
    channel.close = NULL;
    TEST_CHECK(comm_frame_channel_push(&channel, NULL, 0u) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_pop(&channel, NULL, 0u) ==
               COMM_FRAME_CHANNEL_NULL_ARGUMENT);
    TEST_CHECK(comm_frame_channel_push(&channel, &frame, 0u) ==
               COMM_FRAME_CHANNEL_INVALID_STATE);
    TEST_CHECK(backend.push_count == 0u);
    TEST_CHECK(backend.pop_count == 0u);
    TEST_CHECK(backend.close_count == 0u);

    return 0;
}

int main(void)
{
    if (test_bind_validation() != 0) {
        return 1;
    }
    if (test_bind_success() != 0) {
        return 1;
    }
    if (test_push_forwarding() != 0) {
        return 1;
    }
    if (test_pop_forwarding() != 0) {
        return 1;
    }
    if (test_close_forwarding() != 0) {
        return 1;
    }
    if (test_operation_validation() != 0) {
        return 1;
    }

    puts("test_comm_frame_channel: all tests passed");
    return 0;
}
