#include "appliance_client.h"
#include "appliance_manager.h"

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

/* 假传输后端保存 Message Manager 投递的最后一帧。 */
typedef struct {
    comm_frame_t last_frame;
    uint32_t last_timeout_ms;
    size_t push_count;
} integration_transport_t;

static comm_frame_channel_result_t integration_push(
    void *context,
    const comm_frame_t *frame,
    uint32_t timeout_ms)
{
    integration_transport_t *transport =
        (integration_transport_t *)context;

    transport->last_frame = *frame;
    transport->last_timeout_ms = timeout_ms;
    transport->push_count += 1u;
    return COMM_FRAME_CHANNEL_OK;
}

static comm_frame_channel_result_t integration_pop(
    void *context,
    comm_frame_t *frame,
    uint32_t timeout_ms)
{
    (void)context;
    (void)frame;
    (void)timeout_ms;
    return COMM_FRAME_CHANNEL_OK;
}

static comm_frame_channel_result_t integration_close(void *context)
{
    (void)context;
    return COMM_FRAME_CHANNEL_OK;
}

/* 记录最终应投递给 UI 适配层的模型快照。 */
typedef struct {
    appliance_model_t last_model;
    size_t notify_count;
} integration_ui_t;

static void integration_notify_ui(void *context,
                                  const appliance_model_t *model)
{
    integration_ui_t *ui = (integration_ui_t *)context;

    ui->last_model = *model;
    ui->notify_count += 1u;
}

/*
 * 这是应用组合层：Message Manager 的事件回调没有返回值，而 Appliance Manager
 * 会返回业务解析结果，所以组合层负责调用并保存结果，供日志或诊断代码读取。
 */
typedef struct {
    appliance_manager_t *appliance_manager;
    appliance_manager_result_t last_result;
    size_t event_count;
} integration_bridge_t;

static void integration_handle_message_event(
    void *context,
    const comm_message_event_t *event)
{
    integration_bridge_t *bridge = (integration_bridge_t *)context;

    bridge->last_result = appliance_manager_handle_message_event(
        bridge->appliance_manager,
        event);
    bridge->event_count += 1u;
}

/* 构造设备对 QUERY_STATUS 的状态快照响应，sequence 必须沿用请求序号。 */
static void make_status_response(comm_frame_t *response, uint16_t sequence)
{
    memset(response, 0, sizeof(*response));
    response->version = COMM_FRAME_VERSION;
    response->type = COMM_FRAME_TYPE_RESPONSE;
    response->sequence = sequence;
    response->payload_length = APPLIANCE_STATUS_PAYLOAD_SIZE;
    response->payload[APPLIANCE_STATUS_MESSAGE_ID_OFFSET] =
        APPLIANCE_MESSAGE_STATUS_SNAPSHOT;
    response->payload[APPLIANCE_STATUS_RUN_STATE_OFFSET] =
        (uint8_t)APPLIANCE_RUN_STATE_RUNNING;
    response->payload[APPLIANCE_STATUS_PROGRAM_OFFSET] =
        (uint8_t)APPLIANCE_PROGRAM_QUICK;
    response->payload[APPLIANCE_STATUS_PROGRESS_OFFSET] = 40u;
    response->payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET] = 0x00u;
    response->payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET + 1u] = 0x1Eu;
    response->payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET] = 1u;
    response->payload[APPLIANCE_STATUS_FAULT_CODE_OFFSET] = 0x00u;
    response->payload[APPLIANCE_STATUS_FAULT_CODE_OFFSET + 1u] = 0x00u;
}

/*
 * 验证从业务查询、pending 匹配、业务状态解码到 UI 通知的完整帧级链路。
 */
static int test_query_status_round_trip(void)
{
    comm_frame_channel_t channel;
    comm_message_manager_t message_manager;
    comm_message_pending_t pending[2];
    comm_message_manager_config_t config;
    appliance_manager_t appliance_manager;
    appliance_model_t model;
    integration_transport_t transport;
    integration_ui_t ui;
    integration_bridge_t bridge;
    comm_frame_t response;
    uint16_t sequence = 0u;

    memset(&transport, 0, sizeof(transport));
    memset(&ui, 0, sizeof(ui));
    memset(&bridge, 0, sizeof(bridge));
    config.response_timeout_ms = 100u;
    config.send_timeout_ms = 25u;
    config.max_retries = 2u;

    TEST_CHECK(comm_frame_channel_bind(&channel,
                                       &transport,
                                       integration_push,
                                       integration_pop,
                                       integration_close) ==
               COMM_FRAME_CHANNEL_OK);
    TEST_CHECK(appliance_manager_init(&appliance_manager,
                                      integration_notify_ui,
                                      &ui) ==
               APPLIANCE_MANAGER_OK);
    bridge.appliance_manager = &appliance_manager;
    TEST_CHECK(comm_message_manager_init(&message_manager,
                                         &channel,
                                         pending,
                                         2u,
                                         &config,
                                         integration_handle_message_event,
                                         &bridge) ==
               COMM_MESSAGE_MANAGER_OK);

    /* 业务层发起查询后，通用 Message Manager 负责构造 REQUEST 和登记 pending。 */
    TEST_CHECK(appliance_client_request_status(&message_manager,
                                               1000u,
                                               &sequence) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(sequence == 1u);
    TEST_CHECK(transport.push_count == 1u);
    TEST_CHECK(transport.last_timeout_ms == config.send_timeout_ms);
    TEST_CHECK(transport.last_frame.type == COMM_FRAME_TYPE_REQUEST);
    TEST_CHECK(transport.last_frame.sequence == sequence);
    TEST_CHECK(transport.last_frame.payload_length ==
               APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE);
    TEST_CHECK(transport.last_frame.payload[
                   APPLIANCE_QUERY_STATUS_MESSAGE_ID_OFFSET] ==
               APPLIANCE_MESSAGE_QUERY_STATUS);
    TEST_CHECK(pending[0].active == 1);
    TEST_CHECK(pending[0].request.sequence == sequence);

    /*
     * 模拟同 sequence 响应到达。handle_frame 先结束 pending，再通过组合层把
     * STATUS_SNAPSHOT 交给 Appliance Manager，最后才产生一次 UI 模型通知。
     */
    make_status_response(&response, sequence);
    TEST_CHECK(comm_message_manager_handle_frame(&message_manager,
                                                 &response) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(pending[0].active == 0);
    TEST_CHECK(bridge.event_count == 1u);
    TEST_CHECK(bridge.last_result == APPLIANCE_MANAGER_OK);
    TEST_CHECK(ui.notify_count == 1u);
    TEST_CHECK(ui.last_model.run_state == APPLIANCE_RUN_STATE_RUNNING);
    TEST_CHECK(ui.last_model.program == APPLIANCE_PROGRAM_QUICK);
    TEST_CHECK(ui.last_model.progress_percent == 40u);
    TEST_CHECK(ui.last_model.remaining_minutes == 30u);
    TEST_CHECK(ui.last_model.door_locked == 1);

    TEST_CHECK(appliance_manager_get_model(&appliance_manager, &model) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(model.progress_percent == 40u);
    TEST_CHECK(model.remaining_minutes == 30u);

    /*
     * 相同响应再次到达时已经没有 pending，Message Manager 报告未匹配事件；
     * Appliance Manager 不把它当成有效状态更新，因此 UI 不会重复刷新。
     */
    TEST_CHECK(comm_message_manager_handle_frame(&message_manager,
                                                 &response) ==
               COMM_MESSAGE_MANAGER_OK);
    TEST_CHECK(bridge.event_count == 2u);
    TEST_CHECK(bridge.last_result == APPLIANCE_MANAGER_UNSUPPORTED_EVENT);
    TEST_CHECK(ui.notify_count == 1u);

    return 0;
}

/* 查询接口保留 Message Manager 原始的参数和状态错误，便于上层精确处理。 */
static int test_query_status_validation(void)
{
    comm_message_manager_t uninitialized_manager;
    uint16_t sequence = 0xA5A5u;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));

    TEST_CHECK(appliance_client_request_status(NULL, 0u, &sequence) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_client_request_status(&uninitialized_manager,
                                               0u,
                                               &sequence) ==
               COMM_MESSAGE_MANAGER_INVALID_STATE);
    TEST_CHECK(appliance_client_request_status(&uninitialized_manager,
                                               0u,
                                               NULL) ==
               COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(sequence == 0xA5A5u);

    return 0;
}

int main(void)
{
    if (test_query_status_round_trip() != 0) {
        return 1;
    }
    if (test_query_status_validation() != 0) {
        return 1;
    }

    puts("test_appliance_integration: all tests passed");
    return 0;
}
