#include "comm_codec.h"
#include "comm_parser.h"

#include <stdio.h>
#include <string.h>

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

#define TEST_RINGBUFFER_CAPACITY (COMM_FRAME_MAX_ENCODED_SIZE * 2u + 16u)

/* 生成后续测试使用的合法编码帧。 */
static int make_encoded_frame(uint16_t sequence,
                              const uint8_t *payload,
                              size_t payload_length,
                              uint8_t *encoded,
                              size_t *encoded_size)
{
    comm_frame_t frame;

    if (payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
        return 1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.version = COMM_FRAME_VERSION;
    frame.type = (uint8_t)COMM_FRAME_TYPE_REQUEST;
    frame.sequence = sequence;
    frame.payload_length = (uint16_t)payload_length;
    if (payload_length > 0u) {
        memcpy(frame.payload, payload, payload_length);
    }

    return comm_codec_encode(&frame,
                             encoded,
                             COMM_FRAME_MAX_ENCODED_SIZE,
                             encoded_size) == COMM_CODEC_OK ? 0 : 1;
}

/* 验证空指针、临时空间和 RingBuffer 配置错误。 */
static int test_invalid_configuration(void)
{
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t small_storage[COMM_FRAME_MAX_ENCODED_SIZE - 1u];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(NULL,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_NULL_ARGUMENT);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                NULL,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_NULL_ARGUMENT);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                NULL) == COMM_PARSER_NULL_ARGUMENT);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch) - 1u,
                                &frame) == COMM_PARSER_SCRATCH_TOO_SMALL);

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    small_storage,
                                    sizeof(small_storage)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_RINGBUFFER_TOO_SMALL);

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    ringbuffer.write_index = 1u;
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_RINGBUFFER_ERROR);

    return 0;
}

/* 验证帧头分多次到达时，已有候选字节不会被提前消费。 */
static int test_split_frame(void)
{
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    comm_frame_t unchanged_frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;

    TEST_CHECK(make_encoded_frame(0x1234u,
                                  payload,
                                  sizeof(payload),
                                  encoded,
                                  &encoded_size) == 0);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    memset(&frame, 0xA5, sizeof(frame));
    memset(&unchanged_frame, 0xA5, sizeof(unchanged_frame));

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer, encoded, 1u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_NEED_MORE_DATA);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 1u);
    TEST_CHECK(memcmp(&frame, &unchanged_frame, sizeof(frame)) == 0);

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer, &encoded[1], 4u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_NEED_MORE_DATA);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 5u);
    TEST_CHECK(memcmp(&frame, &unchanged_frame, sizeof(frame)) == 0);

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     &encoded[5],
                                     encoded_size - 5u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 0x1234u);
    TEST_CHECK(frame.payload_length == sizeof(payload));
    TEST_CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);

    return 0;
}

/* 验证完整帧跨过 RingBuffer 物理尾部时仍可正确解码。 */
static int test_wrapped_frame(void)
{
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;
    size_t start_index = sizeof(storage) - 4u;
    size_t expected_end_index;

    TEST_CHECK(make_encoded_frame(0x4321u,
                                  payload,
                                  sizeof(payload),
                                  encoded,
                                  &encoded_size) == 0);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    ringbuffer.read_index = start_index;
    ringbuffer.write_index = start_index;

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     encoded,
                                     encoded_size) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);

    expected_end_index = encoded_size - 4u;
    TEST_CHECK(frame.sequence == 0x4321u);
    TEST_CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    TEST_CHECK(ringbuffer.read_index == expected_end_index);
    TEST_CHECK(ringbuffer.write_index == expected_end_index);
    TEST_CHECK(ringbuffer.used == 0u);

    return 0;
}

/* 验证垃圾前缀被清理，但末尾可能成为帧头的 AA 会被保留。 */
static int test_garbage_prefix(void)
{
    static const uint8_t garbage[] = {0x99u, 0x88u, 0xAAu};
    static const uint8_t payload[] = {0x42u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;

    TEST_CHECK(make_encoded_frame(7u,
                                  payload,
                                  sizeof(payload),
                                  encoded,
                                  &encoded_size) == 0);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     garbage,
                                     sizeof(garbage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_NEED_MORE_DATA);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 1u);

    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     &encoded[1],
                                     encoded_size - 1u) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 7u);
    TEST_CHECK(frame.payload[0] == 0x42u);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);

    return 0;
}

/* 验证一次写入多帧时，每次调用只提取一帧。 */
static int test_multiple_frames(void)
{
    static const uint8_t first_payload[] = {0x10u};
    static const uint8_t second_payload[] = {0x20u, 0x30u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t first_encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t second_encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t first_size = 0u;
    size_t second_size = 0u;

    TEST_CHECK(make_encoded_frame(1u,
                                  first_payload,
                                  sizeof(first_payload),
                                  first_encoded,
                                  &first_size) == 0);
    TEST_CHECK(make_encoded_frame(2u,
                                  second_payload,
                                  sizeof(second_payload),
                                  second_encoded,
                                  &second_size) == 0);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     first_encoded,
                                     first_size) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     second_encoded,
                                     second_size) == COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 1u);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == second_size);

    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 2u);
    TEST_CHECK(frame.payload_length == sizeof(second_payload));
    TEST_CHECK(memcmp(frame.payload,
                      second_payload,
                      sizeof(second_payload)) == 0);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);

    return 0;
}

/* 验证 CRC 损坏帧不会阻止后续合法帧被重新找到。 */
static int test_crc_error_resynchronizes(void)
{
    static const uint8_t bad_payload[] = {0x10u, 0x20u};
    static const uint8_t good_payload[] = {0x30u, 0x40u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t bad_encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t good_encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t bad_size = 0u;
    size_t good_size = 0u;

    TEST_CHECK(make_encoded_frame(1u,
                                  bad_payload,
                                  sizeof(bad_payload),
                                  bad_encoded,
                                  &bad_size) == 0);
    TEST_CHECK(make_encoded_frame(2u,
                                  good_payload,
                                  sizeof(good_payload),
                                  good_encoded,
                                  &good_size) == 0);
    bad_encoded[COMM_FRAME_PAYLOAD_OFFSET] ^= 0x01u;

    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     bad_encoded,
                                     bad_size) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     good_encoded,
                                     good_size) == COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 2u);
    TEST_CHECK(memcmp(frame.payload, good_payload, sizeof(good_payload)) == 0);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);

    return 0;
}

/* 验证非法超长帧头后仍能恢复到下一帧。 */
static int test_invalid_length_resynchronizes(void)
{
    static const uint8_t invalid_header[] = {
        0xAAu, 0x55u, 0x01u, 0x01u,
        0x00u, 0x01u, 0xFFu, 0xFFu
    };
    static const uint8_t payload[] = {0x55u};
    comm_ringbuffer_t ringbuffer;
    comm_frame_t frame;
    uint8_t storage[TEST_RINGBUFFER_CAPACITY];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t encoded[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t encoded_size = 0u;

    TEST_CHECK(make_encoded_frame(9u,
                                  payload,
                                  sizeof(payload),
                                  encoded,
                                  &encoded_size) == 0);
    TEST_CHECK(comm_ringbuffer_init(&ringbuffer,
                                    storage,
                                    sizeof(storage)) == COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     invalid_header,
                                     sizeof(invalid_header)) ==
               COMM_RINGBUFFER_OK);
    TEST_CHECK(comm_ringbuffer_write(&ringbuffer,
                                     encoded,
                                     encoded_size) == COMM_RINGBUFFER_OK);

    TEST_CHECK(comm_parser_next(&ringbuffer,
                                scratch,
                                sizeof(scratch),
                                &frame) == COMM_PARSER_FRAME_READY);
    TEST_CHECK(frame.sequence == 9u);
    TEST_CHECK(comm_ringbuffer_size(&ringbuffer) == 0u);

    return 0;
}

int main(void)
{
    if (test_invalid_configuration() != 0) {
        return 1;
    }
    if (test_split_frame() != 0) {
        return 1;
    }
    if (test_wrapped_frame() != 0) {
        return 1;
    }
    if (test_garbage_prefix() != 0) {
        return 1;
    }
    if (test_multiple_frames() != 0) {
        return 1;
    }
    if (test_crc_error_resynchronizes() != 0) {
        return 1;
    }
    if (test_invalid_length_resynchronizes() != 0) {
        return 1;
    }

    puts("test_comm_parser: all tests passed");
    return 0;
}
