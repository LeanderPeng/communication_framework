#include "comm_codec.h"

#include <string.h>

/* 将一个 16 位整数按大端序写入连续的两个字节。 */
static void comm_write_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)(value & 0xFFu);
}

/* 从连续的两个大端序字节中恢复一个 16 位整数。 */
static uint16_t comm_read_u16_be(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) |
                      (uint16_t)source[1]);
}

/* 当前协议版本只接受下面四种通信消息类型。 */
static int comm_frame_type_is_valid(uint8_t type)
{
    switch (type) {
    case COMM_FRAME_TYPE_REQUEST:
    case COMM_FRAME_TYPE_RESPONSE:
    case COMM_FRAME_TYPE_REPORT:
    case COMM_FRAME_TYPE_ERROR:
        return 1;
    default:
        return 0;
    }
}

/* 按 CRC-16/CCITT-FALSE 参数逐字节计算校验值。 */
static uint16_t comm_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = COMM_FRAME_CRC16_INITIAL_VALUE;
    size_t index;

    for (index = 0u; index < length; ++index) {
        unsigned int bit;

        crc ^= (uint16_t)((uint16_t)data[index] << 8);
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^
                                 COMM_FRAME_CRC16_POLYNOMIAL);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return (uint16_t)(crc ^ COMM_FRAME_CRC16_XOR_OUT);
}

comm_codec_result_t comm_codec_encode(const comm_frame_t *frame,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      size_t *encoded_size)
{
    size_t required_size;
    size_t crc_input_size;
    size_t crc_offset;
    uint16_t crc;

    if (encoded_size == NULL) {
        return COMM_CODEC_NULL_ARGUMENT;
    }

    *encoded_size = 0u;

    if ((frame == NULL) || (output == NULL)) {
        return COMM_CODEC_NULL_ARGUMENT;
    }

    if (frame->version != COMM_FRAME_VERSION) {
        return COMM_CODEC_UNSUPPORTED_VERSION;
    }

    if (!comm_frame_type_is_valid(frame->type)) {
        return COMM_CODEC_INVALID_TYPE;
    }

    if (frame->payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
        return COMM_CODEC_PAYLOAD_TOO_LARGE;
    }

    required_size = COMM_FRAME_MIN_ENCODED_SIZE + frame->payload_length;
    if (output_capacity < required_size) {
        return COMM_CODEC_OUTPUT_TOO_SMALL;
    }

    /* 所有可能失败的检查结束后，才开始修改输出缓冲区。 */
    output[COMM_FRAME_SYNC_BYTE_0_OFFSET] = COMM_FRAME_SYNC_BYTE_0;
    output[COMM_FRAME_SYNC_BYTE_1_OFFSET] = COMM_FRAME_SYNC_BYTE_1;
    output[COMM_FRAME_VERSION_OFFSET] = frame->version;
    output[COMM_FRAME_TYPE_OFFSET] = frame->type;
    comm_write_u16_be(&output[COMM_FRAME_SEQUENCE_OFFSET], frame->sequence);
    comm_write_u16_be(&output[COMM_FRAME_PAYLOAD_LENGTH_OFFSET],
                      frame->payload_length);

    if (frame->payload_length > 0u) {
        memcpy(&output[COMM_FRAME_PAYLOAD_OFFSET],
               frame->payload,
               frame->payload_length);
    }

    crc_input_size = (COMM_FRAME_HEADER_SIZE - COMM_FRAME_VERSION_OFFSET) +
                     frame->payload_length;
    crc = comm_crc16_ccitt_false(&output[COMM_FRAME_VERSION_OFFSET],
                                 crc_input_size);
    crc_offset = COMM_FRAME_PAYLOAD_OFFSET + frame->payload_length;
    comm_write_u16_be(&output[crc_offset], crc);

    *encoded_size = required_size;
    return COMM_CODEC_OK;
}

comm_codec_result_t comm_codec_decode(const uint8_t *input,
                                      size_t input_size,
                                      comm_frame_t *frame)
{
    comm_frame_t decoded_frame;
    uint16_t payload_length;
    uint16_t received_crc;
    uint16_t calculated_crc;
    size_t expected_size;
    size_t crc_input_size;
    size_t crc_offset;

    if ((input == NULL) || (frame == NULL)) {
        return COMM_CODEC_NULL_ARGUMENT;
    }

    if (input_size < COMM_FRAME_MIN_ENCODED_SIZE) {
        return COMM_CODEC_INPUT_TOO_SMALL;
    }

    if ((input[COMM_FRAME_SYNC_BYTE_0_OFFSET] != COMM_FRAME_SYNC_BYTE_0) ||
        (input[COMM_FRAME_SYNC_BYTE_1_OFFSET] != COMM_FRAME_SYNC_BYTE_1)) {
        return COMM_CODEC_INVALID_SYNC;
    }

    if (input[COMM_FRAME_VERSION_OFFSET] != COMM_FRAME_VERSION) {
        return COMM_CODEC_UNSUPPORTED_VERSION;
    }

    if (!comm_frame_type_is_valid(input[COMM_FRAME_TYPE_OFFSET])) {
        return COMM_CODEC_INVALID_TYPE;
    }

    payload_length = comm_read_u16_be(
        &input[COMM_FRAME_PAYLOAD_LENGTH_OFFSET]);
    if (payload_length > COMM_FRAME_MAX_PAYLOAD_SIZE) {
        return COMM_CODEC_PAYLOAD_TOO_LARGE;
    }

    expected_size = COMM_FRAME_MIN_ENCODED_SIZE + payload_length;
    if (input_size != expected_size) {
        return COMM_CODEC_LENGTH_MISMATCH;
    }

    crc_input_size = (COMM_FRAME_HEADER_SIZE - COMM_FRAME_VERSION_OFFSET) +
                     payload_length;
    calculated_crc = comm_crc16_ccitt_false(
        &input[COMM_FRAME_VERSION_OFFSET], crc_input_size);
    crc_offset = COMM_FRAME_PAYLOAD_OFFSET + payload_length;
    received_crc = comm_read_u16_be(&input[crc_offset]);
    if (received_crc != calculated_crc) {
        return COMM_CODEC_CRC_MISMATCH;
    }

    /* 使用临时对象，确保任何解码错误都不会产生半更新的输出帧。 */
    memset(&decoded_frame, 0, sizeof(decoded_frame));
    decoded_frame.version = input[COMM_FRAME_VERSION_OFFSET];
    decoded_frame.type = input[COMM_FRAME_TYPE_OFFSET];
    decoded_frame.sequence = comm_read_u16_be(
        &input[COMM_FRAME_SEQUENCE_OFFSET]);
    decoded_frame.payload_length = payload_length;
    if (payload_length > 0u) {
        memcpy(decoded_frame.payload,
               &input[COMM_FRAME_PAYLOAD_OFFSET],
               payload_length);
    }

    *frame = decoded_frame;
    return COMM_CODEC_OK;
}
