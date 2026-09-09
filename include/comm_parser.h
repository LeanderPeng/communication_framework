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
    COMM_PARSER_RINGBUFFER_TOO_SMALL,
    COMM_PARSER_RINGBUFFER_ERROR
} comm_parser_result_t;

/*
 * Parser 在字节流中完成重新同步时累计的诊断信息。
 *
 * frames_ready              成功解码并消费的完整帧数量。
 * discarded_bytes           为寻找下一帧而丢弃的字节总数，包括普通垃圾字节
 *                           和坏候选帧逐字节跳过的内容。
 * oversized_length_candidates 帧头声明的 payload_length 超过配置上限的次数。
 * decode_errors             完整候选帧交给 Codec 后解码失败的总次数。
 * crc_errors                decode_errors 中由 CRC 不匹配造成的次数，是其子集。
 *
 * 所有计数器到达 UINT64_MAX 后保持饱和，不会在长期运行中绕回 0。结构体不
 * 加锁，应该和对应 RingBuffer 一样由同一个接收任务维护。首次使用前必须
 * 调用 comm_parser_stats_reset，或者使用 {0} 对整个结构体进行零初始化。
 */
typedef struct {
    uint64_t frames_ready;
    uint64_t discarded_bytes;
    uint64_t oversized_length_candidates;
    uint64_t decode_errors;
    uint64_t crc_errors;
} comm_parser_stats_t;

/* Parser 统计对象自身接口的结果，不与“是否解析出一帧”的结果混用。 */
typedef enum {
    COMM_PARSER_STATS_OK = 0,
    COMM_PARSER_STATS_NULL_ARGUMENT
} comm_parser_stats_result_t;

/* 把所有 Parser 诊断计数器清零；stats 为空时返回 NULL_ARGUMENT。 */
comm_parser_stats_result_t comm_parser_stats_reset(comm_parser_stats_t *stats);

/*
 * 尝试从 RingBuffer 中提取下一帧。
 *
 * ringbuffer      保存 UART/TCP 原始字节的输入缓冲区。
 * scratch         调用方提供的连续临时缓冲区。
 * scratch_capacity 临时缓冲区容量，至少为 COMM_FRAME_MAX_ENCODED_SIZE。
 * frame           成功时接收解码后的逻辑帧。
 *
 * ringbuffer 的容量也必须至少为 COMM_FRAME_MAX_ENCODED_SIZE，确保最大
 * 合法帧能够完整保留到校验结束。
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

/*
 * 与 comm_parser_next 的解析和消费语义完全相同，同时更新调用方提供的 stats。
 * stats 不能为空；入口参数或初始配置校验失败不会修改统计值。进入解析循环后，
 * 每个已经成功完成的丢弃或解码动作都会立即反映到统计值中。
 *
 * 保留原接口而不是强制所有调用方传入统计对象，可以让资源紧张的目标设备
 * 继续使用无统计路径，也不会破坏现有驱动代码。
 */
comm_parser_result_t comm_parser_next_with_stats(
    comm_ringbuffer_t *ringbuffer,
    uint8_t *scratch,
    size_t scratch_capacity,
    comm_frame_t *frame,
    comm_parser_stats_t *stats);

#endif /* COMM_PARSER_H */
