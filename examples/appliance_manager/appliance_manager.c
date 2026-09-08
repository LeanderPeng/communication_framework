#include "appliance_manager.h"

#include <stddef.h>

/* 把模型写成一个确定的默认快照，避免依赖枚举或结构体的隐式清零假设。 */
static void appliance_manager_set_default_model(appliance_model_t *model)
{
    model->run_state = APPLIANCE_RUN_STATE_OFF;
    model->program = APPLIANCE_PROGRAM_NONE;
    model->remaining_minutes = 0u;
    model->fault_code = 0u;
    model->progress_percent = 0u;
    model->door_locked = 0;
}

/* 检查 reset 可以依赖、并且能够保留下来的长期配置是否仍然完整。 */
static int appliance_manager_has_valid_configuration(
    const appliance_manager_t *manager)
{
    return (manager != NULL) &&
           (manager->initialized == 1) &&
           (manager->notify_callback != NULL);
}

/*
 * 检查一个模型是否满足公开约束。
 * 使用 switch 明确列出支持的枚举值，避免把未来新增值或损坏值误当成有效状态。
 */
static int appliance_model_is_valid(const appliance_model_t *model)
{
    switch (model->run_state) {
        case APPLIANCE_RUN_STATE_OFF:
        case APPLIANCE_RUN_STATE_IDLE:
        case APPLIANCE_RUN_STATE_RUNNING:
        case APPLIANCE_RUN_STATE_PAUSED:
        case APPLIANCE_RUN_STATE_COMPLETED:
        case APPLIANCE_RUN_STATE_FAULT:
            break;

        default:
            return 0;
    }

    switch (model->program) {
        case APPLIANCE_PROGRAM_NONE:
        case APPLIANCE_PROGRAM_COTTON:
        case APPLIANCE_PROGRAM_QUICK:
        case APPLIANCE_PROGRAM_DELICATE:
            break;

        default:
            return 0;
    }

    return (model->progress_percent <= 100u) &&
           ((model->door_locked == 0) || (model->door_locked == 1));
}

/* 检查 manager 长期配置和当前模型是否都可安全交给上层读取。 */
static int appliance_manager_has_valid_model(
    const appliance_manager_t *manager)
{
    return appliance_manager_has_valid_configuration(manager) &&
           appliance_model_is_valid(&manager->model);
}

/* 从两个连续字节中读取大端序无符号 16 位值。 */
static uint16_t appliance_read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8u) | (uint16_t)input[1]);
}

/* 逐字段比较业务模型，避免比较结构体中可能存在的填充字节。 */
static int appliance_models_are_equal(const appliance_model_t *left,
                                      const appliance_model_t *right)
{
    return (left->run_state == right->run_state) &&
           (left->program == right->program) &&
           (left->remaining_minutes == right->remaining_minutes) &&
           (left->fault_code == right->fault_code) &&
           (left->progress_percent == right->progress_percent) &&
           (left->door_locked == right->door_locked);
}

appliance_manager_result_t appliance_manager_init(
    appliance_manager_t *manager,
    appliance_model_changed_fn notify_callback,
    void *notify_context)
{
    /*
     * context 可以为空，因为无状态 UI 适配器可能不需要外部对象。manager 和
     * callback 是完成基本职责所必需的，参数错误时不能写入部分初始化状态。
     */
    if ((manager == NULL) || (notify_callback == NULL)) {
        return APPLIANCE_MANAGER_NULL_ARGUMENT;
    }

    appliance_manager_set_default_model(&manager->model);
    manager->notify_callback = notify_callback;
    manager->notify_context = notify_context;

    /* 所有模型和外部地址准备完成后，最后发布 initialized 状态。 */
    manager->initialized = 1;

    return APPLIANCE_MANAGER_OK;
}

appliance_manager_result_t appliance_manager_reset(
    appliance_manager_t *manager)
{
    if (manager == NULL) {
        return APPLIANCE_MANAGER_NULL_ARGUMENT;
    }

    /* reset 可以修复模型，所以这里只要求不可重建的长期配置仍然有效。 */
    if (!appliance_manager_has_valid_configuration(manager)) {
        return APPLIANCE_MANAGER_INVALID_STATE;
    }

    appliance_manager_set_default_model(&manager->model);
    return APPLIANCE_MANAGER_OK;
}

appliance_manager_result_t appliance_manager_get_model(
    const appliance_manager_t *manager,
    appliance_model_t *output)
{
    if ((manager == NULL) || (output == NULL)) {
        return APPLIANCE_MANAGER_NULL_ARGUMENT;
    }

    if (!appliance_manager_has_valid_model(manager)) {
        return APPLIANCE_MANAGER_INVALID_STATE;
    }

    *output = manager->model;
    return APPLIANCE_MANAGER_OK;
}

appliance_manager_result_t appliance_manager_handle_message_event(
    appliance_manager_t *manager,
    const comm_message_event_t *event)
{
    appliance_model_t candidate;
    const uint8_t *payload;

    if ((manager == NULL) || (event == NULL)) {
        return APPLIANCE_MANAGER_NULL_ARGUMENT;
    }

    /*
     * 完整状态快照能够修复旧模型，因此这里只验证无法从消息中重建的长期配置，
     * 不要求 manager->model 当前有效。
     */
    if (!appliance_manager_has_valid_configuration(manager)) {
        return APPLIANCE_MANAGER_INVALID_STATE;
    }

    /*
     * 本示例位于屏端：主动 REPORT 和查询请求对应的 RESPONSE 都可能携带状态。
     * REQUEST、ERROR、超时和未匹配回复有各自业务含义，暂不当作状态快照处理。
     */
    if ((event->type != COMM_MESSAGE_EVENT_REPORT_RECEIVED) &&
        (event->type != COMM_MESSAGE_EVENT_RESPONSE_RECEIVED)) {
        return APPLIANCE_MANAGER_UNSUPPORTED_EVENT;
    }

    if (event->frame.payload_length == 0u) {
        return APPLIANCE_MANAGER_MALFORMED_PAYLOAD;
    }

    payload = event->frame.payload;
    if (payload[APPLIANCE_STATUS_MESSAGE_ID_OFFSET] !=
        APPLIANCE_MESSAGE_STATUS_SNAPSHOT) {
        return APPLIANCE_MANAGER_UNSUPPORTED_MESSAGE;
    }

    if (event->frame.payload_length != APPLIANCE_STATUS_PAYLOAD_SIZE) {
        return APPLIANCE_MANAGER_MALFORMED_PAYLOAD;
    }

    /*
     * 不能把 payload 强制转换成 appliance_model_t 指针：结构体可能含有填充，
     * enum 和 int 的大小由编译器决定，16 位字段还会受到 CPU 字节序影响。
     * 因此按照协议偏移逐字段读取，并显式转换协议值。
     */
    candidate.run_state =
        (appliance_run_state_t)payload[APPLIANCE_STATUS_RUN_STATE_OFFSET];
    candidate.program =
        (appliance_program_t)payload[APPLIANCE_STATUS_PROGRAM_OFFSET];
    candidate.progress_percent =
        payload[APPLIANCE_STATUS_PROGRESS_OFFSET];
    candidate.remaining_minutes = appliance_read_u16_be(
        &payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET]);
    candidate.door_locked =
        (int)payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET];
    candidate.fault_code = appliance_read_u16_be(
        &payload[APPLIANCE_STATUS_FAULT_CODE_OFFSET]);

    /*
     * 先验证完整临时模型，再决定是否提交。任何一个字段非法时，manager->model
     * 尚未发生变化，避免 UI 看到由新旧字段拼成的中间状态。
     */
    if (!appliance_model_is_valid(&candidate)) {
        return APPLIANCE_MANAGER_MALFORMED_PAYLOAD;
    }

    if (appliance_models_are_equal(&manager->model, &candidate)) {
        return APPLIANCE_MANAGER_OK;
    }

    manager->model = candidate;
    manager->notify_callback(manager->notify_context, &manager->model);

    return APPLIANCE_MANAGER_OK;
}
