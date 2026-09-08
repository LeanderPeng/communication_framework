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

/*
 * 事件记录器复制同步回调参数，便于在回调返回后检查事件内容。
 * manager 可选；提供时还会记录匹配 sequence 在回调执行期间是否仍为 active，
 * 用来验证 handle_frame 确实先结束 pending 事务、再通知上层。
 */
typedef struct {
    comm_message_event_t last_event;
    size_t event_count;
    comm_message_manager_t *manager;
    int matched_sequence_was_active_during_callback;
} fake_event_recorder_t;

static void record_event_callback(void *context,
                                  const comm_message_event_t *event)
{
    fake_event_recorder_t *recorder = (fake_event_recorder_t *)context;
    size_t index;

    recorder->last_event = *event;
    recorder->event_count += 1u;
    recorder->matched_sequence_was_active_during_callback = 0;

    if (recorder->manager == NULL) {
        return;
    }

    for (index = 0u; index < recorder->manager->pending_capacity; ++index) {
        if ((recorder->manager->pending_storage[index].active == 1) &&
            (recorder->manager->pending_storage[index].request.sequence ==
             event->frame.sequence)) {
            recorder->matched_sequence_was_active_during_callback = 1;
            return;
        }
    }
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

/* 验证请求发送成功后才登记 pending、截止时间和输出序号。 */
static int test_send_request_success(void)
{
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    uint16_t sequence = 0xA5A5u;

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

    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 payload,
                                                 sizeof(payload),
                                                 1000u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(sequence == 1u);
    TEST_CHECK(manager.next_sequence == 2u);
    TEST_CHECK(backend.push_count == 1u);
    TEST_CHECK(backend.timeout_ms == config.send_timeout_ms);
    TEST_CHECK(backend.pushed_frame.version == COMM_FRAME_VERSION);
    TEST_CHECK(backend.pushed_frame.type == COMM_FRAME_TYPE_REQUEST);
    TEST_CHECK(backend.pushed_frame.sequence == 1u);
    TEST_CHECK(backend.pushed_frame.payload_length == sizeof(payload));
    TEST_CHECK(memcmp(backend.pushed_frame.payload,
                      payload,
                      sizeof(payload)) == 0);
    TEST_CHECK(backend.pushed_frame.payload[sizeof(payload)] == 0u);

    TEST_CHECK(pending[0].active == 1);
    TEST_CHECK(pending[0].request.type == COMM_FRAME_TYPE_REQUEST);
    TEST_CHECK(pending[0].request.sequence == 1u);
    TEST_CHECK(pending[0].deadline_ms == 1100u);
    TEST_CHECK(pending[0].retries_done == 0u);
    TEST_CHECK(pending[1].active == 0);

    return 0;
}

/* 验证 sequence 会跳过活动请求，并在 UINT16_MAX 后绕回 1。 */
static int test_send_request_sequence_allocation(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[4];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    uint16_t sequence;

    memset(&backend, 0, sizeof(backend));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 0u;
    config.max_retries = 1u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         4u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_OK);

    memset(&pending[0], 0, sizeof(pending[0]));
    pending[0].request.version = COMM_FRAME_VERSION;
    pending[0].request.type = COMM_FRAME_TYPE_REQUEST;
    pending[0].request.sequence = 1u;
    pending[0].active = 1;
    manager.next_sequence = 1u;

    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1000u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(sequence == 2u);
    TEST_CHECK(pending[1].request.sequence == 2u);

    manager.next_sequence = UINT16_MAX;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1100u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(sequence == UINT16_MAX);
    TEST_CHECK(manager.next_sequence == 1u);

    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1200u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(sequence == 3u);
    TEST_CHECK(manager.next_sequence == 4u);

    sequence = 0xA5A5u;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1300u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_PENDING_FULL);
    TEST_CHECK(sequence == 0xA5A5u);
    TEST_CHECK(backend.push_count == 3u);

    return 0;
}

/* 验证发送失败不会提交 pending、推进 sequence 或修改输出值。 */
static int test_send_request_failure_is_transactional(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[1];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    uint16_t sequence = 0xA5A5u;
    uint8_t payload = 0x5Au;

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

    backend.push_result = COMM_FRAME_CHANNEL_TIMEOUT;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 1000u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT);
    TEST_CHECK(pending[0].active == 0);
    TEST_CHECK(manager.next_sequence == 1u);
    TEST_CHECK(sequence == 0xA5A5u);

    backend.push_result = COMM_FRAME_CHANNEL_CLOSED;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 1000u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_CHANNEL_CLOSED);
    TEST_CHECK(pending[0].active == 0);
    TEST_CHECK(manager.next_sequence == 1u);
    TEST_CHECK(sequence == 0xA5A5u);

    backend.push_result = COMM_FRAME_CHANNEL_OK;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 UINT64_MAX - 50u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(pending[0].deadline_ms == UINT64_MAX);

    return 0;
}

/* 验证请求参数和损坏的 pending 状态会在发送前被拒绝。 */
static int test_send_request_validation(void)
{
    comm_message_manager_t manager;
    comm_message_manager_t uninitialized_manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    uint16_t sequence = 0xA5A5u;
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
                                         2u,
                                         &config,
                                         fake_event_callback,
                                         NULL) ==
               COMM_MESSAGE_MANAGER_OK);

    TEST_CHECK(comm_message_manager_send_request(NULL,
                                                 &payload,
                                                 1u,
                                                 0u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 1u,
                                                 0u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 0u,
                                                 NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_send_request(
                   &manager,
                   &payload,
                   COMM_FRAME_MAX_PAYLOAD_SIZE + 1u,
                   0u,
                   &sequence) == COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE);
    TEST_CHECK(comm_message_manager_send_request(&uninitialized_manager,
                                                 &payload,
                                                 1u,
                                                 0u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);

    manager.next_sequence = 0u;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 0u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(comm_message_manager_reset(&manager) ==
               COMM_MESSAGE_MANAGER_OK);

    pending[0].active = 123;
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 &payload,
                                                 1u,
                                                 0u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(backend.push_count == 0u);
    TEST_CHECK(sequence == 0xA5A5u);

    return 0;
}

/* 验证对端 REQUEST 和主动 REPORT 直接产生事件，不占用 pending。 */
static int test_handle_request_and_report(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    fake_event_recorder_t recorder;
    comm_frame_t frame;

    memset(&backend, 0, sizeof(backend));
    memset(&recorder, 0, sizeof(recorder));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         record_event_callback,
                                         &recorder) ==
               COMM_MESSAGE_MANAGER_OK);

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_REQUEST;
    frame.sequence = 0x1234u;
    frame.payload_length = 2u;
    frame.payload[0] = 0x10u;
    frame.payload[1] = 0x20u;

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 1u);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_REQUEST_RECEIVED);
    TEST_CHECK(memcmp(&recorder.last_event.frame,
                      &frame,
                      sizeof(frame)) == 0);
    TEST_CHECK(recorder.last_event.retries_done == 0u);
    TEST_CHECK(pending[0].active == 0);
    TEST_CHECK(pending[1].active == 0);

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_REPORT;
    frame.sequence = 0u;
    frame.payload_length = 1u;
    frame.payload[0] = 0x30u;

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 2u);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    TEST_CHECK(recorder.last_event.frame.payload[0] == 0x30u);
    TEST_CHECK(recorder.last_event.retries_done == 0u);
    TEST_CHECK(pending[0].active == 0);
    TEST_CHECK(pending[1].active == 0);

    return 0;
}

/*
 * 验证 RESPONSE/ERROR 通过 sequence 找到正确 pending，传播该请求的重试次数，
 * 并且在进入事件回调之前释放对应槽位；其他并行请求不受影响。
 */
static int test_handle_matched_reply(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[3];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    fake_event_recorder_t recorder;
    comm_frame_t frame;
    uint16_t first_sequence;
    uint16_t second_sequence;

    memset(&backend, 0, sizeof(backend));
    memset(&recorder, 0, sizeof(recorder));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         3u,
                                         &config,
                                         record_event_callback,
                                         &recorder) ==
               COMM_MESSAGE_MANAGER_OK);
    recorder.manager = &manager;

    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1000u,
                                                 &first_sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1010u,
                                                 &second_sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(first_sequence == 1u);
    TEST_CHECK(second_sequence == 2u);

    /* 模拟第二个请求已经重发两次，并让它先于第一个请求收到回复。 */
    pending[1].retries_done = 2u;
    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_RESPONSE;
    frame.sequence = second_sequence;
    frame.payload_length = 1u;
    frame.payload[0] = 0xA2u;

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 1u);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
    TEST_CHECK(recorder.last_event.frame.sequence == second_sequence);
    TEST_CHECK(recorder.last_event.retries_done == 2u);
    TEST_CHECK(recorder.matched_sequence_was_active_during_callback == 0);
    TEST_CHECK(pending[0].active == 1);
    TEST_CHECK(pending[1].active == 0);

    pending[0].retries_done = 1u;
    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_ERROR;
    frame.sequence = first_sequence;
    frame.payload_length = 1u;
    frame.payload[0] = 0xE1u;

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 2u);
    TEST_CHECK(recorder.last_event.type == COMM_MESSAGE_EVENT_ERROR_RECEIVED);
    TEST_CHECK(recorder.last_event.frame.sequence == first_sequence);
    TEST_CHECK(recorder.last_event.retries_done == 1u);
    TEST_CHECK(recorder.matched_sequence_was_active_during_callback == 0);
    TEST_CHECK(pending[0].active == 0);

    return 0;
}

/* 验证未知、迟到或重复回复产生 UNMATCHED_REPLY，且不会误删其他请求。 */
static int test_handle_unmatched_reply(void)
{
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    fake_event_recorder_t recorder;
    comm_frame_t frame;
    uint16_t sequence;

    memset(&backend, 0, sizeof(backend));
    memset(&recorder, 0, sizeof(recorder));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 1u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         record_event_callback,
                                         &recorder) ==
               COMM_MESSAGE_MANAGER_OK);

    TEST_CHECK(comm_message_manager_send_request(&manager,
                                                 NULL,
                                                 0u,
                                                 1000u,
                                                 &sequence) ==
               COMM_MESSAGE_MANAGER_OK);

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = COMM_FRAME_TYPE_RESPONSE;
    frame.sequence = 0x9999u;

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 1u);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_UNMATCHED_REPLY);
    TEST_CHECK(recorder.last_event.frame.sequence == 0x9999u);
    TEST_CHECK(recorder.last_event.retries_done == 0u);
    TEST_CHECK(pending[0].active == 1);

    /* 第一次匹配结束事务；同一帧再次到达时已经没有活动 pending。 */
    frame.sequence = sequence;
    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
    TEST_CHECK(pending[0].active == 0);

    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(recorder.event_count == 3u);
    TEST_CHECK(recorder.last_event.type ==
               COMM_MESSAGE_EVENT_UNMATCHED_REPLY);
    TEST_CHECK(recorder.last_event.frame.sequence == sequence);

    return 0;
}

/* 验证参数、manager 状态和未知帧类型错误都不会触发事件回调。 */
static int test_handle_frame_validation(void)
{
    comm_message_manager_t manager;
    comm_message_manager_t uninitialized_manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[1];
    comm_message_manager_config_t config;
    fake_channel_backend_t backend;
    fake_event_recorder_t recorder;
    comm_frame_t frame;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(&backend, 0, sizeof(backend));
    memset(&recorder, 0, sizeof(recorder));
    memset(&frame, 0, sizeof(frame));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 1u;
    TEST_CHECK(make_bound_channel(&channel, &backend));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         1u,
                                         &config,
                                         record_event_callback,
                                         &recorder) ==
               COMM_MESSAGE_MANAGER_OK);

    frame.version = COMM_FRAME_VERSION;
    frame.type = 0xFFu;
    TEST_CHECK(comm_message_manager_handle_frame(NULL, &frame) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_handle_frame(&manager, NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(comm_message_manager_handle_frame(&uninitialized_manager,
                                                 &frame) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_UNSUPPORTED_FRAME_TYPE);
    TEST_CHECK(recorder.event_count == 0u);

    pending[0].active = 123;
    frame.type = COMM_FRAME_TYPE_REPORT;
    TEST_CHECK(comm_message_manager_handle_frame(&manager, &frame) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(recorder.event_count == 0u);

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
    if (test_send_request_success() != 0) {
        return 1;
    }
    if (test_send_request_sequence_allocation() != 0) {
        return 1;
    }
    if (test_send_request_failure_is_transactional() != 0) {
        return 1;
    }
    if (test_send_request_validation() != 0) {
        return 1;
    }
    if (test_handle_request_and_report() != 0) {
        return 1;
    }
    if (test_handle_matched_reply() != 0) {
        return 1;
    }
    if (test_handle_unmatched_reply() != 0) {
        return 1;
    }
    if (test_handle_frame_validation() != 0) {
        return 1;
    }

    puts("test_comm_message_manager: all tests passed");
    return 0;
}
