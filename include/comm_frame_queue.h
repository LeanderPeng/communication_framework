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

#endif /* COMM_FRAME_QUEUE_H */
