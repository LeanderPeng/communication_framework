#include "comm_parser.h"

#include "comm_codec.h"

/* 从帧头中的两个大端序字节读取负载长度。 */
static uint16_t comm_parser_read_payload_length(const uint8_t *header)
{
    return (uint16_t)(((uint16_t)header[COMM_FRAME_PAYLOAD_LENGTH_OFFSET]
                       << 8) |
                      (uint16_t)header[
                          COMM_FRAME_PAYLOAD_LENGTH_OFFSET + 1u]);
}

/* 将一个已确认无用的字节从输入缓冲区移除。 */
static int comm_parser_discard_one(comm_ringbuffer_t *ringbuffer)
{
    return comm_ringbuffer_discard(ringbuffer, 1u) == COMM_RINGBUFFER_OK;
}

comm_parser_result_t comm_parser_next(comm_ringbuffer_t *ringbuffer,
                                      uint8_t *scratch,
                                      size_t scratch_capacity,
                                      comm_frame_t *frame)
{
    comm_ringbuffer_result_t ringbuffer_result;
    comm_codec_result_t codec_result;
    size_t available_size;
    size_t frame_size;
    uint16_t payload_length;

    if ((ringbuffer == NULL) || (scratch == NULL) || (frame == NULL)) {
        return COMM_PARSER_NULL_ARGUMENT;
    }

    if (scratch_capacity < COMM_FRAME_MAX_ENCODED_SIZE) {
        return COMM_PARSER_SCRATCH_TOO_SMALL;
    }

    /* 零长度 peek 用于区分空缓冲区和损坏的 RingBuffer 状态。 */
    ringbuffer_result = comm_ringbuffer_peek(ringbuffer, 0u, NULL, 0u);
    if (ringbuffer_result != COMM_RINGBUFFER_OK) {
        return COMM_PARSER_RINGBUFFER_ERROR;
    }

    if (ringbuffer->capacity < COMM_FRAME_MAX_ENCODED_SIZE) {
        return COMM_PARSER_RINGBUFFER_TOO_SMALL;
    }

    for (;;) {
        available_size = comm_ringbuffer_size(ringbuffer);
        if (available_size == 0u) {
            return COMM_PARSER_NEED_MORE_DATA;
        }

        ringbuffer_result = comm_ringbuffer_peek(ringbuffer,
                                                 0u,
                                                 scratch,
                                                 1u);
        if (ringbuffer_result != COMM_RINGBUFFER_OK) {
            return COMM_PARSER_RINGBUFFER_ERROR;
        }

        if (scratch[0] != COMM_FRAME_SYNC_BYTE_0) {
            if (!comm_parser_discard_one(ringbuffer)) {
                return COMM_PARSER_RINGBUFFER_ERROR;
            }
            continue;
        }

        /* 单独到达的第一个同步字节必须保留到下次调用。 */
        if (available_size < 2u) {
            return COMM_PARSER_NEED_MORE_DATA;
        }

        ringbuffer_result = comm_ringbuffer_peek(ringbuffer,
                                                 1u,
                                                 &scratch[1],
                                                 1u);
        if (ringbuffer_result != COMM_RINGBUFFER_OK) {
            return COMM_PARSER_RINGBUFFER_ERROR;
        }

        if (scratch[1] != COMM_FRAME_SYNC_BYTE_1) {
            if (!comm_parser_discard_one(ringbuffer)) {
                return COMM_PARSER_RINGBUFFER_ERROR;
            }
            continue;
        }

        if (available_size < COMM_FRAME_HEADER_SIZE) {
            return COMM_PARSER_NEED_MORE_DATA;
        }

        ringbuffer_result = comm_ringbuffer_peek(ringbuffer,
                                                 0u,
                                                 scratch,
                                                 COMM_FRAME_HEADER_SIZE);
        if (ringbuffer_result != COMM_RINGBUFFER_OK) {
            return COMM_PARSER_RINGBUFFER_ERROR;
        }

        payload_length = comm_parser_read_payload_length(scratch);
        if (payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
            if (!comm_parser_discard_one(ringbuffer)) {
                return COMM_PARSER_RINGBUFFER_ERROR;
            }
            continue;
        }

        frame_size = COMM_FRAME_MIN_ENCODED_SIZE + payload_length;
        if (available_size < frame_size) {
            return COMM_PARSER_NEED_MORE_DATA;
        }

        ringbuffer_result = comm_ringbuffer_peek(ringbuffer,
                                                 0u,
                                                 scratch,
                                                 frame_size);
        if (ringbuffer_result != COMM_RINGBUFFER_OK) {
            return COMM_PARSER_RINGBUFFER_ERROR;
        }

        codec_result = comm_codec_decode(scratch, frame_size, frame);
        if (codec_result == COMM_CODEC_OK) {
            ringbuffer_result = comm_ringbuffer_discard(ringbuffer,
                                                        frame_size);
            if (ringbuffer_result != COMM_RINGBUFFER_OK) {
                return COMM_PARSER_RINGBUFFER_ERROR;
            }
            return COMM_PARSER_FRAME_READY;
        }

        /*
         * 只丢弃候选帧的第一个字节，使候选帧内部可能存在的下一组
         * 同步字节仍有机会被重新识别。
         */
        if (!comm_parser_discard_one(ringbuffer)) {
            return COMM_PARSER_RINGBUFFER_ERROR;
        }
    }
}
