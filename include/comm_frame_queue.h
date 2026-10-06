#ifndef COMM_FRAME_QUEUE_H
#define COMM_FRAME_QUEUE_H

#include <stddef.h>

#include "comm_frame.h"

/* 帧队列接口的执行结果，成功固定为 0。 */
typedef enum {
    COMM_FRAME_QUEUE_OK = 0,
    COMM_FRAME_QUEUE_NULL_ARGUMENT,
    COMM_FRAME_QUEUE_INVALID_CAPACITY,
    COMM_FRAME_QUEUE_INVALID_STATE,
    COMM_FRAME_QUEUE_FULL,
    COMM_FRAME_QUEUE_EMPTY
} comm_frame_queue_result_t;

/*
 * 保存完整逻辑帧的固定容量先进先出队列。
 *
 * storage 指向调用方提供的 comm_frame_t 数组，队列不申请也不释放内存。
 * used 用于区分 read_index == write_index 时的“空”和“满”两种状态，
 * 因此 capacity 个槽位可以全部使用。
 *
 * push 和 pop 都复制完整的 comm_frame_t。入队成功后，调用方可以立即
 * 修改原帧；出队成功后，输出帧也不再依赖队列中的存储内容。
 *
 * 本模块只提供非阻塞核心操作，不提供线程同步、等待或超时。多个任务
 * 同时访问时，应由平台适配层在这些接口外部完成互斥和任务唤醒。
 * 所有字段由本模块维护，调用方不应直接修改。
 */
typedef struct {
    comm_frame_t *storage; /* 调用方拥有的帧存储数组 */
    size_t capacity;       /* storage 中的帧槽位总数 */
    size_t read_index;     /* 下一次出队的帧槽位 */
    size_t write_index;    /* 下一次入队的帧槽位 */
    size_t used;           /* 当前保存的帧数量 */
} comm_frame_queue_t;

/*
 * 核心帧队列的可选诊断统计，由调用方持有，不增加 queue 对象的内存占用。
 *
 * frames_pushed   成功复制进队列的帧数。
 * frames_popped   成功从队列取出的帧数；reset 清空队列不算出队。
 * push_full_count 有效 push 因队列已满而返回 FULL 的次数，不等于丢帧数，
 *                 调用方稍后可能再次投递同一帧。
 * peak_size       统计期间观察到的最大积压帧数，单位是帧槽位，不是字节。
 *
 * 三个累计计数器到达 UINT64_MAX 后保持饱和。首次使用应调用 stats_reset，
 * 从当前队列深度开始统计。一个统计对象应始终对应同一个队列，并在该统计
 * 周期内对全部 push/pop 使用带统计接口；混用原接口会漏记操作和峰值。
 *
 * stats 不得与 queue、storage 或输入/输出帧重叠。统计本身不加锁，跨任务
 * 读写时必须和队列使用相同的外部同步机制；不能无锁读取正在更新的计数器。
 */
typedef struct {
    uint64_t frames_pushed;
    uint64_t frames_popped;
    uint64_t push_full_count;
    size_t peak_size;
} comm_frame_queue_stats_t;

/*
 * 开始一个新的统计周期：累计计数清零，peak_size 设为 queue 当前帧数。
 * 不移除已有帧，不修改队列。参数或队列状态无效时不修改 stats。
 * comm_frame_queue_reset 只清空队列，不会自动清除这个外部统计对象。
 */
comm_frame_queue_result_t comm_frame_queue_stats_reset(
    const comm_frame_queue_t *queue,
    comm_frame_queue_stats_t *stats);

/*
 * 使用调用方提供的固定帧数组初始化队列。
 * storage 不能为空且 capacity 必须大于 0；失败时不修改 queue。
 */
comm_frame_queue_result_t comm_frame_queue_init(comm_frame_queue_t *queue,
                                                comm_frame_t *storage,
                                                size_t capacity);

/* 清空已有帧，但不改变存储数组地址和容量。 */
comm_frame_queue_result_t comm_frame_queue_reset(comm_frame_queue_t *queue);

/* 返回当前帧数量；参数为空或状态无效时返回 0。 */
size_t comm_frame_queue_size(const comm_frame_queue_t *queue);

/* 返回当前剩余槽位数；参数为空或状态无效时返回 0。 */
size_t comm_frame_queue_free_space(const comm_frame_queue_t *queue);

/*
 * 将 frame 的完整副本加入队尾。
 * 队列已满时不修改任何队列状态，并返回 COMM_FRAME_QUEUE_FULL。
 * frame 占用的内存区域不能与队列的 storage 重叠。
 */
comm_frame_queue_result_t comm_frame_queue_push(comm_frame_queue_t *queue,
                                                const comm_frame_t *frame);

/*
 * 将队首帧复制到 frame 并移出队列。
 * 队列为空时不修改 frame 或队列状态，并返回 COMM_FRAME_QUEUE_EMPTY。
 * frame 占用的内存区域不能与队列的 storage 重叠。
 */
comm_frame_queue_result_t comm_frame_queue_pop(comm_frame_queue_t *queue,
                                               comm_frame_t *frame);

/*
 * 与原 push/pop 共用实现并保持相同的帧复制、FIFO 和满/空返回语义。
 * stats 必须非空且已初始化；参数或队列状态错误不修改统计。
 * push 成功时更新入队数和峰值；FULL 只增加满队列拒绝次数。
 * pop 成功时增加出队数；EMPTY 不增加计数。有效操作也会观察操作前深度。
 *
 * 这里统计的是非阻塞核心操作。pthread 包装层会先等待，再调用核心队列，
 * 因此核心 FULL 次数不能替代 pthread 的遇满、等待或最终超时次数。
 */
comm_frame_queue_result_t comm_frame_queue_push_with_stats(
    comm_frame_queue_t *queue,
    const comm_frame_t *frame,
    comm_frame_queue_stats_t *stats);

comm_frame_queue_result_t comm_frame_queue_pop_with_stats(
    comm_frame_queue_t *queue,
    comm_frame_t *frame,
    comm_frame_queue_stats_t *stats);

#endif /* COMM_FRAME_QUEUE_H */
