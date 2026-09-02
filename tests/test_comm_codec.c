#include "comm_codec.h"

#include <stdio.h>
#include <string.h>

/*
 * 与 assert 不同，该检查不会被 NDEBUG 关闭。
 * 失败时返回非零值，使脚本和持续集成能够识别测试失败。
 */
#define TEST_CHECK(condition)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr,                                                  \
                    "test failed: %s (%s:%d)\n",                            \
                    #condition,                                              \
                    __FILE__,                                                \
                    __LINE__);                                               \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* 验证编码错误都会在写入输出缓冲区之前被发现。 */
static int test_encode_invalid_input(void)
{
    comm_frame_t frame;
    uint8_t output[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size;

    memset(&frame, 0, sizeof(frame));
    memset(output, 0xA5, sizeof(output));
    frame.version = COMM_FRAME_VERSION;
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;

    encoded_size = 99u;
    TEST_CHECK(comm_codec_encode(NULL,
                                 output,
                                 sizeof(output),
                                 &encoded_size) == COMM_CODEC_NULL_ARGUMENT);
    TEST_CHECK(encoded_size == 0u);
    TEST_CHECK(output[0] == 0xA5u);

    TEST_CHECK(comm_codec_encode(&frame,
                                 NULL,
                                 sizeof(output),
                                 &encoded_size) == COMM_CODEC_NULL_ARGUMENT);
    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 NULL) == COMM_CODEC_NULL_ARGUMENT);

    frame.version = (uint8_t)(COMM_FRAME_VERSION + 1u);
    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 &encoded_size) ==
               COMM_CODEC_UNSUPPORTED_VERSION);
    TEST_CHECK(output[0] == 0xA5u);
    frame.version = COMM_FRAME_VERSION;

    frame.type = 0xFFu;
    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 &encoded_size) == COMM_CODEC_INVALID_TYPE);
    TEST_CHECK(output[0] == 0xA5u);
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;

    frame.payload_length = (uint16_t)(COMM_FRAME_MAX_PAYLOAD_SIZE + 1u);
    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 &encoded_size) ==
               COMM_CODEC_PAYLOAD_TOO_LARGE);
    TEST_CHECK(output[0] == 0xA5u);
    frame.payload_length = 0u;

    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 COMM_FRAME_MIN_ENCODED_SIZE - 1u,
                                 &encoded_size) ==
               COMM_CODEC_OUTPUT_TOO_SMALL);
    TEST_CHECK(output[0] == 0xA5u);

    return 0;
}

/* 验证零负载帧的固定字段、大端序和 CRC。 */
static int test_encode_empty_payload(void)
{
    static const uint8_t expected[] = {
        0xAAu, 0x55u, 0x01u, 0x01u, 0x12u,
        0x34u, 0x00u, 0x00u, 0x0Eu, 0x4Bu
    };
    comm_frame_t frame;
    uint8_t output[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;
    frame.sequence = 0x1234u;

    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 &encoded_size) == COMM_CODEC_OK);
    TEST_CHECK(encoded_size == sizeof(expected));
    TEST_CHECK(memcmp(output, expected, sizeof(expected)) == 0);

    return 0;
}

/* 验证带负载帧的长度、负载复制和 CRC。 */
static int test_encode_payload(void)
{
    static const uint8_t expected[] = {
        0xAAu, 0x55u, 0x01u, 0x01u, 0x12u, 0x34u, 0x00u,
        0x03u, 0x10u, 0x20u, 0x30u, 0x22u, 0x3Du
    };
    comm_frame_t frame;
    uint8_t output[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;
    frame.sequence = 0x1234u;
    frame.payload_length = 3u;
    frame.payload[0] = 0x10u;
    frame.payload[1] = 0x20u;
    frame.payload[2] = 0x30u;

    TEST_CHECK(comm_codec_encode(&frame,
                                 output,
                                 sizeof(output),
                                 &encoded_size) == COMM_CODEC_OK);
    TEST_CHECK(encoded_size == sizeof(expected));
    TEST_CHECK(memcmp(output, expected, sizeof(expected)) == 0);

    return 0;
}

/* 验证完整合法字节流可以恢复成逻辑帧。 */
static int test_decode_payload(void)
{
    static const uint8_t input[] = {
        0xAAu, 0x55u, 0x01u, 0x01u, 0x12u, 0x34u, 0x00u,
        0x03u, 0x10u, 0x20u, 0x30u, 0x22u, 0x3Du
    };
    comm_frame_t frame;

    memset(&frame, 0xA5, sizeof(frame));
    TEST_CHECK(comm_codec_decode(input, sizeof(input), &frame) ==
               COMM_CODEC_OK);
    TEST_CHECK(frame.version == COMM_FRAME_VERSION);
    TEST_CHECK(frame.type == (uint8_t)COMM_FRAME_TYPE_REQUEST);
    TEST_CHECK(frame.sequence == 0x1234u);
    TEST_CHECK(frame.payload_length == 3u);
    TEST_CHECK(frame.payload[0] == 0x10u);
    TEST_CHECK(frame.payload[1] == 0x20u);
    TEST_CHECK(frame.payload[2] == 0x30u);
    TEST_CHECK(frame.payload[3] == 0u);

    return 0;
}

/* 验证各种坏帧都会被拒绝，并且不会修改输出帧。 */
static int test_decode_invalid_input(void)
{
    static const uint8_t valid_input[] = {
        0xAAu, 0x55u, 0x01u, 0x01u, 0x12u, 0x34u, 0x00u,
        0x03u, 0x10u, 0x20u, 0x30u, 0x22u, 0x3Du
    };
    uint8_t input[sizeof(valid_input) + 1u];
    comm_frame_t frame;
    comm_frame_t original_frame;

    memset(&frame, 0xA5, sizeof(frame));
    original_frame = frame;

    TEST_CHECK(comm_codec_decode(NULL, sizeof(valid_input), &frame) ==
               COMM_CODEC_NULL_ARGUMENT);
    TEST_CHECK(comm_codec_decode(valid_input, sizeof(valid_input), NULL) ==
               COMM_CODEC_NULL_ARGUMENT);
    TEST_CHECK(comm_codec_decode(valid_input,
                                 COMM_FRAME_MIN_ENCODED_SIZE - 1u,
                                 &frame) == COMM_CODEC_INPUT_TOO_SMALL);

    memcpy(input, valid_input, sizeof(valid_input));
    input[COMM_FRAME_SYNC_BYTE_0_OFFSET] ^= 0x01u;
    TEST_CHECK(comm_codec_decode(input, sizeof(valid_input), &frame) ==
               COMM_CODEC_INVALID_SYNC);

    memcpy(input, valid_input, sizeof(valid_input));
    input[COMM_FRAME_VERSION_OFFSET] =
        (uint8_t)(COMM_FRAME_VERSION + 1u);
    TEST_CHECK(comm_codec_decode(input, sizeof(valid_input), &frame) ==
               COMM_CODEC_UNSUPPORTED_VERSION);

    memcpy(input, valid_input, sizeof(valid_input));
    input[COMM_FRAME_TYPE_OFFSET] = 0xFFu;
    TEST_CHECK(comm_codec_decode(input, sizeof(valid_input), &frame) ==
               COMM_CODEC_INVALID_TYPE);

    memcpy(input, valid_input, sizeof(valid_input));
    input[COMM_FRAME_PAYLOAD_LENGTH_OFFSET] = 0x01u;
    input[COMM_FRAME_PAYLOAD_LENGTH_OFFSET + 1u] = 0x01u;
    TEST_CHECK(comm_codec_decode(input, sizeof(valid_input), &frame) ==
               COMM_CODEC_PAYLOAD_TOO_LARGE);

    TEST_CHECK(comm_codec_decode(valid_input,
                                 sizeof(valid_input) - 1u,
                                 &frame) == COMM_CODEC_LENGTH_MISMATCH);

    memcpy(input, valid_input, sizeof(valid_input));
    input[sizeof(valid_input)] = 0x00u;
    TEST_CHECK(comm_codec_decode(input, sizeof(input), &frame) ==
               COMM_CODEC_LENGTH_MISMATCH);

    memcpy(input, valid_input, sizeof(valid_input));
    input[COMM_FRAME_PAYLOAD_OFFSET] ^= 0x01u;
    TEST_CHECK(comm_codec_decode(input, sizeof(valid_input), &frame) ==
               COMM_CODEC_CRC_MISMATCH);
    TEST_CHECK(memcmp(&frame, &original_frame, sizeof(frame)) == 0);

    return 0;
}

int main(void)
{
    if (test_encode_invalid_input() != 0) {
        return 1;
    }
    if (test_encode_empty_payload() != 0) {
        return 1;
    }
    if (test_encode_payload() != 0) {
        return 1;
    }
    if (test_decode_payload() != 0) {
        return 1;
    }
    if (test_decode_invalid_input() != 0) {
        return 1;
    }

    puts("test_comm_codec: all tests passed");
    return 0;
}
