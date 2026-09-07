#include "comm_message_manager.h"

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

/* 假发送后端记录 manager 构造的帧、超时和调用次数。 */
typedef struct {
    comm_frame_channel_result_t push_result;
    comm_frame_t pushed_frame;
    uint32_t timeout_ms;
    size_t push_count;
} fake_channel_backend_t;

static comm_frame_channel_result_t fake_push(void *context,
                                             const comm_frame_t *frame,
                                             uint32_t timeout_ms)
{
    fake_channel_backend_t *backend = (fake_channel_backend_t *)context;

    backend->pushed_frame = *frame;
    backend->timeout_ms = timeout_ms;
    backend->push_count += 1u;

    return backend->push_result;
}

static comm_frame_channel_result_t fake_pop(void *context,
                                            comm_frame_t *frame,
                                            uint32_t timeout_ms)
{
    (void)context;
    (void)frame;
    (void)timeout_ms;
    return COMM_FRAME_CHANNEL_OK;
}

static comm_frame_channel_result_t fake_close(void *context)
{
    (void)context;
    return COMM_FRAME_CHANNEL_OK;
}

static void fake_event_callback(void *context,
                                const comm_message_event_t *event)
{
    (void)context;
    (void)event;
}

static int make_bound_channel(comm_frame_channel_t *channel,
                              fake_channel_backend_t *backend)
{
    return comm_frame_channel_bind(channel,
                                   backend,
                                   fake_push,
                                   fake_pop,
                                   fake_close) == COMM_FRAME_CHANNEL_OK;
}

/* 验证所有参数和配置错误都不会部分修改 manager 或 pending。 */
static int test_init_validation(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_frame_channel_t unbound_channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;

    memset(&manager, 0, sizeof(manager));
    memset(&unbound_channel, 0, sizeof(unbound_channel));
    memset(pending, 0, sizeof(pending));
    memset(&backend, 0, sizeof(backend));
    manager.initialized = 7;
    manager.next_sequence = 99u;
    pending[0].active = 1;
    pending[0].deadline_ms = 1234u;

    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &backend));

    TEST_CHECK(comm_message_manager_init(NULL,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_init(&manager,
                                         NULL,
                                         pending,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         NULL,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         NULL,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         NULL,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);

    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         0u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_INVALID_CONFIG);
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &unbound_channel,
                                         pending,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_INVALID_CONFIG);

    config.response_timeout_ms = 0u;
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_INVALID_CONFIG);

    TEST_CHECK(manager.initialized == 7);
    TEST_CHECK(manager.next_sequence == 99u);
    TEST_CHECK(pending[0].active == 1);
    TEST_CHECK(pending[0].deadline_ms == 1234u);

    return 0;
}

/* 验证成功初始化会复制配置、保存非拥有型地址并清空 pending。 */
static int test_init_success(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[3];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    size_t index;

    memset(pending, 0, sizeof(pending));
    memset(&backend, 0, sizeof(backend));
    for (index = 0u; index < 3u; ++index) {
        pending[index].active = 1;
        pending[index].request.sequence = (uint16_t)(index + 10u);
    }

    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 20u;
    config.max_retries = 3u;
    TEST_CHECK(make_bound_channel(&channel, &backend));

    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         3u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(manager.tx_channel == &channel);
    TEST_CHECK(manager.pending_storage == pending);
    TEST_CHECK(manager.pending_capacity == 3u);
    TEST_CHECK(manager.config.response_timeout_ms == 100u);
    TEST_CHECK(manager.config.send_timeout_ms == 20u);
    TEST_CHECK(manager.config.max_retries == 3u);
    TEST_CHECK(manager.event_callback == fake_event_callback);
    TEST_CHECK(manager.event_context == NULL);
    TEST_CHECK(manager.next_sequence == 1u);
    TEST_CHECK(manager.initialized == 1);

    for (index = 0u; index < 3u; ++index) {
        TEST_CHECK(pending[index].active == 0);
        TEST_CHECK(pending[index].request.sequence ==
                   (uint16_t)(index + 10u));
    }

    config.response_timeout_ms = 999u;
    TEST_CHECK(manager.config.response_timeout_ms == 100u);

    return 0;
}

/* 验证 reset 修复运行状态，但保留长期配置和外部对象地址。 */
static int test_reset(void)
{
    comm_message_manager_t manager;
    comm_message_manager_t uninitialized_manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[3];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    size_t index;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(pending, 0, sizeof(pending));
    memset(&backend, 0, sizeof(backend));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 20u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         3u,
                                         &config,
                                         fake_event_callback,
                                         &backend) ==
               COMM_MESSAGE_MANAGER_OK);

    for (index = 0u; index < 3u; ++index) {
        pending[index].active = 123;
        pending[index].deadline_ms = 500u + index;
        pending[index].retries_done = (uint8_t)index;
    }
    manager.next_sequence = 0u;

    TEST_CHECK(comm_message_manager_reset(&manager) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(manager.tx_channel == &channel);
    TEST_CHECK(manager.pending_storage == pending);
    TEST_CHECK(manager.pending_capacity == 3u);
    TEST_CHECK(manager.config.response_timeout_ms == 100u);
    TEST_CHECK(manager.event_callback == fake_event_callback);
    TEST_CHECK(manager.event_context == &backend);
    TEST_CHECK(manager.next_sequence == 1u);
    TEST_CHECK(manager.initialized == 1);
    for (index = 0u; index < 3u; ++index) {
        TEST_CHECK(pending[index].active == 0);
        TEST_CHECK(pending[index].deadline_ms == 500u + index);
        TEST_CHECK(pending[index].retries_done == (uint8_t)index);
    }

    TEST_CHECK(comm_message_manager_reset(NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_reset(&uninitialized_manager) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);

    manager.pending_storage = NULL;
    TEST_CHECK(comm_message_manager_reset(&manager) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);

    return 0;
}

/* 验证 RESPONSE、REPORT 和 ERROR 的公共构帧与发送路径。 */
static int test_send_non_request_frames(void)
{
    static const uint8_t response_payload[] = {0x10u, 0x20u, 0x30u};
    static const uint8_t error_payload[] = {0xE1u};
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;

    memset(&backend, 0, sizeof(backend));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 25u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_OK);

    TEST_CHECK(comm_message_manager_send_response(
                   &manager,
                   0x1234u,
                   response_payload,
                   sizeof(response_payload)) == COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(backend.push_count == 1u);
    TEST_CHECK(backend.timeout_ms == config.send_timeout_ms);
    TEST_CHECK(backend.pushed_frame.version == COMM_FRAME_VERSION);
    TEST_CHECK(backend.pushed_frame.type == COMM_FRAME_TYPE_RESPONSE);
    TEST_CHECK(backend.pushed_frame.sequence == 0x1234u);
    TEST_CHECK(backend.pushed_frame.payload_length ==
               sizeof(response_payload));
    TEST_CHECK(memcmp(backend.pushed_frame.payload,
                      response_payload,
                      sizeof(response_payload)) == 0);
    TEST_CHECK(backend.pushed_frame.payload[sizeof(response_payload)] == 0u);

    TEST_CHECK(comm_message_manager_send_report(&manager, NULL, 0u) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(backend.push_count == 2u);
    TEST_CHECK(backend.pushed_frame.type == COMM_FRAME_TYPE_REPORT);
    TEST_CHECK(backend.pushed_frame.sequence == 0u);
    TEST_CHECK(backend.pushed_frame.payload_length == 0u);
    TEST_CHECK(backend.pushed_frame.payload[0] == 0u);

    TEST_CHECK(comm_message_manager_send_error(&manager,
                                               0x4321u,
                                               error_payload,
                                               sizeof(error_payload)) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(backend.push_count == 3u);
    TEST_CHECK(backend.pushed_frame.type == COMM_FRAME_TYPE_ERROR);
    TEST_CHECK(backend.pushed_frame.sequence == 0x4321u);
    TEST_CHECK(backend.pushed_frame.payload_length == sizeof(error_payload));
    TEST_CHECK(backend.pushed_frame.payload[0] == error_payload[0]);
    TEST_CHECK(backend.pushed_frame.payload[1] == 0u);

    return 0;
}

/* 验证发送参数错误以及通道结果到 manager 结果的转换。 */
static int test_send_validation_and_channel_mapping(void)
{
    comm_message_manager_t manager;
    comm_message_manager_t uninitialized_manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[1];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    uint8_t payload = 0x5Au;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(&backend, 0, sizeof(backend));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 1u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         1u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_OK);

    TEST_CHECK(comm_message_manager_send_report(NULL, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_send_report(&manager, NULL, 1u) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_send_report(
                   &manager,
                   &payload,
                   COMM_FRAME_MAX_PAYLOAD_SIZE + 1u) ==
               COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE);
    TEST_CHECK(comm_message_manager_send_response(&manager,
                                                  0u,
                                                  &payload,
                                                  1u) ==
               COMM_MESSAGE_MANAGER_INVALID_SEQUENCE);
    TEST_CHECK(comm_message_manager_send_error(&manager,
                                               0u,
                                               &payload,
                                               1u) ==
               COMM_MESSAGE_MANAGER_INVALID_SEQUENCE);
    TEST_CHECK(comm_message_manager_send_report(&uninitialized_manager,
                                                &payload,
                                                1u) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(backend.push_count == 0u);

    backend.push_result = COMM_FRAME_CHANNEL_TIMEOUT;
    TEST_CHECK(comm_message_manager_send_report(&manager, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT);
    backend.push_result = COMM_FRAME_CHANNEL_CLOSED;
    TEST_CHECK(comm_message_manager_send_report(&manager, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_CHANNEL_CLOSED);
    backend.push_result = COMM_FRAME_CHANNEL_BACKEND_ERROR;
    TEST_CHECK(comm_message_manager_send_report(&manager, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_CHANNEL_ERROR);
    backend.push_result = COMM_FRAME_CHANNEL_INVALID_STATE;
    TEST_CHECK(comm_message_manager_send_report(&manager, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_CHANNEL_ERROR);
    TEST_CHECK(backend.push_count == 4u);

    channel.close = NULL;
    TEST_CHECK(comm_message_manager_send_report(&manager, &payload, 1u) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(backend.push_count == 4u);

    return 0;
}

int main(void)
{
    if (test_init_validation() != 0) {
        return 1;
    }
    if (test_init_success() != 0) {
        return 1;
    }
    if (test_reset() != 0) {
        return 1;
    }
    if (test_send_non_request_frames() != 0) {
        return 1;
    }
    if (test_send_validation_and_channel_mapping() != 0) {
        return 1;
    }

    puts("test_comm_message_manager: all tests passed");
    return 0;
}
