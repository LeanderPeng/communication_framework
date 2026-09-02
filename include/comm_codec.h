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
    COMM_CODEC_OUTPUT_TOO_SMALL,
    COMM_CODEC_INPUT_TOO_SMALL,
    COMM_CODEC_INVALID_SYNC,
    COMM_CODEC_LENGTH_MISMATCH,
    COMM_CODEC_CRC_MISMATCH
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

/*
 * 将一段完整且只包含一帧的连续字节流解码为逻辑帧。
 *
 * input      待解码的输入字节。
 * input_size 输入字节总数，必须与帧内长度字段严格一致。
 * frame      由调用方提供的输出帧。
 *
 * 函数不处理拆包、粘包和垃圾前缀；这些字节流问题由增量解析器处理。
 * 只有所有字段和 CRC 均验证成功后才会修改 frame，失败时保留其原值。
 */
comm_codec_result_t comm_codec_decode(const uint8_t *input,
                                      size_t input_size,
                                      comm_frame_t *frame);

#endif /* COMM_CODEC_H */
