#include "comm_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 验证所有错误都会在写入输出缓冲区之前被发现。 */
static void test_invalid_input(void)
{
    comm_frame_t frame;
    uint8_t output[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size;

    memset(&frame, 0, sizeof(frame));
    memset(output, 0xA5, sizeof(output));
    frame.version = COMM_FRAME_VERSION;
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;

    encoded_size = 99u;
    assert(comm_codec_encode(NULL, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_NULL_ARGUMENT);
    assert(encoded_size == 0u);
    assert(output[0] == 0xA5u);

    assert(comm_codec_encode(&frame, NULL, sizeof(output), &encoded_size) ==
           COMM_CODEC_NULL_ARGUMENT);
    assert(comm_codec_encode(&frame, output, sizeof(output), NULL) ==
           COMM_CODEC_NULL_ARGUMENT);

    frame.version = (uint8_t)(COMM_FRAME_VERSION + 1u);
    assert(comm_codec_encode(&frame, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_UNSUPPORTED_VERSION);
    assert(output[0] == 0xA5u);
    frame.version = COMM_FRAME_VERSION;

    frame.type = 0xFFu;
    assert(comm_codec_encode(&frame, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_INVALID_TYPE);
    assert(output[0] == 0xA5u);
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;

    frame.payload_length = (uint16_t)(COMM_FRAME_MAX_PAYLOAD_SIZE + 1u);
    assert(comm_codec_encode(&frame, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_PAYLOAD_TOO_LARGE);
    assert(output[0] == 0xA5u);
    frame.payload_length = 0u;

    assert(comm_codec_encode(&frame,
                             output,
                             COMM_FRAME_MIN_ENCODED_SIZE - 1u,
                             &encoded_size) == COMM_CODEC_OUTPUT_TOO_SMALL);
    assert(output[0] == 0xA5u);
}

/* 验证零负载帧的固定字段、大端序和 CRC。 */
static void test_encode_empty_payload(void)
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

    assert(comm_codec_encode(&frame, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_OK);
    assert(encoded_size == sizeof(expected));
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

/* 验证带负载帧的长度、负载复制和 CRC。 */
static void test_encode_payload(void)
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

    assert(comm_codec_encode(&frame, output, sizeof(output), &encoded_size) ==
           COMM_CODEC_OK);
    assert(encoded_size == sizeof(expected));
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

int main(void)
{
    test_invalid_input();
    test_encode_empty_payload();
    test_encode_payload();

    puts("test_comm_codec: all tests passed");
    return 0;
}
