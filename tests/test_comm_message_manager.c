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

/* init 只要求通道完整绑定，本阶段不会真正调用这些假后端函数。 */
static comm_frame_channel_result_t fake_push(void *context,
                                             const comm_frame_t *frame,
                                             uint32_t timeout_ms)
{
    (void)context;
    (void)frame;
    (void)timeout_ms;
    return COMM_FRAME_CHANNEL_OK;
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

static int make_bound_channel(comm_frame_channel_t *channel, int *context)
{
    return comm_frame_channel_bind(channel,
                                   context,
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
    int channel_context = 0;

    memset(&manager, 0, sizeof(manager));
    memset(&unbound_channel, 0, sizeof(unbound_channel));
    memset(pending, 0, sizeof(pending));
    manager.initialized = 7;
    manager.next_sequence = 99u;
    pending[0].active = 1;
    pending[0].deadline_ms = 1234u;

    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 10u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &channel_context));

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
    int channel_context = 0;
    size_t index;

    memset(pending, 0, sizeof(pending));
    for (index = 0u; index < 3u; ++index) {
        pending[index].active = 1;
        pending[index].request.sequence = (uint16_t)(index + 10u);
    }

    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 20u;
    config.max_retries = 3u;
    TEST_CHECK(make_bound_channel(&channel, &channel_context));

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
    int channel_context = 0;
    size_t index;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(pending, 0, sizeof(pending));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 20u;
    config.max_retries = 2u;
    TEST_CHECK(make_bound_channel(&channel, &channel_context));
    TEST_CHECK(comm_message_manager_init(&manager,
                                         &channel,
                                         pending,
                                         3u,
                                         &config,
                                         fake_event_callback,
                                         &channel_context) ==
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
    TEST_CHECK(manager.event_context == &channel_context);
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

    puts("test_comm_message_manager: all tests passed");
    return 0;
}
