#ifndef COMM_RINGBUFFER_H
#define COMM_RINGBUFFER_H

#include <stddef.h>
#include <stdint.h>

/* RingBuffer 接口的执行结果，成功固定为 0。 */
typedef enum {
    COMM_RINGBUFFER_OK = 0,
    COMM_RINGBUFFER_NULL_ARGUMENT,
    COMM_RINGBUFFER_INVALID_CAPACITY,
    COMM_RINGBUFFER_INVALID_STATE,
    COMM_RINGBUFFER_INSUFFICIENT_SPACE,
    COMM_RINGBUFFER_INSUFFICIENT_DATA
} comm_ringbuffer_result_t;

/*
 * 固定内存环形缓冲区。
 *
 * storage 指向调用方提供的存储区，RingBuffer 不申请也不释放该内存。
 * used 用于区分 read_index == write_index 时的“空”和“满”两种状态，
 * 因此 capacity 个字节可以全部使用，不需要永久浪费一个槽位。
 *
 * storage 的生命周期必须长于 RingBuffer 的使用周期。本模块不提供线程
 * 同步；若多个任务同时访问，应由平台适配层在接口外部加锁。
 *
 * 所有字段由本模块维护，调用方不应直接修改。
 */
typedef struct {
    uint8_t *storage;   /* 调用方拥有的字节存储区 */
    size_t capacity;    /* 存储区总容量 */
    size_t read_index;  /* 下一次读取的起始位置 */
    size_t write_index; /* 下一次写入的起始位置 */
    size_t used;        /* 当前保存的有效字节数 */
} comm_ringbuffer_t;

/*
 * 使用调用方提供的固定存储区初始化 RingBuffer。
 * storage 不能为空且 capacity 必须大于 0；失败时不修改 ringbuffer。
 */
comm_ringbuffer_result_t comm_ringbuffer_init(comm_ringbuffer_t *ringbuffer,
                                              uint8_t *storage,
                                              size_t capacity);

/* 清空已有数据，但不改变存储区地址和容量。 */
comm_ringbuffer_result_t comm_ringbuffer_reset(
    comm_ringbuffer_t *ringbuffer);

/* 返回当前有效字节数；参数为空或状态无效时返回 0。 */
size_t comm_ringbuffer_size(const comm_ringbuffer_t *ringbuffer);

/* 返回当前剩余空间；参数为空或状态无效时返回 0。 */
size_t comm_ringbuffer_free_space(const comm_ringbuffer_t *ringbuffer);

/*
 * 写入指定数量的字节。
 * 空间不足时一个字节也不写，并返回 COMM_RINGBUFFER_INSUFFICIENT_SPACE。
 * length 为 0 时允许 data 为空，并直接返回成功。
 * data 指向的输入区域不能与 RingBuffer 的 storage 重叠。
 */
comm_ringbuffer_result_t comm_ringbuffer_write(
    comm_ringbuffer_t *ringbuffer,
    const uint8_t *data,
    size_t length);

/*
 * 读取并移除指定数量的字节。
 * 数据不足时一个字节也不读，并返回 COMM_RINGBUFFER_INSUFFICIENT_DATA。
 * length 为 0 时允许 output 为空，并直接返回成功。
 * output 指向的输出区域不能与 RingBuffer 的 storage 重叠。
 */
comm_ringbuffer_result_t comm_ringbuffer_read(comm_ringbuffer_t *ringbuffer,
                                              uint8_t *output,
                                              size_t length);

#endif /* COMM_RINGBUFFER_H */
