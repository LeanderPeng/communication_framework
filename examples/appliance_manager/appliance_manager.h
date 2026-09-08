#ifndef APPLIANCE_MANAGER_H
#define APPLIANCE_MANAGER_H

#include <stdint.h>

#include "appliance_protocol.h"
#include "comm_message_manager.h"

/* Appliance Manager 对外接口的执行结果，成功固定为 0。 */
typedef enum {
    APPLIANCE_MANAGER_OK = 0,
    APPLIANCE_MANAGER_NULL_ARGUMENT,
    APPLIANCE_MANAGER_INVALID_STATE,
    APPLIANCE_MANAGER_UNSUPPORTED_EVENT,
    APPLIANCE_MANAGER_UNSUPPORTED_MESSAGE,
    APPLIANCE_MANAGER_MALFORMED_PAYLOAD
} appliance_manager_result_t;

/*
 * 示例洗衣机的运行状态。
 *
 * 这里描述的是业务含义，不是通信帧中的 type。通信层只知道 REQUEST、
 * RESPONSE 等通用消息类型；Appliance Manager 才知道某段 payload 表示关机、
 * 运行或故障。把两类枚举分开，可以避免核心通信框架依赖具体产品业务。
 */
typedef enum {
    APPLIANCE_RUN_STATE_OFF = 0,
    APPLIANCE_RUN_STATE_IDLE,
    APPLIANCE_RUN_STATE_RUNNING,
    APPLIANCE_RUN_STATE_PAUSED,
    APPLIANCE_RUN_STATE_COMPLETED,
    APPLIANCE_RUN_STATE_FAULT
} appliance_run_state_t;

/* 示例程序类型；NONE 表示当前没有选择洗涤程序。 */
typedef enum {
    APPLIANCE_PROGRAM_NONE = 0,
    APPLIANCE_PROGRAM_COTTON,
    APPLIANCE_PROGRAM_QUICK,
    APPLIANCE_PROGRAM_DELICATE
} appliance_program_t;

/*
 * 业务层持有的洗衣机状态快照。
 *
 * 该结构体同样不是线路字节布局，不能直接 memcpy 到通信 payload。业务字节
 * 布局由 appliance_protocol.h 单独定义，解析时逐字段读取。这样既不受结构体
 * 填充和 CPU 字节序影响，也不会把 UI 数据表示误当成设备通信协议。
 *
 * progress_percent 的有效范围是 0..100；door_locked 只能是 0 或 1；
 * fault_code 为 0 表示当前没有故障。字段一致性由 Appliance Manager 维护。
 */
typedef struct {
    appliance_run_state_t run_state;
    appliance_program_t program;
    uint16_t remaining_minutes;
    uint16_t fault_code;
    uint8_t progress_percent;
    int door_locked;
} appliance_model_t;

/*
 * 模型变化通知在 Appliance Manager 的当前调用栈中同步执行。
 *
 * model 指向 manager 内部的只读快照，只在回调期间保证有效。真正的 UI 适配层
 * 应复制需要的数据并投递到 UI 所属队列，不能在通信任务中直接调用 LVGL，
 * 也不应从回调内部再次进入同一个 Appliance Manager。
 */
typedef void (*appliance_model_changed_fn)(
    void *context,
    const appliance_model_t *model);

/*
 * Appliance Manager 的运行状态。
 *
 * manager 不分配内存，也不拥有 notify_context。和 comm_message_manager 一样，
 * 当前设计由一个固定任务串行调用，因此内部模型本身不加锁。
 */
typedef struct {
    appliance_model_t model;
    appliance_model_changed_fn notify_callback;
    void *notify_context;
    int initialized;
} appliance_manager_t;

/*
 * 初始化业务管理器，并把模型设置为确定的安全状态：关机、无程序、无进度、
 * 无剩余时间、无故障且门锁未锁。notify_callback 不能为空，context 允许为空。
 */
appliance_manager_result_t appliance_manager_init(
    appliance_manager_t *manager,
    appliance_model_changed_fn notify_callback,
    void *notify_context);

/*
 * 把模型恢复到与初始化相同的安全状态，但保留通知回调和 context。
 * reset 只重置内部状态，不主动产生模型变化通知。
 */
appliance_manager_result_t appliance_manager_reset(
    appliance_manager_t *manager);

/*
 * 把当前模型复制到调用方提供的 output 中。
 * 使用复制而不是暴露可写指针，防止上层绕过 manager 直接破坏模型一致性。
 */
appliance_manager_result_t appliance_manager_get_model(
    const appliance_manager_t *manager,
    appliance_model_t *output);

/*
 * 处理 comm_message_manager 已经分类完成的一次事件。
 *
 * 当前作为屏端示例，只接受 REPORT_RECEIVED 和 RESPONSE_RECEIVED 中携带的
 * STATUS_SNAPSHOT。收到完整快照时先解码到局部临时模型，全部字段通过校验后
 * 才一次性替换当前模型；失败不会留下“前几个字段已更新”的半新半旧状态。
 *
 * 模型实际发生变化时同步调用 notify_callback；重复收到内容完全相同的快照
 * 仍返回 OK，但不重复通知 UI 适配层。
 */
appliance_manager_result_t appliance_manager_handle_message_event(
    appliance_manager_t *manager,
    const comm_message_event_t *event);

#endif /* APPLIANCE_MANAGER_H */
