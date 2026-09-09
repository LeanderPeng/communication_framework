#include "appliance_client.h"

#include "appliance_protocol.h"

comm_message_manager_result_t appliance_client_request_status(
    comm_message_manager_t *message_manager,
    uint64_t now_ms,
    uint16_t *sequence)
{
    static const uint8_t payload[APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE] = {
        APPLIANCE_MESSAGE_QUERY_STATUS
    };

    /*
     * QUERY_STATUS 的业务负载永远只有消息 ID。这里不自行生成 sequence，也不
     * 直接操作发送通道，否则会绕过 Message Manager 的 pending 和超时重试。
     */
    return comm_message_manager_send_request(message_manager,
                                             payload,
                                             sizeof(payload),
                                             now_ms,
                                             sequence);
}
