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
 * 检查即将交给上层的模型是否满足公开约束。
 * 使用 switch 明确列出支持的枚举值，避免把未来新增值或损坏值误当成有效状态。
 */
static int appliance_manager_has_valid_model(
    const appliance_manager_t *manager)
{
    if (!appliance_manager_has_valid_configuration(manager)) {
        return 0;
    }

    switch (manager->model.run_state) {
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

    switch (manager->model.program) {
        case APPLIANCE_PROGRAM_NONE:
        case APPLIANCE_PROGRAM_COTTON:
        case APPLIANCE_PROGRAM_QUICK:
        case APPLIANCE_PROGRAM_DELICATE:
            break;

        default:
            return 0;
    }

    return (manager->model.progress_percent <= 100u) &&
           ((manager->model.door_locked == 0) ||
            (manager->model.door_locked == 1));
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
