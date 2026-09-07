#ifndef COMM_FRAME_QUEUE_PTHREAD_H
#define COMM_FRAME_QUEUE_PTHREAD_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "comm_frame_queue.h"

/* UINT32_MAX 表示不设置截止时间，一直等待到条件满足或队列关闭。 */
#define COMM_FRAME_QUEUE_PTHREAD_WAIT_FOREVER UINT32_MAX

/* pthread 帧队列接口的执行结果，成功固定为 0。 */
typedef enum {
    COMM_FRAME_QUEUE_PTHREAD_OK = 0,
    COMM_FRAME_QUEUE_PTHREAD_NULL_ARGUMENT,
    COMM_FRAME_QUEUE_PTHREAD_INVALID_CAPACITY,
    COMM_FRAME_QUEUE_PTHREAD_INVALID_STATE,
    COMM_FRAME_QUEUE_PTHREAD_TIMEOUT,
    COMM_FRAME_QUEUE_PTHREAD_CLOSED,
    COMM_FRAME_QUEUE_PTHREAD_SYSTEM_ERROR
} comm_frame_queue_pthread_result_t;

/*
 * 给非阻塞 comm_frame_queue_t 增加 Linux pthread 同步能力。
 *
 * mutex       保护 core 和 closed，任何线程都不能绕过本包装层直接访问它们。
 * not_empty   消费者在队列为空时等待，成功入队后由生产者唤醒。
 * not_full    生产者在队列已满时等待，成功出队后由消费者唤醒。
 * initialized 标记 pthread 对象是否已经完整初始化。
 * closed      标记队列是否已经进入关闭状态。
 *
 * 所有字段由本模块维护，调用方不应直接修改。
 */
typedef struct {
    comm_frame_queue_t core;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int initialized;
    int closed;
} comm_frame_queue_pthread_t;

/*
 * 使用调用方提供的帧数组初始化 pthread 队列。
 * storage 的生命周期必须覆盖从 init 成功到 destroy 完成的整个周期。
 * 同一个 queue 成功初始化后，必须先 destroy 才能再次初始化。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_init(
    comm_frame_queue_pthread_t *queue,
    comm_frame_t *storage,
    size_t capacity);

/*
 * 关闭队列并唤醒所有等待线程，此操作可重复调用。
 * 关闭后 push 返回 CLOSED；pop 可以继续取出关闭前已经入队的帧，
 * 队列排空后返回 CLOSED。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_close(
    comm_frame_queue_pthread_t *queue);

/*
 * 等待可用槽位并复制一帧到队尾。
 * timeout_ms 为 0 时立即尝试；为 WAIT_FOREVER 时永久等待；其他值表示
 * 从调用开始计算的最大等待毫秒数。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_push(
    comm_frame_queue_pthread_t *queue,
    const comm_frame_t *frame,
    uint32_t timeout_ms);

/*
 * 等待可用帧并将队首帧复制到 frame。
 * timeout_ms 的含义与 push 相同。除成功外，不修改 frame。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_pop(
    comm_frame_queue_pthread_t *queue,
    comm_frame_t *frame,
    uint32_t timeout_ms);

/*
 * 释放 mutex 和条件变量持有的系统资源，不释放 storage。
 * 调用前必须保证没有其他线程正在使用或等待此队列；通常应先 close，
 * 等待相关线程退出，再调用 destroy。destroy 后可以重新初始化。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_destroy(
    comm_frame_queue_pthread_t *queue);

#endif /* COMM_FRAME_QUEUE_PTHREAD_H */
