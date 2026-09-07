#ifndef COMM_FRAME_CHANNEL_H
#define COMM_FRAME_CHANNEL_H

#include <stdint.h>

#include "comm_frame.h"

/* 与具体操作系统无关的永久等待值。 */
#define COMM_FRAME_CHANNEL_WAIT_FOREVER UINT32_MAX

/*
 * 帧通道统一返回值。
 *
 * 平台适配层负责把 pthread、RTOS 或其他后端的返回值转换到这里。
 * BACKEND_ERROR 表示底层同步对象、队列状态或系统调用发生异常；这种错误
 * 不等同于正常的队列超时或关闭流程。
 */
typedef enum {
    COMM_FRAME_CHANNEL_OK = 0,
    COMM_FRAME_CHANNEL_NULL_ARGUMENT,
    COMM_FRAME_CHANNEL_INVALID_STATE,
    COMM_FRAME_CHANNEL_TIMEOUT,
    COMM_FRAME_CHANNEL_CLOSED,
    COMM_FRAME_CHANNEL_BACKEND_ERROR
} comm_frame_channel_result_t;

/* 向平台队列投递一帧的函数类型。 */
typedef comm_frame_channel_result_t (*comm_frame_channel_push_fn)(
    void *context,
    const comm_frame_t *frame,
    uint32_t timeout_ms);

/* 从平台队列接收一帧的函数类型。 */
typedef comm_frame_channel_result_t (*comm_frame_channel_pop_fn)(
    void *context,
    comm_frame_t *frame,
    uint32_t timeout_ms);

/* 关闭平台队列并唤醒等待任务的函数类型。 */
typedef comm_frame_channel_result_t (*comm_frame_channel_close_fn)(
    void *context);

/*
 * 对具体平台帧队列的非拥有型抽象视图。
 *
 * context 保存真实后端对象的地址。例如 Linux 端可以指向一个
 * comm_frame_queue_pthread_t，RTOS 端则可以指向包含系统队列句柄的对象。
 * push/pop/close 保存该后端对应的适配函数。
 *
 * channel 不申请、不释放，也不复制 context 指向的对象。后端对象必须先于
 * channel 初始化，并且生命周期必须长于 channel 的全部使用过程。channel
 * 是否支持多线程安全访问，取决于所绑定后端的实现。
 *
 * 所有字段由 comm_frame_channel_bind 填写，调用方不应直接修改。
 */
typedef struct {
    void *context;
    comm_frame_channel_push_fn push;
    comm_frame_channel_pop_fn pop;
    comm_frame_channel_close_fn close;
} comm_frame_channel_t;

/*
 * 将 channel 绑定到一个已经初始化完成的平台后端。
 * context 和三个函数指针都不能为空；失败时不修改 channel。
 * 此操作只保存地址，不初始化后端，也不取得后端资源的所有权。
 */
comm_frame_channel_result_t comm_frame_channel_bind(
    comm_frame_channel_t *channel,
    void *context,
    comm_frame_channel_push_fn push,
    comm_frame_channel_pop_fn pop,
    comm_frame_channel_close_fn close);

/*
 * 通过已绑定的后端投递一帧。
 * timeout_ms 为 0 时立即尝试，为 WAIT_FOREVER 时永久等待，其他值表示
 * 最大等待毫秒数。具体计时精度由后端平台决定。
 */
comm_frame_channel_result_t comm_frame_channel_push(
    const comm_frame_channel_t *channel,
    const comm_frame_t *frame,
    uint32_t timeout_ms);

/*
 * 通过已绑定的后端接收一帧。
 * timeout_ms 的含义与 push 相同。除返回 OK 外，不应修改 frame。
 */
comm_frame_channel_result_t comm_frame_channel_pop(
    const comm_frame_channel_t *channel,
    comm_frame_t *frame,
    uint32_t timeout_ms);

/*
 * 请求后端进入关闭状态并唤醒等待者。
 * 统一语义为：关闭后拒绝新的 push，pop 可以排空关闭前已经进入后端的帧，
 * 后端排空后返回 CLOSED。此函数不销毁后端，也不释放 context。
 */
comm_frame_channel_result_t comm_frame_channel_close(
    const comm_frame_channel_t *channel);

#endif /* COMM_FRAME_CHANNEL_H */
