#ifndef COMM_CODEC_H
#define COMM_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "comm_frame.h"

/* 编解码接口的执行结果，成功固定为 0，便于调用方统一判断。 */
typedef enum {
    COMM_CODEC_OK = 0,
    COMM_CODEC_NULL_ARGUMENT,
    COMM_CODEC_UNSUPPORTED_VERSION,
    COMM_CODEC_INVALID_TYPE,
    COMM_CODEC_PAYLOAD_TOO_LARGE,
    COMM_CODEC_OUTPUT_TOO_SMALL
} comm_codec_result_t;

/*
 * 将内存中的逻辑帧编码为可由 UART、TCP 等传输层发送的连续字节流。
 *
 * frame           输入帧，函数只读取，不修改。
 * output          由调用方提供的输出缓冲区。
 * output_capacity 输出缓冲区的总容量，单位为字节。
 * encoded_size    成功时写入实际编码长度；失败时写入 0。
 *
 * 函数不申请动态内存。frame、output 和 encoded_size 在函数返回后仍由
 * 调用方拥有。所有参数和容量会在写入 output 之前完成检查。
 */
comm_codec_result_t comm_codec_encode(const comm_frame_t *frame,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      size_t *encoded_size);

#endif /* COMM_CODEC_H */
