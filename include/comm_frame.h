#ifndef COMM_FRAME_H
#define COMM_FRAME_H

#include <stdint.h>

/* 使用两个同步字节，降低负载或噪声被误认为帧起始位置的概率。 */
#define COMM_FRAME_SYNC_BYTE_0          0xAAu
#define COMM_FRAME_SYNC_BYTE_1          0x55u

#define COMM_FRAME_VERSION              0x01u

/*
 * 固定帧头在线路字节流中的偏移。
 * sequence 和 payload_length 均使用大端序，高字节在前。
 */
#define COMM_FRAME_SYNC_BYTE_0_OFFSET   0u
#define COMM_FRAME_SYNC_BYTE_1_OFFSET   1u
#define COMM_FRAME_VERSION_OFFSET       2u
#define COMM_FRAME_TYPE_OFFSET          3u
#define COMM_FRAME_SEQUENCE_OFFSET      4u
#define COMM_FRAME_PAYLOAD_LENGTH_OFFSET 6u
#define COMM_FRAME_PAYLOAD_OFFSET       8u

/*
 * 这是当前实现允许的负载上限，不代表 16 位长度字段的理论上限。
 * 不同目标平台可在编译时根据需要覆盖该值。
 */
#ifndef COMM_FRAME_MAX_PAYLOAD_SIZE
#define COMM_FRAME_MAX_PAYLOAD_SIZE     256u
#endif

#if COMM_FRAME_MAX_PAYLOAD_SIZE == 0
#error "COMM_FRAME_MAX_PAYLOAD_SIZE must be greater than zero"
#endif

#if COMM_FRAME_MAX_PAYLOAD_SIZE > UINT16_MAX
#error "COMM_FRAME_MAX_PAYLOAD_SIZE must fit in the 16-bit length field"
#endif

/* 编码后各部分的大小，单位均为字节。 */
#define COMM_FRAME_HEADER_SIZE          8u
#define COMM_FRAME_CRC_SIZE             2u
#define COMM_FRAME_MIN_ENCODED_SIZE     \
    (COMM_FRAME_HEADER_SIZE + COMM_FRAME_CRC_SIZE)
#define COMM_FRAME_MAX_ENCODED_SIZE     \
    (COMM_FRAME_MIN_ENCODED_SIZE + COMM_FRAME_MAX_PAYLOAD_SIZE)

/*
 * CRC-16/CCITT-FALSE 参数。
 * CRC 从 version 开始计算，一直覆盖到 payload 的最后一个字节，
 * 不包含两个同步字节和 CRC 字段本身；CRC 结果也按大端序写入。
 */
#define COMM_FRAME_CRC16_POLYNOMIAL     0x1021u
#define COMM_FRAME_CRC16_INITIAL_VALUE  0xFFFFu
#define COMM_FRAME_CRC16_XOR_OUT        0x0000u

typedef enum {
    COMM_FRAME_TYPE_REQUEST  = 0x01,
    COMM_FRAME_TYPE_RESPONSE = 0x02,
    COMM_FRAME_TYPE_REPORT   = 0x03,
    COMM_FRAME_TYPE_ERROR    = 0x04
} comm_frame_type_t;

/*
 * 一帧数据在内存中的逻辑表示。
 *
 * 该结构体不等同于线路上的字节布局。编解码器必须逐字段序列化，
 * 从而避免结构体填充和 CPU 字节序影响通信协议。
 */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint16_t payload_length;
    uint8_t payload[COMM_FRAME_MAX_PAYLOAD_SIZE];
} comm_frame_t;

#endif /* COMM_FRAME_H */
