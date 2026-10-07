#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "appliance_client.h"
#include "appliance_manager.h"
#include "comm_codec.h"
#include "comm_frame_channel_pthread.h"
#include "comm_parser.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAPACITY 2u
#define REPORT_BURST 16u
#define MANAGER_TICK_MS 10u
#define RX_WAIT_MS 20u
#define RUN_LIMIT_MS 5000u
#define STREAM_DURATION_MS 1000u
#define MAX_RECONNECTS 2u

typedef enum {
    MODE_NORMAL, MODE_RETRY, MODE_TIMEOUT, MODE_RX_FULL,
    MODE_STOP_FULL, MODE_DISCONNECT, MODE_IDLE, MODE_STREAM,
    MODE_RECONNECT, MODE_RECONNECT_TIMEOUT, MODE_RECONNECT_EXHAUSTED
} demo_mode_t;

typedef enum { CONNECTION_OFFLINE, CONNECTION_SYNCING, CONNECTION_ONLINE } connection_state_t;
typedef enum { FAILURE_NONE, FAILURE_INTERNAL, FAILURE_TRANSPORT } failure_kind_t;

/* 跨连接保存业务快照；state 表明快照是否已经在当前连接上重新确认。 */
typedef struct {
    appliance_manager_t appliance;
    connection_state_t state;
    unsigned int notifications;
    int quiet;
} application_t;

typedef struct {
    comm_ringbuffer_t ring;
    uint8_t storage[COMM_FRAME_MAX_ENCODED_SIZE];
    uint8_t scratch[COMM_FRAME_MAX_ENCODED_SIZE];
} stream_reader_t;

typedef struct {
    /* 初始化后到所有线程 join 前，通道绑定、描述符和模式保持不变。 */
    demo_mode_t mode;
    unsigned int session;
    application_t *app;
    int sockets[2];
    comm_frame_queue_pthread_t tx_queue;
    comm_frame_queue_pthread_t rx_queue;
    comm_frame_queue_pthread_stats_t tx_stats;
    comm_frame_queue_pthread_stats_t rx_stats;
    comm_frame_t tx_storage[QUEUE_CAPACITY];
    comm_frame_t rx_storage[QUEUE_CAPACITY];
    comm_frame_channel_t tx;
    comm_frame_channel_t rx;
    pthread_t workers[3];
    size_t started;
    pthread_mutex_t stop_mutex;
    int stopping;
    int failed;
    failure_kind_t failure_kind;
    int mutex_ready;
    int tx_ready;
    int rx_ready;

    /* 以下对象只由主线程初始化、调用和读取，工作线程不访问。 */
    comm_message_manager_t messages;
    comm_message_pending_t pending[4];
    comm_message_manager_stats_t message_stats;
    unsigned int notification_base;
    unsigned int responses;
    unsigned int reports;
    unsigned int timeouts;
    unsigned int unmatched;
    unsigned int ignored_reports;
    uint16_t sync_sequence;
    uint16_t followup_sequence;
    /* 仅模拟设备线程写，主线程必须 join 后才能读取。 */
    unsigned int peer_sent_reports;
    int business_failed;
} runtime_t;

/* 信号处理器只记录退出请求，不调用 mutex、队列或日志接口。 */
static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
    interrupted = signal_number;
}

/* 同步对象损坏后无法保证安全回收，示例直接终止，避免继续访问失效对象。 */
static void check_pthread(int result, const char *operation)
{
    if (result != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(result));
        abort();
    }
}

static int is_stopping(runtime_t *r)
{
    int result;
    check_pthread(pthread_mutex_lock(&r->stop_mutex), "stop lock");
    result = r->stopping;
    check_pthread(pthread_mutex_unlock(&r->stop_mutex), "stop unlock");
    return result;
}

/*
 * 所有退出原因共用此入口，可重复调用。先唤醒等待者，暂不 close 描述符；
 * 必须等所有线程 join 后才能 close/destroy，避免描述符复用和释放后访问。
 * 这是中止式停止，不承诺发送完队列内的帧，也不将未完成请求计为最终超时。
 */
static void stop_with_kind(runtime_t *r, const char *reason, failure_kind_t kind)
{
    check_pthread(pthread_mutex_lock(&r->stop_mutex), "stop lock");
    if (!r->stopping) {
        r->stopping = 1;
        r->failure_kind = kind;
        if (reason != NULL) {
            r->failed = 1;
            fprintf(stderr, "runtime stopped: %s\n", reason);
        }
        if (comm_frame_channel_close(&r->tx) != COMM_FRAME_CHANNEL_OK ||
            comm_frame_channel_close(&r->rx) != COMM_FRAME_CHANNEL_OK) {
            fprintf(stderr, "queue close failed\n");
            abort();
        }
        /* shutdown 可唤醒阻塞的 recv/send；正常停止产生的 EOF 不再报错。 */
        if (shutdown(r->sockets[0], SHUT_RDWR) != 0 && errno != ENOTCONN) {
            perror("shutdown local");
            abort();
        }
        if (shutdown(r->sockets[1], SHUT_RDWR) != 0 && errno != ENOTCONN) {
            perror("shutdown peer");
            abort();
        }
    }
    check_pthread(pthread_mutex_unlock(&r->stop_mutex), "stop unlock");
}

static void stop_runtime(runtime_t *r, const char *reason)
{
    stop_with_kind(r, reason, reason == NULL ? FAILURE_NONE : FAILURE_INTERNAL);
}

static void transport_failed(runtime_t *r, const char *reason)
{
    stop_with_kind(r, reason, FAILURE_TRANSPORT);
}

static int reconnect_mode(demo_mode_t mode)
{
    return mode == MODE_RECONNECT || mode == MODE_RECONNECT_TIMEOUT ||
           mode == MODE_RECONNECT_EXHAUSTED;
}

/* 连接状态和模型一样，只允许主线程读写；状态变化需要单独通知未来的 UI。 */
static void set_connection_state(application_t *app, connection_state_t state,
                                  unsigned int session)
{
    static const char *const names[] = {"offline", "syncing", "online"};
    app->state = state;
    printf("connection: session=%u state=%s\n", session, names[state]);
}

static int monotonic_ms(runtime_t *r, uint64_t *output)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        stop_runtime(r, "clock_gettime failed");
        return 0;
    }
    *output = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 1;
}

static int init_reader(runtime_t *r, stream_reader_t *reader)
{
    if (comm_ringbuffer_init(&reader->ring, reader->storage,
                            sizeof(reader->storage)) != COMM_RINGBUFFER_OK) {
        stop_runtime(r, "reader init failed");
        return 0;
    }
    return 1;
}

/* 每个接收线程拥有独立的 RingBuffer 和临时区，不跨线程共享裸字节。 */
static int receive_frame(runtime_t *r, int fd, stream_reader_t *reader,
                         comm_frame_t *frame)
{
    uint8_t input[7];
    while (!is_stopping(r)) {
        size_t available;
        ssize_t count;
        comm_parser_result_t parsed = comm_parser_next(
            &reader->ring, reader->scratch, sizeof(reader->scratch), frame);
        if (parsed == COMM_PARSER_FRAME_READY) {
            return 1;
        }
        if (parsed != COMM_PARSER_NEED_MORE_DATA) {
            stop_runtime(r, "parser failed");
            return 0;
        }
        /* 先解析已有数据，再按剩余容量读取，保留半帧且不覆盖未消费字节。 */
        available = comm_ringbuffer_free_space(&reader->ring);
        if (available == 0u) {
            stop_runtime(r, "full ring cannot make progress");
            return 0;
        }
        if (available > sizeof(input)) {
            available = sizeof(input);
        }
        count = recv(fd, input, available, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            transport_failed(r, count == 0 ? "transport EOF" : "transport recv failed");
            return 0;
        }
        if (comm_ringbuffer_write(&reader->ring, input, (size_t)count) != COMM_RINGBUFFER_OK) {
            stop_runtime(r, "ring write failed");
            return 0;
        }
    }
    return 0;
}

/* 短写只推进实际成功的字节数；断连返回错误，不让 SIGPIPE 终止进程。 */
static int send_bytes(runtime_t *r, int fd, const uint8_t *wire, size_t size)
{
    size_t offset = 0u;
    while (offset < size && !is_stopping(r)) {
        size_t chunk = size - offset;
        ssize_t count;
        /* 刻意小块发送用于演示；socket 不保证一次 send 对应一次 recv。 */
        if (chunk > 3u) {
            chunk = 3u;
        }
        count = send(fd, wire + offset, chunk, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            transport_failed(r, "transport send failed");
            return 0;
        }
        offset += (size_t)count;
    }
    return offset == size;
}

static int send_frame(runtime_t *r, int fd, const comm_frame_t *frame)
{
    uint8_t wire[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t size;
    if (comm_codec_encode(frame, wire, sizeof(wire), &size) != COMM_CODEC_OK) {
        stop_runtime(r, "encode failed");
        return 0;
    }
    return send_bytes(r, fd, wire, size);
}

static void *tx_main(void *context)
{
    runtime_t *r = context;
    while (!is_stopping(r)) {
        comm_frame_t frame;
        comm_frame_channel_result_t result = comm_frame_channel_pop(
            &r->tx, &frame, COMM_FRAME_CHANNEL_WAIT_FOREVER);
        if (result == COMM_FRAME_CHANNEL_CLOSED) {
            break;
        }
        if (result != COMM_FRAME_CHANNEL_OK) {
            stop_runtime(r, "TX pop failed");
            break;
        }
        if (!send_frame(r, r->sockets[0], &frame)) {
            break;
        }
    }
    return NULL;
}

static void *rx_main(void *context)
{
    runtime_t *r = context;
    stream_reader_t reader;
    comm_frame_t frame;
    if (!init_reader(r, &reader)) {
        return NULL;
    }
    while (receive_frame(r, r->sockets[0], &reader, &frame)) {
        int warned = 0;
        /*
         * 满队列时保留当前完整帧并暂停读取，等待空位或停止。流式 socket 的
         * 背压会向对端传播；无硬件流控 UART 不能直接照搬此不丢帧策略。
         */
        while (!is_stopping(r)) {
            comm_frame_channel_result_t result = comm_frame_channel_push(&r->rx, &frame, RX_WAIT_MS);
            if (result == COMM_FRAME_CHANNEL_OK) {
                break;
            }
            if (result == COMM_FRAME_CHANNEL_CLOSED) {
                return NULL;
            }
            if (result != COMM_FRAME_CHANNEL_TIMEOUT) {
                stop_runtime(r, "RX push failed");
                return NULL;
            }
            if (!warned) {
                puts("RX full: retaining frame and waiting for space");
                warned = 1;
            }
        }
    }
    return NULL;
}

/* 模拟设备按协议字段构造业务快照，不访问本端 manager 或模型。 */
static comm_frame_t status_frame(uint8_t type, uint16_t sequence, uint8_t progress)
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
    frame.payload[APPLIANCE_STATUS_REMAINING_MINUTES_OFFSET + 1u] = 30u;
    frame.payload[APPLIANCE_STATUS_DOOR_LOCKED_OFFSET] = 1u;
    return frame;
}

static int peer_status(runtime_t *r, uint8_t type, uint16_t sequence, uint8_t progress)
{
    comm_frame_t frame = status_frame(type, sequence, progress);
    return send_frame(r, r->sockets[1], &frame);
}

/* 故意只发送半个响应后结束写端，接收方必须丢弃残帧并取消对应事务。 */
static void peer_cut_response(runtime_t *r, uint16_t sequence)
{
    comm_frame_t frame = status_frame(COMM_FRAME_TYPE_RESPONSE, sequence, 99u);
    uint8_t wire[COMM_FRAME_MAX_ENCODED_SIZE];
    size_t size;
    if (comm_codec_encode(&frame, wire, sizeof(wire), &size) != COMM_CODEC_OK) {
        stop_runtime(r, "peer encode failed");
        return;
    }
    if (send_bytes(r, r->sockets[1], wire, size / 2u)) {
        puts("peer: disconnecting in the middle of a response");
        if (shutdown(r->sockets[1], SHUT_WR) != 0) {
            stop_runtime(r, "peer shutdown failed");
        }
    }
}

/* 持续上报期间不回复第二次查询，末尾发送迟到回复作为流结束标记。 */
static int peer_stream(runtime_t *r, uint16_t sequence)
{
    uint64_t start;
    uint64_t now;
    if (!monotonic_ms(r, &start)) {
        return 0;
    }
    do {
        uint8_t progress = (uint8_t)((r->peer_sent_reports + 1u) % 101u);
        if (!peer_status(r, COMM_FRAME_TYPE_REPORT, 0u, progress)) {
            return 0;
        }
        ++r->peer_sent_reports;
        if (!monotonic_ms(r, &now)) {
            return 0;
        }
    } while (now - start < STREAM_DURATION_MS && !is_stopping(r));
    return peer_status(r, COMM_FRAME_TYPE_RESPONSE, sequence, 99u);
}

static void *peer_main(void *context)
{
    runtime_t *r = context;
    stream_reader_t reader;
    comm_frame_t frame;
    unsigned int requests = 0u;
    if (!init_reader(r, &reader)) {
        return NULL;
    }
    while (receive_frame(r, r->sockets[1], &reader, &frame)) {
        unsigned int i;
        if (frame.type != COMM_FRAME_TYPE_REQUEST || frame.sequence == 0u ||
            frame.payload_length != APPLIANCE_QUERY_STATUS_PAYLOAD_SIZE ||
            frame.payload[APPLIANCE_QUERY_STATUS_MESSAGE_ID_OFFSET] != APPLIANCE_MESSAGE_QUERY_STATUS) {
            stop_runtime(r, "peer received invalid query");
            break;
        }
        ++requests;
        printf("peer: query seq=%u, received=%u\n", (unsigned int)frame.sequence, requests);
        if (r->mode == MODE_DISCONNECT) {
            if (shutdown(r->sockets[1], SHUT_RDWR) != 0) {
                stop_runtime(r, "peer shutdown failed");
            }
            return NULL;
        }
        if (r->mode == MODE_TIMEOUT || (r->mode == MODE_RETRY && requests == 1u)) {
            puts("peer: intentionally withholding reply");
            continue;
        }
        if (r->mode == MODE_RECONNECT_EXHAUSTED ||
            (reconnect_mode(r->mode) && r->session == 1u && requests == 2u)) {
            peer_cut_response(r, frame.sequence);
            return NULL;
        }
        if (reconnect_mode(r->mode) && r->session > 1u) {
            if (requests == 1u) {
                /* 同步前的上报不能使旧缓存变为在线，未知旧序号也不能更新模型。 */
                if (!peer_status(r, COMM_FRAME_TYPE_REPORT, 0u, 99u) ||
                    !peer_status(r, COMM_FRAME_TYPE_RESPONSE, 2u, 99u)) {
                    return NULL;
                }
            }
            if (r->mode == MODE_RECONNECT_TIMEOUT) {
                continue;
            }
            if (!peer_status(r, COMM_FRAME_TYPE_RESPONSE, frame.sequence, 80u)) {
                return NULL;
            }
            continue;
        }
        if (r->mode == MODE_STREAM && frame.sequence != 1u) {
            if (requests == 2u && !peer_stream(r, frame.sequence)) {
                return NULL;
            }
            continue;
        }
        if ((r->mode == MODE_RX_FULL || r->mode == MODE_STOP_FULL) && requests == 1u) {
            for (i = 1u; i <= REPORT_BURST; ++i) {
                if (!peer_status(r, COMM_FRAME_TYPE_REPORT, 0u, (uint8_t)i)) {
                    return NULL;
                }
            }
        }
        if (!peer_status(r, COMM_FRAME_TYPE_RESPONSE, frame.sequence, 40u)) {
            break;
        }
    }
    return NULL;
}

static void model_changed(void *context, const appliance_model_t *model)
{
    application_t *app = context;
    ++app->notifications;
    if (app->quiet && app->notifications != 1u && app->notifications % 1000u != 0u) {
        return;
    }
    /* 此处只打印；未来接入 UI 时必须复制快照并交给 UI 任务。 */
    printf("model: progress=%u, remaining=%u, door=%d\n",
           (unsigned int)model->progress_percent,
           (unsigned int)model->remaining_minutes, model->door_locked);
}

static const char *event_name(comm_message_event_type_t type)
{
    switch (type) {
    case COMM_MESSAGE_EVENT_REQUEST_RECEIVED: return "request";
    case COMM_MESSAGE_EVENT_RESPONSE_RECEIVED: return "response";
    case COMM_MESSAGE_EVENT_REPORT_RECEIVED: return "report";
    case COMM_MESSAGE_EVENT_ERROR_RECEIVED: return "error";
    case COMM_MESSAGE_EVENT_REQUEST_TIMEOUT: return "timeout";
    case COMM_MESSAGE_EVENT_UNMATCHED_REPLY: return "unmatched_reply";
    default: return "unknown";
    }
}

static void message_event(void *context, const comm_message_event_t *event)
{
    runtime_t *r = context;
    if (r->mode != MODE_STREAM || event->type != COMM_MESSAGE_EVENT_REPORT_RECEIVED) {
        printf("manager: event=%s, seq=%u, retries=%u\n", event_name(event->type),
               (unsigned int)event->frame.sequence, (unsigned int)event->retries_done);
    }
    if (event->type == COMM_MESSAGE_EVENT_REQUEST_TIMEOUT) {
        ++r->timeouts;
    } else if (event->type == COMM_MESSAGE_EVENT_ERROR_RECEIVED) {
        /* ERROR 已结束对应事务，不能继续等一个永远不会触发的 pending 超时。 */
        r->business_failed = 1;
    } else if (event->type == COMM_MESSAGE_EVENT_UNMATCHED_REPLY) {
        ++r->unmatched;
    } else if (event->type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED ||
               event->type == COMM_MESSAGE_EVENT_REPORT_RECEIVED) {
        if (event->type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED) {
            ++r->responses;
        } else {
            ++r->reports;
            if (r->app->state != CONNECTION_ONLINE) {
                ++r->ignored_reports;
                return;
            }
        }
        if (appliance_manager_handle_message_event(&r->app->appliance, event) != APPLIANCE_MANAGER_OK) {
            r->business_failed = 1;
        } else if (event->type == COMM_MESSAGE_EVENT_RESPONSE_RECEIVED &&
                   event->frame.sequence == r->sync_sequence && r->app->state == CONNECTION_SYNCING) {
            set_connection_state(r->app, CONNECTION_ONLINE, r->session);
        }
    }
}

/* 部分初始化失败也统一走 destroy；尚未启动线程，无需 shutdown 或 join。 */
static int init_runtime(runtime_t *r, demo_mode_t mode, application_t *app, unsigned int session)
{
    const comm_message_manager_config_t config = {200u, 0u, 1u};
    memset(r, 0, sizeof(*r));
    r->mode = mode;
    r->app = app;
    r->session = session;
    r->notification_base = app->notifications;
    r->sockets[0] = -1;
    r->sockets[1] = -1;
    if (pthread_mutex_init(&r->stop_mutex, NULL) != 0) {
        return 0;
    }
    r->mutex_ready = 1;
    if (comm_frame_queue_pthread_init_with_stats(&r->tx_queue, r->tx_storage,
            QUEUE_CAPACITY, &r->tx_stats) != COMM_FRAME_QUEUE_PTHREAD_OK) {
        return 0;
    }
    r->tx_ready = 1;
    if (comm_frame_queue_pthread_init_with_stats(&r->rx_queue, r->rx_storage,
            QUEUE_CAPACITY, &r->rx_stats) != COMM_FRAME_QUEUE_PTHREAD_OK) {
        return 0;
    }
    r->rx_ready = 1;
    if (comm_frame_channel_pthread_bind(&r->tx, &r->tx_queue) != COMM_FRAME_CHANNEL_OK ||
        comm_frame_channel_pthread_bind(&r->rx, &r->rx_queue) != COMM_FRAME_CHANNEL_OK ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, r->sockets) != 0 ||
        comm_message_manager_init_with_stats(&r->messages, &r->tx, r->pending, 4u,
            &config, message_event, r, &r->message_stats) != COMM_MESSAGE_MANAGER_OK) {
        return 0;
    }
    return 1;
}

static int start_runtime(runtime_t *r)
{
    void *(*entries[])(void *) = {tx_main, rx_main, peer_main};
    size_t i;
    for (i = 0u; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        int result = pthread_create(&r->workers[i], NULL, entries[i], r);
        if (result != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(result));
            stop_runtime(r, "worker startup failed");
            return 0;
        }
        ++r->started;
    }
    return 1;
}

static void join_runtime(runtime_t *r)
{
    size_t i;
    for (i = 0u; i < r->started; ++i) {
        check_pthread(pthread_join(r->workers[i], NULL), "pthread_join");
    }
    r->started = 0u;
}

static void destroy_runtime(runtime_t *r)
{
    size_t i;
    for (i = 0u; i < 2u; ++i) {
        if (r->sockets[i] >= 0) {
            /* Linux close 即使被信号中断也不重试，避免误关复用的描述符。 */
            if (close(r->sockets[i]) != 0) {
                perror("close");
            }
            r->sockets[i] = -1;
        }
    }
    if ((r->rx_ready && comm_frame_queue_pthread_destroy(&r->rx_queue) != COMM_FRAME_QUEUE_PTHREAD_OK) ||
        (r->tx_ready && comm_frame_queue_pthread_destroy(&r->tx_queue) != COMM_FRAME_QUEUE_PTHREAD_OK)) {
        fprintf(stderr, "queue destroy failed\n");
        abort();
    }
    if (r->mutex_ready) {
        check_pthread(pthread_mutex_destroy(&r->stop_mutex), "stop mutex destroy");
    }
}

static int queue_snapshot(runtime_t *r, comm_frame_queue_pthread_stats_t *stats)
{
    if (comm_frame_queue_pthread_get_stats(&r->rx_queue, stats) != COMM_FRAME_QUEUE_PTHREAD_OK) {
        stop_runtime(r, "RX snapshot failed");
        return 0;
    }
    return 1;
}

/* 主线程就是 manager 所属任务，每轮最多处理一帧，持续输入也不能饿死定时检查。 */
static void run_manager(runtime_t *r)
{
    uint64_t start;
    uint64_t now;
    int hold_rx = r->mode == MODE_RX_FULL || r->mode == MODE_STOP_FULL;
    if (!monotonic_ms(r, &start)) {
        return;
    }
    if (r->mode != MODE_IDLE && appliance_client_request_status(&r->messages, start, &r->sync_sequence)
        != COMM_MESSAGE_MANAGER_OK) {
        stop_runtime(r, "initial query rejected by TX channel");
        return;
    }
    puts("manager: running");
    while (!is_stopping(r) && !interrupted) {
        comm_message_manager_result_t result;
        if (!monotonic_ms(r, &now)) {
            return;
        }
        if (now - start >= RUN_LIMIT_MS) {
            stop_runtime(r, "demo deadline exceeded");
            return;
        }
        if (r->mode == MODE_IDLE && now - start >= 200u) {
            break;
        }
        if (hold_rx) {
            comm_frame_queue_pthread_stats_t stats;
            if (!queue_snapshot(r, &stats)) {
                return;
            }
            /* 用已发生的等待超时作为握手，不依赖猜测线程是否已阻塞。 */
            if (stats.push_timeout_count != 0u) {
                if (r->mode == MODE_STOP_FULL) {
                    puts("manager: stopping with RX producer under backpressure");
                    break;
                }
                puts("manager: resume RX consumption");
                hold_rx = 0;
            }
        }
        if (hold_rx) {
            const struct timespec delay = {0, MANAGER_TICK_MS * 1000000L};
            if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
                stop_runtime(r, "nanosleep failed");
                return;
            }
        } else {
            comm_frame_t frame;
            comm_frame_channel_result_t received = comm_frame_channel_pop(&r->rx, &frame, MANAGER_TICK_MS);
            if (received == COMM_FRAME_CHANNEL_CLOSED) {
                break;
            }
            if (received == COMM_FRAME_CHANNEL_OK) {
                if (is_stopping(r)) {
                    break;
                }
                if (comm_message_manager_handle_frame(&r->messages, &frame) != COMM_MESSAGE_MANAGER_OK) {
                    stop_runtime(r, "handle frame failed");
                    return;
                }
            } else if (received != COMM_FRAME_CHANNEL_TIMEOUT) {
                stop_runtime(r, "manager RX pop failed");
                return;
            }
        }
        if (is_stopping(r) || !monotonic_ms(r, &now)) {
            return;
        }
        result = comm_message_manager_process_timeouts(&r->messages, now);
        /* TX 满时不阻塞 manager；保留到期事务，下轮再尝试重发。 */
        if (result != COMM_MESSAGE_MANAGER_OK && result != COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT) {
            stop_runtime(r, "process timeouts failed");
            return;
        }
        if (r->business_failed) {
            stop_runtime(r, "business reply rejected");
            return;
        }
        if (r->timeouts != 0u && r->mode != MODE_TIMEOUT && r->mode != MODE_STREAM) {
            stop_runtime(r, r->app->state == CONNECTION_SYNCING ?
                         "state synchronization timed out" : "query timed out");
            return;
        }
        if (r->mode == MODE_STREAM ||
            (reconnect_mode(r->mode) && r->session == 1u)) {
            if (r->app->state == CONNECTION_ONLINE && r->followup_sequence == 0u) {
                result = appliance_client_request_status(&r->messages, now, &r->followup_sequence);
                if (result != COMM_MESSAGE_MANAGER_OK && result != COMM_MESSAGE_MANAGER_CHANNEL_TIMEOUT) {
                    stop_runtime(r, "followup query failed");
                    return;
                }
            }
            if (r->mode == MODE_STREAM && r->unmatched != 0u) {
                if (r->timeouts != 1u || r->reports == 0u) {
                    stop_runtime(r, "stream did not exercise timeout handling");
                    return;
                }
                break;
            }
            continue;
        }
        if ((r->mode == MODE_TIMEOUT && r->timeouts == 1u) ||
            (r->mode != MODE_TIMEOUT && r->responses == 1u &&
             (r->mode != MODE_RX_FULL || r->reports == REPORT_BURST))) {
            break;
        }
    }
    stop_runtime(r, NULL);
}

/* manager 由主线程拥有；join 后清除旧事务，不把断线取消冒充请求最终超时。 */
static unsigned int cancel_pending(runtime_t *r)
{
    size_t i;
    unsigned int cancelled = 0u;
    for (i = 0u; i < r->messages.pending_capacity; ++i) {
        if (r->pending[i].active) {
            ++cancelled;
        }
    }
    if (comm_message_manager_reset(&r->messages) != COMM_MESSAGE_MANAGER_OK) {
        fprintf(stderr, "manager reset failed\n");
        abort();
    }
    return cancelled;
}

/* 不再访问已销毁的运行对象；分段等待允许信号中断重连退避。 */
static int reconnect_delay(unsigned int attempt)
{
    unsigned int i;
    for (i = 0u; i < attempt * 10u && !interrupted; ++i) {
        const struct timespec delay = {0, 10000000L};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            perror("reconnect delay");
            return 0;
        }
    }
    return !interrupted;
}

/* 新连接建立前彻底回收旧连接，旧队列、线程和半帧不能跨连接复用。 */
static int run_sessions(demo_mode_t mode, const char *name, application_t *app)
{
    unsigned int session;
    for (session = 1u; ; ++session) {
        runtime_t runtime;
        comm_message_manager_stats_t messages;
        comm_frame_queue_pthread_stats_t rx;
        appliance_model_t model;
        failure_kind_t failure;
        unsigned int cancelled;
        int status;
        if (interrupted) {
            return 0;
        }
        if (!init_runtime(&runtime, mode, app, session)) {
            fprintf(stderr, "runtime initialization failed\n");
            destroy_runtime(&runtime);
            return 1;
        }
        set_connection_state(app, CONNECTION_SYNCING, session);
        if (start_runtime(&runtime)) {
            run_manager(&runtime);
        }
        stop_runtime(&runtime, NULL);
        set_connection_state(app, CONNECTION_OFFLINE, session);
        join_runtime(&runtime);
        /* 所有旧任务已退出，才能检查原因、读取统计和销毁旧连接。 */
        failure = runtime.failure_kind;
        status = runtime.failed ? 1 : 0;
        cancelled = cancel_pending(&runtime);
        if (comm_message_manager_get_stats(&runtime.messages, &messages) != COMM_MESSAGE_MANAGER_OK ||
            !queue_snapshot(&runtime, &rx) ||
            appliance_manager_get_model(&app->appliance, &model) != APPLIANCE_MANAGER_OK) {
            status = 1;
            failure = FAILURE_INTERNAL;
        } else {
            printf("summary: mode=%s session=%u responses=%u reports=%u notifications=%u timeouts=%u "
                   "retries=%" PRIu64 " rx_wait_timeouts=%" PRIu64 " unmatched=%u ignored_reports=%u "
                   "cancelled_pending=%u cached_progress=%u peer_reports=%u\n",
                   name, session, runtime.responses, runtime.reports,
                   app->notifications - runtime.notification_base, runtime.timeouts,
                   messages.retries_sent, rx.push_timeout_count, runtime.unmatched,
                   runtime.ignored_reports, cancelled, (unsigned int)model.progress_percent,
                   runtime.peer_sent_reports);
            if (mode == MODE_STREAM && !interrupted && failure == FAILURE_NONE &&
                (runtime.reports != runtime.peer_sent_reports || messages.retries_sent != 1u)) {
                fprintf(stderr, "stream accounting mismatch\n");
                status = 1;
                failure = FAILURE_INTERNAL;
            }
        }
        destroy_runtime(&runtime);
        puts("runtime: all workers joined; resources released");
        if (interrupted || !reconnect_mode(mode) || failure != FAILURE_TRANSPORT) {
            return status;
        }
        if (session > MAX_RECONNECTS) {
            fprintf(stderr, "reconnect: attempt limit reached\n");
            return 1;
        }
        printf("reconnect: attempt=%u delay_ms=%u\n", session, session * 100u);
        if (!reconnect_delay(session)) {
            return interrupted ? 0 : 1;
        }
    }
}

int main(int argc, char **argv)
{
    static const char *const names[] = {
        "normal", "retry", "timeout", "rx-full", "stop-full", "disconnect", "idle",
        "stream", "reconnect", "reconnect-timeout", "reconnect-exhausted"
    };
    application_t app = {0};
    struct sigaction action;
    size_t mode = 0u;
    int status;
    if (argc > 1) {
        for (mode = 0u; mode < sizeof(names) / sizeof(names[0]); ++mode) {
            if (strcmp(argv[1], names[mode]) == 0) {
                break;
            }
        }
    }
    if (argc > 2 || mode == sizeof(names) / sizeof(names[0])) {
        fprintf(stderr, "usage: %s [normal|retry|timeout|rx-full|stop-full|disconnect|idle|"
                "stream|reconnect|reconnect-timeout|reconnect-exhausted]\n", argv[0]);
        return 2;
    }
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    app.quiet = mode == MODE_STREAM;
    if (appliance_manager_init(&app.appliance, model_changed, &app) != APPLIANCE_MANAGER_OK) {
        fprintf(stderr, "application initialization failed\n");
        return 1;
    }
    status = run_sessions((demo_mode_t)mode, names[mode], &app);
    return interrupted ? 128 + (int)interrupted : status;
}
