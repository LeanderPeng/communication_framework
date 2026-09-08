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

/* 本阶段只验证生命周期，模型变化通知会在后续业务消息解析测试中记录。 */
static void fake_model_changed(void *context,
                               const appliance_model_t *model)
{
    (void)context;
    (void)model;
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

    puts("test_appliance_manager: all tests passed");
    return 0;
}
