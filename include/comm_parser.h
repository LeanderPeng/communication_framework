#ifndef COMM_PARSER_H
#define COMM_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "comm_frame.h"
#include "comm_ringbuffer.h"

/* Parser 每次尝试提取一帧后的结果。 */
typedef enum {
    COMM_PARSER_FRAME_READY = 0,
    COMM_PARSER_NEED_MORE_DATA,
    COMM_PARSER_NULL_ARGUMENT,
    COMM_PARSER_SCRATCH_TOO_SMALL,
    COMM_PARSER_RINGBUFFER_ERROR
} comm_parser_result_t;

/*
 * 尝试从 RingBuffer 中提取下一帧。
 *
 * ringbuffer      保存 UART/TCP 原始字节的输入缓冲区。
 * scratch         调用方提供的连续临时缓冲区。
 * scratch_capacity 临时缓冲区容量，至少为 COMM_FRAME_MAX_ENCODED_SIZE。
 * frame           成功时接收解码后的逻辑帧。
 *
 * 返回 COMM_PARSER_FRAME_READY 时，恰好消费一帧并更新 frame。
 * 返回 COMM_PARSER_NEED_MORE_DATA 时，不完整的候选帧仍保留在 RingBuffer；
 * 在找到候选帧之前遇到的垃圾字节可能已经被丢弃。
 *
 * Parser 会在坏帧后逐字节重新寻找同步字节。scratch 不能与 RingBuffer 的
 * storage 或 frame 占用的内存区域重叠。除 FRAME_READY 外不修改 frame。
 */
comm_parser_result_t comm_parser_next(comm_ringbuffer_t *ringbuffer,
                                      uint8_t *scratch,
                                      size_t scratch_capacity,
                                      comm_frame_t *frame);

#endif /* COMM_PARSER_H */
