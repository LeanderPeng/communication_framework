#include "comm_message_manager.h"

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
#define CHECK_OK(call) CHECK((call) == COMM_MESSAGE_MANAGER_OK)

typedef struct {
    comm_message_manager_t manager;
    comm_frame_channel_t channel;
    comm_message_pending_t pending[2];
    comm_message_manager_stats_t stats;
    comm_message_manager_config_t config;
    comm_frame_channel_result_t send_result;
    comm_frame_channel_result_t script[2];
    size_t script_size;
    size_t script_index;
    size_t sends;
    size_t events;
    comm_frame_t last_sent;
    comm_message_event_t last_event;
    comm_message_manager_stats_t stats_at_callback;
    int matching_pending_at_callback;
} fixture_t;

static comm_frame_channel_result_t push(void *context, const comm_frame_t *frame,
                                       uint32_t timeout_ms)
{
    fixture_t *f = (fixture_t *)context;
    (void)timeout_ms;
    f->sends += 1u;
    f->last_sent = *frame;
    if (f->script_index < f->script_size) {
        return f->script[f->script_index++];
    }
    return f->send_result;
}

static comm_frame_channel_result_t pop(void *context, comm_frame_t *frame,
                                      uint32_t timeout_ms)
{
    (void)context;
    (void)frame;
    (void)timeout_ms;
    return COMM_FRAME_CHANNEL_TIMEOUT;
}

static comm_frame_channel_result_t close_channel(void *context)
{
    (void)context;
    return COMM_FRAME_CHANNEL_OK;
}

static void record_event(void *context, const comm_message_event_t *event)
{
    fixture_t *f = (fixture_t *)context;
    size_t index;
    f->events += 1u;
    f->last_event = *event;
    /* 测试只在所属线程观察内部数据，验证提交顺序，不从回调重入 manager。 */
    if (f->manager.stats != NULL) {
        f->stats_at_callback = f->stats;
    }
    f->matching_pending_at_callback = 0;
    for (index = 0u; index < 2u; ++index) {
        if (f->pending[index].active == 1 &&
            f->pending[index].request.sequence == event->frame.sequence) {
            f->matching_pending_at_callback = 1;
        }
    }
}

static int setup(fixture_t *f, int enable_stats, uint8_t retries)
{
    memset(f, 0, sizeof(*f));
    f->config.response_timeout_ms = 100u;
    f->config.send_timeout_ms = 25u;
    f->config.max_retries = retries;
    CHECK(comm_frame_channel_bind(&f->channel, f, push, pop, close_channel) ==
          COMM_FRAME_CHANNEL_OK);
    if (enable_stats) {
        CHECK_OK(comm_message_manager_init_with_stats(
            &f->manager, &f->channel, f->pending, 2u, &f->config,
            record_event, f, &f->stats));
    } else {
        CHECK_OK(comm_message_manager_init(
            &f->manager, &f->channel, f->pending, 2u, &f->config, record_event, f));
    }
    return 0;
}

static int stats_equal(const comm_message_manager_stats_t *a,
                       const comm_message_manager_stats_t *b)
{
    return a->frames_sent == b->frames_sent && a->requests_sent == b->requests_sent &&
           a->send_failures == b->send_failures &&
           a->requests_received == b->requests_received &&
           a->reports_received == b->reports_received &&
           a->responses_matched == b->responses_matched &&
           a->errors_matched == b->errors_matched &&
           a->unmatched_replies == b->unmatched_replies &&
           a->retries_sent == b->retries_sent &&
           a->request_timeouts == b->request_timeouts &&
           a->pending_full_count == b->pending_full_count &&
           a->pending_peak == b->pending_peak;
}

static int expect_stats(fixture_t *f, const comm_message_manager_stats_t *expected)
{
    comm_message_manager_stats_t snapshot;
    CHECK_OK(comm_message_manager_get_stats(&f->manager, &snapshot));
    CHECK(stats_equal(&snapshot, expected));
    return 0;
}

static int receive(fixture_t *f, uint8_t type, uint16_t sequence)
{
    comm_frame_t frame = {0};
    frame.version = COMM_FRAME_VERSION;
    frame.type = type;
    frame.sequence = sequence;
    CHECK_OK(comm_message_manager_handle_frame(&f->manager, &frame));
    return 0;
}

/* 初次请求、重发、非请求发送各有口径；乱序、重复和未知回复不能重复结束事务。 */
static int test_message_flow(void)
{
    fixture_t f;
    uint16_t first;
    uint16_t second;
    uint16_t rejected = 0xABCDu;
    comm_message_manager_stats_t expected = {0};
    comm_frame_t unknown = {0};
    CHECK(setup(&f, 1, 1u) == 0);
    CHECK(expect_stats(&f, &expected) == 0);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &first));
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &second));
    CHECK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &rejected) ==
          COMM_MESSAGE_MANAGER_PENDING_FULL);
    CHECK(rejected == 0xABCDu && f.sends == 2u && f.manager.next_sequence == 3u);
    CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, second) == 0);
    CHECK(f.stats_at_callback.responses_matched == 1u);
    CHECK(f.matching_pending_at_callback == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, second) == 0);
    CHECK(f.last_event.type == COMM_MESSAGE_EVENT_UNMATCHED_REPLY);
    CHECK(receive(&f, COMM_FRAME_TYPE_ERROR, first) == 0);
    CHECK(f.stats_at_callback.errors_matched == 1u && f.matching_pending_at_callback == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_ERROR, first) == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, 500u) == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_REQUEST, 30u) == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_REPORT, 0u) == 0);
    CHECK(f.events == 7u);
    CHECK_OK(comm_message_manager_send_response(&f.manager, 30u, NULL, 0u));
    CHECK_OK(comm_message_manager_send_report(&f.manager, NULL, 0u));
    CHECK_OK(comm_message_manager_send_error(&f.manager, 30u, NULL, 0u));
    expected.frames_sent = 5u;
    expected.requests_sent = 2u;
    expected.pending_peak = 2u;
    expected.pending_full_count = 1u;
    expected.responses_matched = 1u;
    expected.errors_matched = 1u;
    expected.unmatched_replies = 3u;
    expected.requests_received = 1u;
    expected.reports_received = 1u;
    CHECK(expect_stats(&f, &expected) == 0);
    unknown.type = 0xFFu;
    CHECK(comm_message_manager_handle_frame(&f.manager, &unknown) ==
          COMM_MESSAGE_MANAGER_UNSUPPORTED_FRAME_TYPE);
    CHECK(f.events == 7u);
    CHECK(expect_stats(&f, &expected) == 0);
    return 0;
}

static int test_send_failures(void)
{
    fixture_t f;
    const comm_frame_channel_result_t failures[] = {
        COMM_FRAME_CHANNEL_TIMEOUT, COMM_FRAME_CHANNEL_CLOSED,
        COMM_FRAME_CHANNEL_BACKEND_ERROR
    };
    const comm_message_manager_result_t results[] = {
        COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT, COMM_MESSAGE_MANAGER_CHANNEL_CLOSED,
        COMM_MESSAGE_MANAGER_CHANNEL_ERROR
    };
    comm_message_manager_stats_t expected = {0};
    uint16_t sequence = 0xABCDu;
    size_t index;
    CHECK(setup(&f, 1, 1u) == 0);
    for (index = 0u; index < 3u; ++index) {
        f.send_result = failures[index];
        CHECK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &sequence) ==
              results[index]);
        CHECK(comm_message_manager_send_response(&f.manager, 1u, NULL, 0u) == results[index]);
        CHECK(comm_message_manager_send_report(&f.manager, NULL, 0u) == results[index]);
        CHECK(comm_message_manager_send_error(&f.manager, 1u, NULL, 0u) == results[index]);
        expected.send_failures += 4u;
        CHECK(expect_stats(&f, &expected) == 0);
        CHECK(sequence == 0xABCDu && f.manager.next_sequence == 1u);
        CHECK(f.pending[0].active == 0 && f.pending[1].active == 0 && f.events == 0u);
    }
    CHECK(comm_message_manager_send_request(&f.manager, NULL, 1u, 0u, &sequence) ==
          COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_send_report(&f.manager, NULL, 1u) ==
          COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_send_response(&f.manager, 0u, NULL, 0u) ==
          COMM_MESSAGE_MANAGER_INVALID_SEQUENCE);
    CHECK(comm_message_manager_send_error(&f.manager, 0u, NULL, 0u) ==
          COMM_MESSAGE_MANAGER_INVALID_SEQUENCE);
    CHECK(comm_message_manager_send_request(&f.manager, (const uint8_t *)"x",
          COMM_FRAME_MAX_PAYLOAD_SIZE + 1u, 0u, &sequence) ==
          COMM_MESSAGE_MANAGER_PAYLOAD_TOO_LARGE);
    CHECK(f.sends == 12u);
    CHECK(expect_stats(&f, &expected) == 0);
    return 0;
}

/* 一轮返回首个通道错误，但仍逐项记录后续失败、成功重发及最终超时。 */
static int test_retry_partial_failure_and_final_timeout(void)
{
    fixture_t f;
    uint16_t first;
    uint16_t second;
    comm_message_manager_stats_t expected = {0};
    CHECK(setup(&f, 1, 1u) == 0);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &first));
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &second));
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 99u));
    CHECK(f.sends == 2u);
    f.script_size = 2u;
    f.script[0] = COMM_FRAME_CHANNEL_TIMEOUT;
    f.script[1] = COMM_FRAME_CHANNEL_CLOSED;
    CHECK(comm_message_manager_process_timeouts(&f.manager, 100u) ==
          COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT);
    CHECK(f.stats.send_failures == 2u && f.stats.retries_sent == 0u);
    CHECK(f.pending[0].deadline_ms == 100u && f.pending[1].deadline_ms == 100u);
    f.script_index = 0u;
    f.script[0] = COMM_FRAME_CHANNEL_OK;
    f.script[1] = COMM_FRAME_CHANNEL_TIMEOUT;
    CHECK(comm_message_manager_process_timeouts(&f.manager, 100u) ==
          COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT);
    CHECK(f.pending[0].retries_done == 1u && f.pending[0].deadline_ms == 200u);
    CHECK(f.pending[1].retries_done == 0u && f.pending[1].deadline_ms == 100u);
    CHECK(f.events == 0u && f.stats.frames_sent == 3u && f.stats.retries_sent == 1u);
    f.send_result = COMM_FRAME_CHANNEL_CLOSED;
    CHECK(comm_message_manager_process_timeouts(&f.manager, 200u) ==
          COMM_MESSAGE_MANAGER_CHANNEL_CLOSED);
    CHECK(f.last_event.type == COMM_MESSAGE_EVENT_REQUEST_TIMEOUT);
    CHECK(f.last_event.frame.sequence == first && f.matching_pending_at_callback == 0);
    CHECK(f.stats_at_callback.request_timeouts == 1u);
    f.send_result = COMM_FRAME_CHANNEL_BACKEND_ERROR;
    CHECK(comm_message_manager_process_timeouts(&f.manager, 200u) ==
          COMM_MESSAGE_MANAGER_CHANNEL_ERROR);
    f.send_result = COMM_FRAME_CHANNEL_OK;
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 250u));
    CHECK(f.last_sent.sequence == second && f.pending[1].deadline_ms == 350u);
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 350u));
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 400u));
    CHECK(f.events == 2u && f.pending[0].active == 0 && f.pending[1].active == 0);
    CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, first) == 0);
    expected.frames_sent = 4u;
    expected.requests_sent = 2u;
    expected.send_failures = 5u;
    expected.retries_sent = 2u;
    expected.request_timeouts = 2u;
    expected.unmatched_replies = 1u;
    expected.pending_peak = 2u;
    CHECK(expect_stats(&f, &expected) == 0);
    return 0;
}

static int test_reset_period_and_manager_reset(void)
{
    fixture_t f;
    uint16_t first;
    uint16_t second;
    comm_message_manager_stats_t expected = {0};
    CHECK(setup(&f, 1, 1u) == 0);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &first));
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &second));
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 100u));
    CHECK_OK(comm_message_manager_reset_stats(&f.manager));
    expected.pending_peak = 2u;
    CHECK(expect_stats(&f, &expected) == 0);
    CHECK(f.pending[0].active == 1 && f.pending[1].active == 1);
    CHECK(f.pending[0].request.sequence == first && f.pending[1].request.sequence == second);
    CHECK(f.pending[0].deadline_ms == 200u && f.pending[1].deadline_ms == 200u);
    CHECK(f.pending[0].retries_done == 1u && f.pending[1].retries_done == 1u);
    CHECK(f.manager.next_sequence == 3u && f.sends == 4u && f.events == 0u);
    CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, first) == 0);
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 200u));
    expected.responses_matched = 1u;
    expected.request_timeouts = 1u;
    CHECK(expect_stats(&f, &expected) == 0);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 300u, &first));
    expected.frames_sent = 1u;
    expected.requests_sent = 1u;
    CHECK_OK(comm_message_manager_reset(&f.manager));
    CHECK(f.pending[0].active == 0 && f.pending[1].active == 0);
    CHECK(f.manager.next_sequence == 1u && f.events == 2u);
    CHECK(expect_stats(&f, &expected) == 0);
    CHECK_OK(comm_message_manager_reset_stats(&f.manager));
    memset(&expected, 0, sizeof(expected));
    CHECK(expect_stats(&f, &expected) == 0);
    return 0;
}

static int test_optional_and_invalid_state(void)
{
    fixture_t f;
    comm_message_manager_t uninitialized = {0};
    comm_message_manager_stats_t output = {.frames_sent = 77u};
    comm_message_manager_stats_t sentinel = output;
    comm_message_manager_stats_t before;
    uint16_t sequence;
    CHECK(setup(&f, 0, 0u) == 0);
    CHECK(f.manager.stats == NULL);
    CHECK(comm_message_manager_get_stats(&f.manager, &output) ==
          COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_reset_stats(&f.manager) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &sequence));
    CHECK_OK(comm_message_manager_process_timeouts(&f.manager, 100u));
    CHECK(f.last_event.type == COMM_MESSAGE_EVENT_REQUEST_TIMEOUT);
    CHECK(stats_equal(&output, &sentinel));
    CHECK(comm_message_manager_get_stats(NULL, &output) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_get_stats(&uninitialized, NULL) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_get_stats(&uninitialized, &output) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_reset_stats(NULL) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_reset_stats(&uninitialized) == COMM_MESSAGE_MANAGER_INVALID_STATE);

    CHECK(setup(&f, 1, 1u) == 0);
    CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &sequence));
    CHECK_OK(comm_message_manager_get_stats(&f.manager, &before));
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, f.pending,
          2u, &f.config, record_event, &f, NULL) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(NULL, &f.channel, f.pending,
          2u, &f.config, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(&f.manager, NULL, f.pending,
          2u, &f.config, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, NULL,
          2u, &f.config, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, f.pending,
          2u, NULL, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, f.pending,
          2u, &f.config, NULL, &f, &f.stats) == COMM_MESSAGE_MANAGER_NULL_ARGUMENT);
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, f.pending,
          0u, &f.config, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_INVALID_CONFIG);
    f.config.response_timeout_ms = 0u;
    CHECK(comm_message_manager_init_with_stats(&f.manager, &f.channel, f.pending,
          2u, &f.config, record_event, &f, &f.stats) == COMM_MESSAGE_MANAGER_INVALID_CONFIG);
    CHECK(expect_stats(&f, &before) == 0);
    CHECK(f.pending[0].active == 1 && f.manager.next_sequence == 2u);

    f.pending[0].active = 7; /* 测试注入损坏状态。 */
    CHECK(comm_message_manager_get_stats(&f.manager, &output) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_reset_stats(&f.manager) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_send_request(&f.manager, NULL, 0u, 0u, &sequence) ==
          COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_process_timeouts(&f.manager, 100u) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(comm_message_manager_handle_frame(&f.manager, &f.last_sent) == COMM_MESSAGE_MANAGER_INVALID_STATE);
    CHECK(stats_equal(&output, &sentinel));
    CHECK(stats_equal(&f.stats, &before));
    CHECK(f.sends == 1u && f.events == 0u);
    CHECK_OK(comm_message_manager_reset(&f.manager));
    CHECK(expect_stats(&f, &before) == 0);
    return 0;
}

/* 所有累计计数先从 MAX-1 到 MAX，再次发生同类事件仍保持饱和。 */
static int test_saturation(void)
{
    fixture_t f;
    uint16_t first;
    uint16_t second;
    uint16_t third;
    size_t index;
    comm_message_manager_stats_t expected = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, 2u
    };
    CHECK(setup(&f, 1, 1u) == 0);
    f.stats = expected; /* 测试专用边界注入，单任务，无并发读写。 */
    f.stats.frames_sent -= 1u;
    f.stats.requests_sent -= 1u;
    f.stats.send_failures -= 1u;
    f.stats.requests_received -= 1u;
    f.stats.reports_received -= 1u;
    f.stats.responses_matched -= 1u;
    f.stats.errors_matched -= 1u;
    f.stats.unmatched_replies -= 1u;
    f.stats.retries_sent -= 1u;
    f.stats.request_timeouts -= 1u;
    f.stats.pending_full_count -= 1u;
    f.stats.pending_peak = 0u;
    for (index = 0u; index < 2u; ++index) {
        uint64_t now_ms = index * 1000u;
        CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, now_ms, &first));
        CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, now_ms, &second));
        CHECK(comm_message_manager_send_request(&f.manager, NULL, 0u, now_ms, &third) ==
              COMM_MESSAGE_MANAGER_PENDING_FULL);
        f.send_result = COMM_FRAME_CHANNEL_TIMEOUT;
        CHECK(comm_message_manager_send_report(&f.manager, NULL, 0u) == COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT);
        f.send_result = COMM_FRAME_CHANNEL_OK;
        CHECK_OK(comm_message_manager_process_timeouts(&f.manager, now_ms + 100u));
        CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, first) == 0);
        CHECK(receive(&f, COMM_FRAME_TYPE_RESPONSE, first) == 0);
        CHECK(receive(&f, COMM_FRAME_TYPE_ERROR, second) == 0);
        CHECK(receive(&f, COMM_FRAME_TYPE_REQUEST, 123u) == 0);
        CHECK(receive(&f, COMM_FRAME_TYPE_REPORT, 0u) == 0);
        CHECK_OK(comm_message_manager_send_request(&f.manager, NULL, 0u, now_ms + 100u, &third));
        CHECK_OK(comm_message_manager_process_timeouts(&f.manager, now_ms + 200u));
        CHECK_OK(comm_message_manager_process_timeouts(&f.manager, now_ms + 300u));
        CHECK(expect_stats(&f, &expected) == 0);
    }
    return 0;
}

int main(void)
{
    if (test_message_flow() != 0 || test_send_failures() != 0 ||
        test_retry_partial_failure_and_final_timeout() != 0 ||
        test_reset_period_and_manager_reset() != 0 ||
        test_optional_and_invalid_state() != 0 || test_saturation() != 0) {
        return 1;
    }
    puts("test_comm_message_manager_stats: all tests passed");
    return 0;
}
