#include "appliance_client.h"
#include "appliance_manager.h"
#include "comm_codec.h"
#include "comm_frame_channel_pthread.h"
#include "comm_parser.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "test failed: %s (%s:%d)\n",                    \
                    #condition, __FILE__, __LINE__);                          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

#define QUEUE_CAPACITY 4u
#define TRACE_CAPACITY 16u

typedef struct {
    comm_message_event_type_t type;
    uint16_t sequence;
    appliance_manager_result_t business_result;
} event_record_t;

/*
 * 使用真实 pthread 队列和 Channel，但由一个测试线程按确定顺序推进各阶段。
 * 此处验证模块间的字节、帧、事件和模型交接，不模拟线程调度或真实 UART/TCP。
 */
typedef struct {
    comm_frame_queue_pthread_t tx_queue;
    comm_frame_queue_pthread_t rx_queue;
    comm_frame_queue_pthread_stats_t tx_stats_storage;
    comm_frame_queue_pthread_stats_t rx_stats_storage;
    comm_frame_t tx_storage[QUEUE_CAPACITY];
    comm_frame_t rx_storage[QUEUE_CAPACITY];
    comm_frame_channel_t tx_channel;
    comm_frame_channel_t rx_channel;
    comm_ringbuffer_t ring;
    uint8_t ring_storage[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
    comm_parser_stats_t parser_stats;
    comm_message_manager_t messages;
    comm_message_pending_t pending[2];
    comm_message_manager_stats_t message_stats_storage;
    appliance_manager_t appliance;
    event_record_t events[TRACE_CAPACITY];
    appliance_model_t notifications[TRACE_CAPACITY];
    size_t event_count;
    size_t notify_count;
    int callback_error;
} fixture_t;

/* 复制模型，验证通知数据的所有权；尚未接入实际 UI 任务或 LVGL。 */
static void notify_model(void *context, const appliance_model_t *model)
{
    fixture_t *f = (fixture_t *)context;
    if (f->notify_count == TRACE_CAPACITY) {
        f->callback_error = 1;
        return;
    }
    f->notifications[f->notify_count++] = *model;
}

static void handle_event(void *context, const comm_message_event_t *event)
{
    fixture_t *f = (fixture_t *)context;
    event_record_t *record;
    if (f->event_count == TRACE_CAPACITY) {
        f->callback_error = 1;
        return;
    }
    record = &f->events[f->event_count++];
    record->type = event->type;
    record->sequence = event->frame.sequence;
    record->business_result = appliance_manager_handle_message_event(&f->appliance, event);
}

static int setup(fixture_t *f)
{
    const comm_message_manager_config_t config = {100u, 0u, 1u};
    memset(f, 0, sizeof(*f));
    CHECK(comm_frame_queue_pthread_init_with_stats(
        &f->tx_queue, f->tx_storage, QUEUE_CAPACITY, &f->tx_stats_storage) ==
        COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(comm_frame_queue_pthread_init_with_stats(
        &f->rx_queue, f->rx_storage, QUEUE_CAPACITY, &f->rx_stats_storage) ==
        COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(comm_frame_channel_pthread_bind(&f->tx_channel, &f->tx_queue) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_channel_pthread_bind(&f->rx_channel, &f->rx_queue) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_ringbuffer_init(&f->ring, f->ring_storage, sizeof(f->ring_storage)) ==
          COMM_RINGBUFFER_OK);
    CHECK(comm_parser_stats_reset(&f->parser_stats) == COMM_PARSER_STATS_OK);
    CHECK(appliance_manager_init(&f->appliance, notify_model, f) == APPLIANCE_MANAGER_OK);
    CHECK(comm_message_manager_init_with_stats(
        &f->messages, &f->tx_channel, f->pending, 2u, &config, handle_event, f,
        &f->message_stats_storage) == COMM_MESSAGE_MANAGER_OK);
    return 0;
}

static int finish(fixture_t *f)
{
    CHECK(f->callback_error == 0);
    CHECK(comm_frame_channel_close(&f->tx_channel) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_channel_close(&f->rx_channel) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_frame_queue_pthread_destroy(&f->tx_queue) == COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(comm_frame_queue_pthread_destroy(&f->rx_queue) == COMM_FRAME_QUEUE_PTHREAD_OK);
    return 0;
}

static size_t pending_count(const fixture_t *f)
{
    return (size_t)(f->pending[0].active == 1) + (size_t)(f->pending[1].active == 1);
}

/* 模拟发送任务：先从 TX Channel 出队，再编码得到真正要传输的字节。 */
static int take_tx_wire(fixture_t *f, uint8_t *wire, size_t *size)
{
    comm_frame_t frame;
    CHECK(comm_frame_channel_pop(&f->tx_channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    CHECK(comm_codec_encode(&frame, wire, COMM_FRAME_MAX_ENCODED_SIZE, size) == COMM_CODEC_OK);
    return 0;
}

static int encode_status(uint8_t type, uint16_t sequence, uint8_t progress,
                         uint8_t *wire, size_t *size)
{
    comm_frame_t frame = {0};
    frame.version = COMM_FRAME_VERSION;
    frame.type = type;
    frame.sequence = sequence;
    frame.payload_length = APPLIANCE_STATUS_PAYLOAD_SIZE;
    frame.payload[APPLIANCE_STATUS_MESSAGE_ID_OFFSET] = APPLIANCE_MESSAGE_STATUS_SNAPSHOT;
    frame.payload[APPLIANCE_STATUS_RUN_STATE_OFFSET] = APPLIANCE_RUN_STATE_RUNNING;
    frame.payload[APPLIANCE_STATUS_PROGRAM_OFFSET] = APPLIANCE_PROGRAM_QUICK;
    frame.payload[APPLIANCE_STATUS_PROGRESS_OFFSET] = progress;
    frame.payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET] = 0x00u;
    frame.payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET + 1u] = 0x1Eu;
    frame.payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET] = 1u;
    CHECK(comm_codec_encode(&frame, wire, COMM_FRAME_MAX_ENCODED_SIZE, size) == COMM_CODEC_OK);
    return 0;
}

/* 模拟对端只读取请求字节，从解码结果取得序号，不直接访问本端 pending。 */
static int peer_reply(const uint8_t *query, size_t query_size, uint8_t progress,
                      uint8_t *reply, size_t *reply_size)
{
    comm_frame_t request;
    CHECK(comm_codec_decode(query, query_size, &request) == COMM_CODEC_OK);
    CHECK(request.type == COMM_FRAME_TYPE_REQUEST && request.sequence != 0u);
    CHECK(request.payload_length == APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE);
    CHECK(request.payload[APPLIANCE_QUERY_STATUS_MESSAGE_ID_OFFSET] ==
          APPLIANCE_MESSAGE_QUERY_STATUS);
    return encode_status(COMM_FRAME_TYPE_RESPONSE, request.sequence, progress,
                         reply, reply_size);
}

static int query_and_reply(fixture_t *f, uint64_t now_ms, uint8_t progress,
                           uint16_t *sequence, uint8_t *reply, size_t *reply_size)
{
    uint8_t query[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t query_size;
    CHECK(appliance_client_request_status(&f->messages, now_ms, sequence) ==
          COMM_MESSAGE_MANAGER_OK);
    CHECK(take_tx_wire(f, query, &query_size) == 0);
    CHECK(query_size == COMM_FRAME_MIN_ENCODED_SIZE + APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE);
    CHECK(query[COMM_FRAME_SEQUENCE_OFFSET] == (uint8_t)(*sequence >> 8u));
    CHECK(query[COMM_FRAME_SEQUENCE_OFFSET + 1u] == (uint8_t)(*sequence & 0xFFu));
    return peer_reply(query, query_size, progress, reply, reply_size);
}

/*
 * 模拟一次读取到的字节块：写入 RingBuffer，循环提取所有完整帧并投递 RX。
 * 测试块大小和帧数受控；任何缓冲溢出或投递失败都让测试失败，不静默丢帧。
 * 真实运行程序的背压/溢出恢复策略需要在后续接收任务中单独实现。
 */
static int feed_bytes(fixture_t *f, const uint8_t *data, size_t size)
{
    comm_frame_t frame;
    comm_parser_result_t result;
    CHECK(comm_ringbuffer_write(&f->ring, data, size) == COMM_RINGBUFFER_OK);
    for (;;) {
        result = comm_parser_next_with_stats(
            &f->ring, f->scratch, sizeof(f->scratch), &frame, &f->parser_stats);
        if (result == COMM_PARSER_NEED_MORE_DATA) {
            return 0;
        }
        CHECK(result == COMM_PARSER_FRAME_READY);
        CHECK(comm_frame_channel_push(&f->rx_channel, &frame, 0u) == COMM_FRAME_CHANNEL_OK);
    }
}

/* 唯一接收入口：Message Manager 只能处理从 RX Channel 取出的帧。 */
static int dispatch_rx(fixture_t *f)
{
    comm_frame_t frame;
    comm_frame_channel_result_t result;
    for (;;) {
        result = comm_frame_channel_pop(&f->rx_channel, &frame, 0u);
        if (result == COMM_FRAME_CHANNEL_TIMEOUT) {
            return 0;
        }
        CHECK(result == COMM_FRAME_CHANNEL_OK);
        CHECK(comm_message_manager_handle_frame(&f->messages, &frame) == COMM_MESSAGE_MANAGER_OK);
        CHECK(f->callback_error == 0);
    }
}

static int expect_model(fixture_t *f, uint8_t progress)
{
    appliance_model_t model;
    CHECK(appliance_manager_get_model(&f->appliance, &model) == APPLIANCE_MANAGER_OK);
    CHECK(model.run_state == APPLIANCE_RUN_STATE_RUNNING);
    CHECK(model.program == APPLIANCE_PROGRAM_QUICK);
    CHECK(model.progress_percent == progress && model.remaining_minutes == 30u);
    CHECK(model.door_locked == 1 && model.fault_code == 0u);
    CHECK(f->notify_count > 0u);
    CHECK(f->notifications[f->notify_count - 1u].progress_percent == progress);
    return 0;
}

static int expect_drained(fixture_t *f, uint64_t tx_frames, uint64_t rx_frames)
{
    comm_frame_queue_pthread_stats_t tx;
    comm_frame_queue_pthread_stats_t rx;
    CHECK(comm_frame_queue_pthread_get_stats(&f->tx_queue, &tx) == COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(comm_frame_queue_pthread_get_stats(&f->rx_queue, &rx) == COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(tx.frames_pushed == tx_frames && tx.frames_popped == tx_frames);
    CHECK(rx.frames_pushed == rx_frames && rx.frames_popped == rx_frames);
    CHECK(tx.push_timeout_count == 0u && rx.push_timeout_count == 0u);
    CHECK(comm_ringbuffer_size(&f->ring) == 0u);
    CHECK(f->parser_stats.frames_ready == rx_frames);
    return 0;
}

/* 遍历响应的每个两段拆分位置，包括同步字、长度、payload 和 CRC 内部。 */
static int test_every_split_position(void)
{
    const size_t response_size = COMM_FRAME_MIN_ENCODED_SIZE + APPLIANCE_STATUS_PAYLOAD_SIZE;
    size_t split;
    for (split = 1u; split < response_size; ++split) {
        fixture_t f;
        uint8_t reply[COMM_FRAME_MAX_ENCODED_SIZE];
        size_t reply_size;
        uint16_t sequence;
        CHECK(setup(&f) == 0);
        CHECK(query_and_reply(&f, 1000u, 40u, &sequence, reply, &reply_size) == 0);
        CHECK(reply_size == response_size);
        CHECK(feed_bytes(&f, reply, split) == 0);
        CHECK(dispatch_rx(&f) == 0);
        CHECK(f.event_count == 0u && f.notify_count == 0u && pending_count(&f) == 1u);
        CHECK(f.parser_stats.frames_ready == 0u && comm_ringbuffer_size(&f.ring) == split);
        CHECK(feed_bytes(&f, reply + split, reply_size - split) == 0);
        /* Parser 完成并入队，尚未调度 manager，不能提前更新 pending 或模型。 */
        CHECK(f.event_count == 0u && f.notify_count == 0u && pending_count(&f) == 1u);
        CHECK(dispatch_rx(&f) == 0);
        CHECK(f.event_count == 1u && f.events[0].sequence == sequence);
        CHECK(f.events[0].type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
        CHECK(f.events[0].business_result == APPLIANCE_MANAGER_OK);
        CHECK(f.notify_count == 1u && pending_count(&f) == 0u);
        CHECK(expect_model(&f, 40u) == 0);
        CHECK(expect_drained(&f, 1u, 1u) == 0);
        CHECK(f.parser_stats.discarded_bytes == 0u && f.parser_stats.decode_errors == 0u);
        CHECK(finish(&f) == 0);
    }
    return 0;
}

/* 先用可丢弃字节推进读写位置，使逐字节到达的合法帧跨越环形数组末尾。 */
static int test_bytewise_wrapped_frame(void)
{
    fixture_t f;
    uint8_t noise[COMM_FRAME_MAX_ENCODED_SIZE - 3u];
    uint8_t reply[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t reply_size;
    size_t index;
    uint16_t sequence;
    CHECK(setup(&f) == 0);
    memset(noise, 0x33, sizeof(noise));
    CHECK(feed_bytes(&f, noise, sizeof(noise)) == 0);
    CHECK(f.ring.read_index == sizeof(noise) && f.ring.write_index == sizeof(noise));
    CHECK(query_and_reply(&f, 1000u, 40u, &sequence, reply, &reply_size) == 0);
    CHECK(feed_bytes(&f, NULL, 0u) == 0);
    for (index = 0u; index < reply_size; ++index) {
        CHECK(feed_bytes(&f, &reply[index], 1u) == 0);
        CHECK(dispatch_rx(&f) == 0);
        CHECK(f.notify_count == (index + 1u == reply_size ? 1u : 0u));
    }
    CHECK(f.ring.read_index == reply_size - 3u);
    CHECK(f.parser_stats.discarded_bytes == sizeof(noise));
    CHECK(f.parser_stats.crc_errors == 0u && pending_count(&f) == 0u);
    CHECK(expect_model(&f, 40u) == 0);
    CHECK(expect_drained(&f, 1u, 1u) == 0);
    return finish(&f);
}

/* 两个请求乱序应答 + 重复应答 + 主动上报一次粘连到达，随后统一消费 RX。 */
static int test_burst_out_of_order_and_duplicates(void)
{
    fixture_t f;
    uint8_t first[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t second[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t report[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t burst[4u * (COMM_FRAME_MIN_ENCODED_SIZE + APPLIANCE_STATUS_PAYLOAD_SIZE)];
    size_t first_size;
    size_t second_size;
    size_t report_size;
    size_t size = 0u;
    uint16_t first_sequence;
    uint16_t second_sequence;
    comm_message_manager_stats_t stats;
    comm_frame_queue_pthread_stats_t rx_stats;
    CHECK(setup(&f) == 0);
    CHECK(query_and_reply(&f, 1000u, 40u, &first_sequence, first, &first_size) == 0);
    CHECK(query_and_reply(&f, 1000u, 60u, &second_sequence, second, &second_size) == 0);
    CHECK(first_sequence != second_sequence);
    CHECK(encode_status(COMM_FRAME_TYPE_REPORT, 0u, 80u, report, &report_size) == 0);
    memcpy(burst + size, second, second_size); size += second_size;
    memcpy(burst + size, first, first_size); size += first_size;
    memcpy(burst + size, first, first_size); size += first_size;
    memcpy(burst + size, report, report_size); size += report_size;
    CHECK(feed_bytes(&f, burst, size) == 0);
    CHECK(f.parser_stats.frames_ready == 4u);
    CHECK(f.event_count == 0u && f.notify_count == 0u && pending_count(&f) == 2u);
    CHECK(comm_frame_queue_pthread_get_stats(&f.rx_queue, &rx_stats) == COMM_FRAME_QUEUE_PTHREAD_OK);
    CHECK(rx_stats.frames_pushed == 4u && rx_stats.frames_popped == 0u && rx_stats.peak_size == 4u);
    /* 改写已消费的原字节块和临时缓冲，RX 中的整帧副本仍应保持独立。 */
    memset(burst, 0, sizeof(burst));
    memset(f.scratch, 0xA5, sizeof(f.scratch));
    CHECK(dispatch_rx(&f) == 0);
    CHECK(f.event_count == 4u && f.notify_count == 3u && pending_count(&f) == 0u);
    CHECK(f.events[0].sequence == second_sequence && f.events[1].sequence == first_sequence);
    CHECK(f.events[0].type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
    CHECK(f.events[1].type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
    CHECK(f.events[2].type == COMM_MESSAGE_EVENT_UNMATCHED_REPLY);
    CHECK(f.events[2].business_result == APPLIANCE_MANAGER_UNSUPPORTED_EVENT);
    CHECK(f.events[3].type == COMM_MESSAGE_EVENT_REPORT_RECEIVED);
    CHECK(f.notifications[0].progress_percent == 60u && f.notifications[1].progress_percent == 40u);
    CHECK(expect_model(&f, 80u) == 0);
    /* 相同 REPORT 仍是有效上报，但业务模型未变，不重复通知 UI。 */
    CHECK(feed_bytes(&f, report, report_size) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.notify_count == 3u && f.event_count == 5u);
    CHECK(comm_message_manager_get_stats(&f.messages, &stats) == COMM_MESSAGE_MANAGER_OK);
    CHECK(stats.requests_sent == 2u && stats.responses_matched == 2u);
    CHECK(stats.unmatched_replies == 1u && stats.reports_received == 2u);
    CHECK(expect_drained(&f, 2u, 5u) == 0);
    return finish(&f);
}

/* 坏字节不得进入业务层；后续合法回复仍能匹配原请求并更新模型。 */
static int test_corrupt_stream_recovers(void)
{
    static const uint8_t garbage[] = {0x33u, 0xAAu, 0xAAu, 0x54u, 0x00u};
    static const uint8_t oversized_header[] = {
        0xAAu, 0x55u, 0x01u, 0x02u, 0x00u, 0x10u, 0xFFu, 0xFFu
    };
    fixture_t f;
    uint8_t reply[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t bad[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t reply_size;
    size_t bad_size;
    uint16_t sequence;
    CHECK(setup(&f) == 0);
    CHECK(query_and_reply(&f, 1000u, 40u, &sequence, reply, &reply_size) == 0);
    CHECK(encode_status(COMM_FRAME_TYPE_RESPONSE, sequence, 70u, bad, &bad_size) == 0);
    bad[COMM_FRAME_PAYLOAD_OFFSET + APPLIANCE_STATUS_PROGRESS_OFFSET] ^= 1u;
    CHECK(feed_bytes(&f, garbage, sizeof(garbage)) == 0);
    CHECK(feed_bytes(&f, bad, bad_size) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.event_count == 0u && f.notify_count == 0u && pending_count(&f) == 1u);
    CHECK(f.parser_stats.crc_errors == 1u && f.parser_stats.decode_errors == 1u);
    CHECK(feed_bytes(&f, oversized_header, sizeof(oversized_header)) == 0);
    CHECK(feed_bytes(&f, reply, 2u) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.event_count == 0u && f.notify_count == 0u && pending_count(&f) == 1u);
    CHECK(feed_bytes(&f, reply + 2u, reply_size - 2u) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.parser_stats.discarded_bytes == sizeof(garbage) + bad_size + sizeof(oversized_header));
    CHECK(f.parser_stats.oversized_length_candidates == 1u);
    CHECK(f.parser_stats.decode_errors == 1u && f.parser_stats.crc_errors == 1u);
    CHECK(f.event_count == 1u && f.notify_count == 1u && pending_count(&f) == 0u);
    CHECK(expect_model(&f, 40u) == 0);
    CHECK(expect_drained(&f, 1u, 1u) == 0);
    return finish(&f);
}

/* 重发真正经过 TX 编码，字节与原请求一致；截止前/最终超时后回复分别处理。 */
static int test_retry_response(int late)
{
    fixture_t f;
    uint8_t query[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t retry[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t reply[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t query_size;
    size_t retry_size;
    size_t reply_size;
    uint16_t sequence;
    comm_message_manager_stats_t stats;
    appliance_model_t model;
    CHECK(setup(&f) == 0);
    CHECK(appliance_client_request_status(&f.messages, 1000u, &sequence) == COMM_MESSAGE_MANAGER_OK);
    CHECK(take_tx_wire(&f, query, &query_size) == 0);
    CHECK(peer_reply(query, query_size, 40u, reply, &reply_size) == 0);
    CHECK(comm_message_manager_process_timeouts(&f.messages, 1099u) == COMM_MESSAGE_MANAGER_OK);
    CHECK(comm_message_manager_process_timeouts(&f.messages, 1100u) == COMM_MESSAGE_MANAGER_OK);
    CHECK(take_tx_wire(&f, retry, &retry_size) == 0);
    CHECK(retry_size == query_size && memcmp(query, retry, query_size) == 0);
    CHECK(f.notify_count == 0u && pending_count(&f) == 1u);
    CHECK(f.pending[0].request.sequence == sequence && f.pending[0].retries_done == 1u);
    if (late) {
        CHECK(comm_message_manager_process_timeouts(&f.messages, 1200u) == COMM_MESSAGE_MANAGER_OK);
        CHECK(f.event_count == 1u && f.events[0].type == COMM_MESSAGE_EVENT_REQUEST_TIMEOUT);
        CHECK(pending_count(&f) == 0u);
    } else {
        CHECK(comm_message_manager_process_timeouts(&f.messages, 1150u) == COMM_MESSAGE_MANAGER_OK);
    }
    CHECK(feed_bytes(&f, reply, reply_size) == 0 && dispatch_rx(&f) == 0);
    CHECK(comm_message_manager_process_timeouts(&f.messages, 1300u) == COMM_MESSAGE_MANAGER_OK);
    CHECK(comm_message_manager_get_stats(&f.messages, &stats) == COMM_MESSAGE_MANAGER_OK);
    CHECK(stats.frames_sent == 2u && stats.requests_sent == 1u && stats.retries_sent == 1u);
    CHECK(stats.request_timeouts == (late ? 1u : 0u));
    CHECK(stats.responses_matched == (late ? 0u : 1u));
    CHECK(stats.unmatched_replies == (late ? 1u : 0u));
    CHECK(pending_count(&f) == 0u);
    if (late) {
        CHECK(f.event_count == 2u && f.events[1].type == COMM_MESSAGE_EVENT_UNMATCHED_REPLY);
        CHECK(f.events[1].business_result == APPLIANCE_MANAGER_UNSUPPORTED_EVENT);
        CHECK(f.notify_count == 0u);
        CHECK(appliance_manager_get_model(&f.appliance, &model) == APPLIANCE_MANAGER_OK);
        CHECK(model.run_state == APPLIANCE_RUN_STATE_OFF);
    } else {
        CHECK(f.event_count == 1u && f.notify_count == 1u);
        CHECK(expect_model(&f, 40u) == 0);
    }
    CHECK(expect_drained(&f, 2u, 1u) == 0);
    return finish(&f);
}

/* CRC 合法不等于业务合法：非法快照不更新模型，但匹配回复已结束 pending。 */
static int test_invalid_business_payload(void)
{
    fixture_t f;
    uint8_t reply[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t reply_size;
    uint16_t sequence;
    appliance_model_t model;
    comm_message_manager_stats_t stats;
    CHECK(setup(&f) == 0);
    CHECK(query_and_reply(&f, 1000u, 101u, &sequence, reply, &reply_size) == 0);
    CHECK(feed_bytes(&f, reply, reply_size) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.event_count == 1u && f.events[0].type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED);
    CHECK(f.events[0].business_result == APPLIANCE_MANAGER_MALFORMED_PAYLOAD);
    CHECK(f.notify_count == 0u && pending_count(&f) == 0u);
    CHECK(f.parser_stats.frames_ready == 1u && f.parser_stats.decode_errors == 0u);
    CHECK(appliance_manager_get_model(&f.appliance, &model) == APPLIANCE_MANAGER_OK);
    CHECK(model.run_state == APPLIANCE_RUN_STATE_OFF && model.progress_percent == 0u);
    CHECK(comm_message_manager_process_timeouts(&f.messages, 1200u) == COMM_MESSAGE_MANAGER_OK);
    CHECK(query_and_reply(&f, 1200u, 40u, &sequence, reply, &reply_size) == 0);
    CHECK(sequence == 2u);
    CHECK(feed_bytes(&f, reply, reply_size) == 0 && dispatch_rx(&f) == 0);
    CHECK(f.event_count == 2u && f.notify_count == 1u && pending_count(&f) == 0u);
    CHECK(expect_model(&f, 40u) == 0);
    CHECK(comm_message_manager_get_stats(&f.messages, &stats) == COMM_MESSAGE_MANAGER_OK);
    CHECK(stats.responses_matched == 2u && stats.retries_sent == 0u && stats.request_timeouts == 0u);
    CHECK(expect_drained(&f, 2u, 2u) == 0);
    return finish(&f);
}

int main(void)
{
    if (test_every_split_position() != 0 || test_bytewise_wrapped_frame() != 0 ||
        test_burst_out_of_order_and_duplicates() != 0 || test_corrupt_stream_recovers() != 0 ||
        test_retry_response(0) != 0 || test_retry_response(1) != 0 ||
        test_invalid_business_payload() != 0) {
        return 1;
    }
    puts("test_appliance_stream_integration: all tests passed");
    return 0;
}
