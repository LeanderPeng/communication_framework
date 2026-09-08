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

/* 生命周期测试不关心模型变化内容，因此使用一个空通知回调。 */
static void fake_model_changed(void *context,
                               const appliance_model_t *model)
{
    (void)context;
    (void)model;
}

/* 复制每次模型变化通知，验证回调拿到的是已经完整提交的新快照。 */
typedef struct {
    appliance_model_t last_model;
    size_t notify_count;
} model_notification_recorder_t;

static void record_model_changed(void *context,
                                 const appliance_model_t *model)
{
    model_notification_recorder_t *recorder =
        (model_notification_recorder_t *)context;

    recorder->last_model = *model;
    recorder->notify_count += 1u;
}

/* 构造一条字段合法的 STATUS_SNAPSHOT，具体测试再修改关注的字段。 */
static void make_status_event(comm_message_event_t *event,
                              comm_message_event_type_t event_type)
{
    memset(event, 0, sizeof(*event));
    event->type = event_type;
    event->frame.version = COMM_FRAME_VERSION;
    event->frame.type = (event_type == COMM_MESSAGE_EVENT_REPORT_RECEIVED)
                            ? COMM_FRAME_TYPE_REPORT
                            : COMM_FRAME_TYPE_RESPONSE;
    event->frame.sequence =
        (event_type == COMM_MESSAGE_EVENT_REPORT_RECEIVED) ? 0u : 0x1234u;
    event->frame.payload_length = APPLIANCE_STATUS_PAYLOAD_SIZE;
    event->frame.payload[APPLIANCE_STATUS_MESSAGE_ID_OFFSET] =
        APPLIANCE_MESSAGE_STATUS_SNAPSHOT;
    event->frame.payload[APPLIANCE_STATUS_RUN_STATE_OFFSET] =
        (uint8_t)APPLIANCE_RUN_STATE_RUNNING;
    event->frame.payload[APPLIANCE_STATUS_PROGRAM_OFFSET] =
        (uint8_t)APPLIANCE_PROGRAM_COTTON;
    event->frame.payload[APPLIANCE_STATUS_PROGRESS_OFFSET] = 75u;
    event->frame.payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET] = 0x01u;
    event->frame.payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET + 1u] =
        0x2Cu;
    event->frame.payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET] = 1u;
    event->frame.payload[APPLIANCE_STATUS_FAULT_CODE_OFFSET] = 0x00u;
    event->frame.payload[APPLIANCE_STATUS_FAULT_CODE_OFFSET + 1u] = 0x00u;
}

/* 测试中逐字段比较模型，避免依赖结构体填充字节。 */
static int models_are_equal(const appliance_model_t *left,
                            const appliance_model_t *right)
{
    return (left->run_state == right->run_state) &&
           (left->program == right->program) &&
           (left->remaining_minutes == right->remaining_minutes) &&
           (left->fault_code == right->fault_code) &&
           (left->progress_percent == right->progress_percent) &&
           (left->door_locked == right->door_locked);
}

/* 参数错误不能在调用方对象中留下半初始化状态。 */
static int test_init_validation(void)
{
    appliance_manager_t manager;

    memset(&manager, 0xA5, sizeof(manager));

    TEST_CHECK(appliance_manager_init(NULL,
                                      fake_model_changed,
                                      NULL) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_init(&manager, NULL, NULL) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(manager.initialized != 1);

    return 0;
}

/* 成功初始化后，每个模型字段都必须处于文档约定的安全默认值。 */
static int test_init_success(void)
{
    appliance_manager_t manager;
    int context_value = 123;

    memset(&manager, 0xA5, sizeof(manager));

    TEST_CHECK(appliance_manager_init(&manager,
                                      fake_model_changed,
                                      &context_value) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(manager.model.run_state == APPLIANCE_RUN_STATE_OFF);
    TEST_CHECK(manager.model.program == APPLIANCE_PROGRAM_NONE);
    TEST_CHECK(manager.model.remaining_minutes == 0u);
    TEST_CHECK(manager.model.fault_code == 0u);
    TEST_CHECK(manager.model.progress_percent == 0u);
    TEST_CHECK(manager.model.door_locked == 0);
    TEST_CHECK(manager.notify_callback == fake_model_changed);
    TEST_CHECK(manager.notify_context == &context_value);
    TEST_CHECK(manager.initialized == 1);

    return 0;
}

/* reset 只恢复模型，不丢失已经配置好的 UI 适配回调。 */
static int test_reset(void)
{
    appliance_manager_t manager;
    appliance_manager_t uninitialized_manager;
    int context_value = 123;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    TEST_CHECK(appliance_manager_init(&manager,
                                      fake_model_changed,
                                      &context_value) ==
               APPLIANCE_MANAGER_OK);

    manager.model.run_state = APPLIANCE_RUN_STATE_RUNNING;
    manager.model.program = APPLIANCE_PROGRAM_COTTON;
    manager.model.remaining_minutes = 42u;
    manager.model.fault_code = 7u;
    manager.model.progress_percent = 55u;
    manager.model.door_locked = 1;

    TEST_CHECK(appliance_manager_reset(&manager) == APPLIANCE_MANAGER_OK);
    TEST_CHECK(manager.model.run_state == APPLIANCE_RUN_STATE_OFF);
    TEST_CHECK(manager.model.program == APPLIANCE_PROGRAM_NONE);
    TEST_CHECK(manager.model.remaining_minutes == 0u);
    TEST_CHECK(manager.model.fault_code == 0u);
    TEST_CHECK(manager.model.progress_percent == 0u);
    TEST_CHECK(manager.model.door_locked == 0);
    TEST_CHECK(manager.notify_callback == fake_model_changed);
    TEST_CHECK(manager.notify_context == &context_value);
    TEST_CHECK(manager.initialized == 1);

    TEST_CHECK(appliance_manager_reset(NULL) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_reset(&uninitialized_manager) ==
               APPLIANCE_MANAGER_INVALID_STATE);

    return 0;
}

/* get_model 返回副本；修改副本不会绕过 manager 改写内部模型。 */
static int test_get_model(void)
{
    appliance_manager_t manager;
    appliance_manager_t uninitialized_manager;
    appliance_model_t output;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(&output, 0xA5, sizeof(output));
    TEST_CHECK(appliance_manager_init(&manager,
                                      fake_model_changed,
                                      NULL) ==
               APPLIANCE_MANAGER_OK);

    manager.model.run_state = APPLIANCE_RUN_STATE_PAUSED;
    manager.model.program = APPLIANCE_PROGRAM_QUICK;
    manager.model.remaining_minutes = 12u;
    manager.model.progress_percent = 75u;
    manager.model.door_locked = 1;

    TEST_CHECK(appliance_manager_get_model(&manager, &output) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(output.run_state == APPLIANCE_RUN_STATE_PAUSED);
    TEST_CHECK(output.program == APPLIANCE_PROGRAM_QUICK);
    TEST_CHECK(output.remaining_minutes == 12u);
    TEST_CHECK(output.fault_code == 0u);
    TEST_CHECK(output.progress_percent == 75u);
    TEST_CHECK(output.door_locked == 1);

    output.progress_percent = 1u;
    TEST_CHECK(manager.model.progress_percent == 75u);

    /* 非法模型不能泄漏给上层，但 reset 可以把它修复为默认状态。 */
    manager.model.progress_percent = 101u;
    TEST_CHECK(appliance_manager_get_model(&manager, &output) ==
               APPLIANCE_MANAGER_INVALID_STATE);
    TEST_CHECK(appliance_manager_reset(&manager) == APPLIANCE_MANAGER_OK);
    TEST_CHECK(appliance_manager_get_model(&manager, &output) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(output.progress_percent == 0u);

    TEST_CHECK(appliance_manager_get_model(NULL, &output) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_get_model(&manager, NULL) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_get_model(&uninitialized_manager,
                                           &output) ==
               APPLIANCE_MANAGER_INVALID_STATE);

    return 0;
}

/*
 * 验证 REPORT 和 RESPONSE 都能携带状态快照，大端字段正确解码，并且只有模型
 * 实际发生变化时才通知 UI 适配层。
 */
static int test_handle_status_snapshot_success(void)
{
    appliance_manager_t manager;
    appliance_model_t output;
    model_notification_recorder_t recorder;
    comm_message_event_t event;

    memset(&recorder, 0, sizeof(recorder));
    TEST_CHECK(appliance_manager_init(&manager,
                                      record_model_changed,
                                      &recorder) ==
               APPLIANCE_MANAGER_OK);

    make_status_event(&event, COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(appliance_manager_get_model(&manager, &output) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(output.run_state == APPLIANCE_RUN_STATE_RUNNING);
    TEST_CHECK(output.program == APPLIANCE_PROGRAM_COTTON);
    TEST_CHECK(output.progress_percent == 75u);
    TEST_CHECK(output.remaining_minutes == 300u);
    TEST_CHECK(output.door_locked == 1);
    TEST_CHECK(output.fault_code == 0u);
    TEST_CHECK(recorder.notify_count == 1u);
    TEST_CHECK(models_are_equal(&recorder.last_model, &output));

    /* 完全相同的周期上报不需要让 UI 重复刷新。 */
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(recorder.notify_count == 1u);

    /* 查询请求的 RESPONSE 同样可以带状态；改变一个字段后产生一次新通知。 */
    event.type = COMM_MESSAGE_EVENT_RESPONSE_RECEIVED;
    event.frame.type = COMM_FRAME_TYPE_RESPONSE;
    event.frame.sequence = 0x1234u;
    event.frame.payload[APPLIANCE_STATUS_PROGRESS_OFFSET] = 76u;
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(recorder.notify_count == 2u);
    TEST_CHECK(recorder.last_model.progress_percent == 76u);
    TEST_CHECK(manager.model.progress_percent == 76u);

    return 0;
}

/* 任一字段或长度非法时，旧模型和通知次数必须完整保持。 */
static int test_handle_status_snapshot_is_transactional(void)
{
    appliance_manager_t manager;
    appliance_model_t previous_model;
    appliance_model_t output;
    model_notification_recorder_t recorder;
    comm_message_event_t valid_event;
    comm_message_event_t invalid_event;

    memset(&recorder, 0, sizeof(recorder));
    TEST_CHECK(appliance_manager_init(&manager,
                                      record_model_changed,
                                      &recorder) ==
               APPLIANCE_MANAGER_OK);
    make_status_event(&valid_event, COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &valid_event) ==
               APPLIANCE_MANAGER_OK);
    previous_model = manager.model;
    TEST_CHECK(recorder.notify_count == 1u);

    invalid_event = valid_event;
    invalid_event.frame.payload_length = 0u;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    invalid_event = valid_event;
    invalid_event.frame.payload_length =
        (uint16_t)(APPLIANCE_STATUS_PAYLOAD_SIZE - 1u);
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    invalid_event = valid_event;
    invalid_event.frame.payload[APPLIANCE_STATUS_MESSAGE_ID_OFFSET] = 0xFFu;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_UNSUPPORTED_MESSAGE);

    invalid_event = valid_event;
    invalid_event.frame.payload[APPLIANCE_STATUS_RUN_STATE_OFFSET] = 0xFFu;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    invalid_event = valid_event;
    invalid_event.frame.payload[APPLIANCE_STATUS_PROGRAM_OFFSET] = 0xFFu;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    invalid_event = valid_event;
    invalid_event.frame.payload[APPLIANCE_STATUS_PROGRESS_OFFSET] = 101u;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    invalid_event = valid_event;
    invalid_event.frame.payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET] = 2u;
    TEST_CHECK(appliance_manager_handle_message_event(&manager,
                                                      &invalid_event) ==
               APPLIANCE_MANAGER_MALFORMED_PAYLOAD);

    TEST_CHECK(appliance_manager_get_model(&manager, &output) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(models_are_equal(&output, &previous_model));
    TEST_CHECK(recorder.notify_count == 1u);

    return 0;
}

/* 验证参数、manager 状态和非状态类消息事件都被明确拒绝。 */
static int test_handle_message_event_validation(void)
{
    appliance_manager_t manager;
    appliance_manager_t uninitialized_manager;
    model_notification_recorder_t recorder;
    comm_message_event_t event;

    memset(&uninitialized_manager, 0, sizeof(uninitialized_manager));
    memset(&recorder, 0, sizeof(recorder));
    make_status_event(&event, COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    TEST_CHECK(appliance_manager_init(&manager,
                                      record_model_changed,
                                      &recorder) ==
               APPLIANCE_MANAGER_OK);

    TEST_CHECK(appliance_manager_handle_message_event(NULL, &event) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_handle_message_event(&manager, NULL) ==
               APPLIANCE_MANAGER_NULL_ARGUMENT);
    TEST_CHECK(appliance_manager_handle_message_event(&uninitialized_manager,
                                                      &event) ==
               APPLIANCE_MANAGER_INVALID_STATE);

    event.type = COMM_MESSAGE_EVENT_REQUEST_RECEIVED;
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_UNSUPPORTED_EVENT);
    TEST_CHECK(recorder.notify_count == 0u);

    /* 完整状态快照允许覆盖并修复当前损坏的业务模型。 */
    manager.model.progress_percent = 101u;
    make_status_event(&event, COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_OK);
    TEST_CHECK(manager.model.progress_percent == 75u);
    TEST_CHECK(recorder.notify_count == 1u);

    manager.notify_callback = NULL;
    TEST_CHECK(appliance_manager_handle_message_event(&manager, &event) ==
               APPLIANCE_MANAGER_INVALID_STATE);
    TEST_CHECK(recorder.notify_count == 1u);

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
    if (test_get_model() != 0) {
        return 1;
    }
    if (test_handle_status_snapshot_success() != 0) {
        return 1;
    }
    if (test_handle_status_snapshot_is_transactional() != 0) {
        return 1;
    }
    if (test_handle_message_event_validation() != 0) {
        return 1;
    }

    puts("test_appliance_manager: all tests passed");
    return 0;
}
