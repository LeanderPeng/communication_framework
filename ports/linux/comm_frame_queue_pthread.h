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
 * pthread 层的可选统计，全部更新、读取和清零都受 queue->mutex 保护。
 *
 * frames_pushed / frames_popped：核心队列实际完成复制的帧数；后续 signal 或
 * unlock 若失败，已经完成的帧操作和统计不回滚。
 * push_full_encounters / pop_empty_encounters：未关闭队列上，一次有效调用
 * 首次发现满/空的次数，每次调用最多一次，不是核心 FULL 返回次数。
 * push_wait_count / pop_wait_count：首次进入条件变量等待的调用数；不计算
 * mutex 竞争，也不因虚假唤醒或多次等待重复增加。零等待失败不计入此项。
 * push_timeout_count / pop_timeout_count：因满/空进入 TIMEOUT 处理的次数，
 * 包括 timeout_ms=0 的立即失败。关闭唤醒不计超时；解锁失败仍优先报系统错误。
 * peak_size：统计期间最大积压帧数，单位是帧。
 *
 * 所有 uint64_t 计数均饱和递增。统计按事件发生时刻归属周期：若等待期间
 * reset_stats，先前的遇满/等待记录被清除，之后的成功/超时记入新周期。
 */
typedef struct {
    uint64_t frames_pushed;
    uint64_t frames_popped;
    uint64_t push_full_encounters;
    uint64_t pop_empty_encounters;
    uint64_t push_wait_count;
    uint64_t pop_wait_count;
    uint64_t push_timeout_count;
    uint64_t pop_timeout_count;
    size_t peak_size;
} comm_frame_queue_pthread_stats_t;

/*
 * 给非阻塞 comm_frame_queue_t 增加 Linux pthread 同步能力。
 *
 * mutex       保护 core、closed 和 stats，不能绕过本包装层并发访问它们。
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
    comm_frame_queue_pthread_stats_t *stats; /* 可选外部存储，NULL 表示不统计 */
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
 * 初始化并启用统计。stats_storage 必须非空，由调用方持有，生命周期必须覆盖
 * init 成功到 destroy 完成，且不能与 queue 或帧 storage 重叠，也不能被其他
 * 队列共用。初始化成功时清零统计，失败时不修改 stats_storage。
 *
 * 初始化后不允许直接读写统计存储，使用 get_stats/reset_stats。所有普通
 * push/pop（包括经由 Channel 的调用）自动记录，不需要改调用处。
 * 原 init 仍禁用统计；queue 只增加一个指针，不强制分配整份计数器。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_init_with_stats(
    comm_frame_queue_pthread_t *queue,
    comm_frame_t *storage,
    size_t capacity,
    comm_frame_queue_pthread_stats_t *stats_storage);

/*
 * 在锁内复制一致的统计快照；除 OK 外不修改 output。
 * output 必须是调用方独立的缓冲区，不得与队列、帧存储或统计存储重叠。
 * 未初始化、未启用统计或核心状态无效时返回 INVALID_STATE。
 * close 后仍可读取；不能与 init/destroy 并发调用。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_get_stats(
    comm_frame_queue_pthread_t *queue,
    comm_frame_queue_pthread_stats_t *output);

/*
 * 在锁内清零累计值，peak_size 从当前队列深度开始。不清空帧、不重新打开
 * 已关闭的队列，也不唤醒等待者。状态约束与 get_stats 相同。
 */
comm_frame_queue_pthread_result_t comm_frame_queue_pthread_reset_stats(
    comm_frame_queue_pthread_t *queue);

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
 * 从调用开始计算的最大等待毫秒数。有限等待使用单调时钟计算，不受
 * 系统日期和时间被校准的影响。内部 mutex 的调度可能使函数实际返回
 * 时间略晚于指定截止时间。timeout_ms 为 0 且队列已满时返回 TIMEOUT。
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
