#ifndef APPLIANCE_CLIENT_H
#define APPLIANCE_CLIENT_H

#include <stdint.h>

#include "comm_message_manager.h"

/*
 * 发送一次查询洗衣机完整状态的业务请求。
 *
 * 本函数只负责构造 QUERY_STATUS payload，外层 REQUEST 帧、sequence、pending、
 * 超时和重试仍由 comm_message_manager 负责。直接返回 Message Manager 的结果，
 * 可以让调用方区分 pending 已满、发送超时、通道关闭等具体原因。
 *
 * now_ms 和 sequence 的约束与 comm_message_manager_send_request 完全相同。
 */
comm_message_manager_result_t appliance_client_request_status(
    comm_message_manager_t *message_manager,
    uint64_t now_ms,
    uint16_t *sequence);

#endif /* APPLIANCE_CLIENT_H */
