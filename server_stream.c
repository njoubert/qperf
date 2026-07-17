#include "server_stream.h"
#include "common.h"

#include <ev.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <quicly/streambuf.h>

typedef struct
{
    uint64_t target_offset;
    uint64_t acked_offset;
    quicly_stream_t *stream;
    // the stats below are connection-wide, so only one stream per connection
    // reports them -- otherwise a -P run prints N identical copies per second
    bool reports;
    int report_id;
    int report_second;
    uint64_t report_num_packets_sent;
    uint64_t report_num_packets_lost;
    uint64_t total_num_packets_sent;
    uint64_t total_num_packets_lost;
    ev_timer report_timer;
} server_stream;

// the receiving half of an upload run. mirrors client_stream.c's download report:
// the receiver is the side that can measure goodput.
typedef struct
{
    uint64_t bytes_received;
    int index;
} server_upload_stream;

static int report_counter = 0;

static server_upload_stream *upload_streams[QPERF_MAX_STREAMS];
static int num_upload_streams = 0;
static int upload_second = 0;
static ev_timer upload_report_timer;
static bool upload_reporting = false;

static void format_size(char *dst, double bytes)
{
    bytes *= 8;
    const char *suffixes[] = {"bit/s", "kbit/s", "mbit/s", "gbit/s"};
    int i = 0;
    while(i < 4 && bytes > 1024) {
        bytes /= 1024;
        i++;
    }
    sprintf(dst, "%.4g %s", bytes, suffixes[i]);
}

static void upload_report_cb(EV_P_ ev_timer *w, int revents)
{
    char size_str[100];
    uint64_t total = 0;

    for(int i = 0; i < num_upload_streams; ++i) {
        total += upload_streams[i]->bytes_received;
    }

    format_size(size_str, total);
    printf("upload second %i: %s (%" PRIu64 " bytes received)\n", upload_second, size_str, total);

    if(num_upload_streams > 1) {
        for(int i = 0; i < num_upload_streams; ++i) {
            format_size(size_str, upload_streams[i]->bytes_received);
            printf("  stream %i: %s (%" PRIu64 " bytes received)\n", upload_streams[i]->index, size_str,
                   upload_streams[i]->bytes_received);
        }
    }

    fflush(stdout);
    ++upload_second;

    for(int i = 0; i < num_upload_streams; ++i) {
        upload_streams[i]->bytes_received = 0;
    }
}

static void server_upload_destroy(quicly_stream_t *stream, quicly_error_t err)
{
    server_upload_stream *s = stream->data;

    for(int i = 0; i < num_upload_streams; ++i) {
        if(upload_streams[i] == s) {
            memmove(&upload_streams[i], &upload_streams[i + 1], (num_upload_streams - i - 1) * sizeof(upload_streams[0]));
            --num_upload_streams;
            break;
        }
    }

    if(num_upload_streams == 0 && upload_reporting) {
        ev_timer_stop(EV_DEFAULT, &upload_report_timer);
        upload_reporting = false;
        printf("upload finished\n");
        fflush(stdout);
    }

    free(s);
}

static void server_upload_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    server_upload_stream *s = stream->data;

    if(!upload_reporting) {
        upload_reporting = true;
        upload_second = 0;
        for(int i = 0; i < num_upload_streams; ++i) {
            upload_streams[i]->bytes_received = 0;
        }
        ev_timer_init(&upload_report_timer, upload_report_cb, 1.0, 1.0);
        ev_timer_start(EV_DEFAULT, &upload_report_timer);
        // stream count is not final here: peer streams register as their first
        // data arrives, so later streams may still be joining this report.
        printf("upload started, receiving\n");
    }

    // len is already deduplicated by quicly_recvstate_update, so each byte of
    // stream data is counted exactly once regardless of reordering/retransmits.
    s->bytes_received += len;
    quicly_stream_sync_recvbuf(stream, len);
}

static void server_upload_receive_reset(quicly_stream_t *stream, quicly_error_t err)
{
    fprintf(stderr, "upload stream reset: %li\n", (long)err);
}

static const quicly_stream_callbacks_t server_upload_callbacks = {
    &server_upload_destroy,
    &quicly_stream_noop_on_send_shift,
    &quicly_stream_noop_on_send_emit,
    &quicly_stream_noop_on_send_stop,
    &server_upload_receive,
    &server_upload_receive_reset
};

static void print_report(server_stream *s)
{
    quicly_stats_t stats;
    quicly_get_stats(s->stream->conn, &stats);
    s->report_num_packets_sent = stats.num_packets.sent - s->total_num_packets_sent;
    s->report_num_packets_lost = stats.num_packets.lost - s->total_num_packets_lost;
    s->total_num_packets_sent = stats.num_packets.sent;
    s->total_num_packets_lost = stats.num_packets.lost;
    printf("connection %i second %i send window: %"PRIu32" packets sent: %"PRIu64" packets lost: %"PRIu64"\n", s->report_id, s->report_second, stats.cc.cwnd, s->report_num_packets_sent, s->report_num_packets_lost);
    fflush(stdout);
    ++s->report_second;
}

static void server_report_cb(EV_P, ev_timer *w, int revents)
{
    print_report((server_stream*)w->data);
}

static void server_stream_destroy(quicly_stream_t *stream, quicly_error_t err)
{
    server_stream *s = (server_stream*)stream->data;
    if(s->reports) {
        print_report(s);
        printf("connection %i total packets sent: %"PRIu64" total packets lost: %"PRIu64"\n", s->report_id, s->total_num_packets_sent, s->total_num_packets_lost);
        ev_timer_stop(EV_DEFAULT, &s->report_timer);
    }
    free(s);
}

static void server_stream_send_shift(quicly_stream_t *stream, size_t delta)
{
    server_stream *s = stream->data;
    s->acked_offset += delta;
}

static void server_stream_send_emit(quicly_stream_t *stream, size_t off, void *dst, size_t *len, int *wrote_all)
{
    server_stream *s = stream->data;
    uint64_t data_off = s->acked_offset + off;

    if(data_off + *len < s->target_offset) {
        *wrote_all = 0;
    } else {
        printf("done sending\n");
        *wrote_all = 1;
        *len = s->target_offset - data_off;
        assert(data_off + *len == s->target_offset);
    }

    memset(dst, 0x58, *len);
}

static void server_stream_send_stop(quicly_stream_t *stream, quicly_error_t err)
{
    printf("server_stream_send_stop stream-id=%li\n", stream->stream_id);
    fprintf(stderr, "received STOP_SENDING: %li\n", err);
}

static void server_stream_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    //print_escaped((const char*)src, len);
    quicly_stream_sync_recvbuf(stream, len);

    if(quicly_recvstate_transfer_complete(&stream->recvstate)) {
        server_stream *s = (server_stream*)stream->data;
        printf("request received on stream %"PRIi64", sending data\n", (int64_t)stream->stream_id);
        quicly_stream_sync_sendbuf(stream, 1);
        if(s->reports) {
            ev_timer_start(EV_DEFAULT, &s->report_timer);
        }
    }
}

static void server_stream_receive_reset(quicly_stream_t *stream, quicly_error_t err)
{
    printf("server_stream_receive_reset stream-id=%li\n", stream->stream_id);
    fprintf(stderr, "received RESET_STREAM: %li\n", err);
}

static const quicly_stream_callbacks_t server_stream_callbacks = {
    &server_stream_destroy,
    &server_stream_send_shift,
    &server_stream_send_emit,
    &server_stream_send_stop,
    &server_stream_receive,
    &server_stream_receive_reset
};

quicly_error_t server_on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    // a client-initiated unidirectional stream means the client is uploading to us
    if(quicly_stream_is_unidirectional(stream->stream_id)) {
        server_upload_stream *s = calloc(1, sizeof(*s));
        assert(s != NULL);
        stream->data = s;
        stream->callbacks = &server_upload_callbacks;

        if(num_upload_streams < QPERF_MAX_STREAMS) {
            s->index = num_upload_streams;
            upload_streams[num_upload_streams++] = s;
        } else {
            s->index = -1;
        }

        return 0;
    }

    server_stream *s = malloc(sizeof(server_stream));
    s->target_offset = UINT64_MAX;
    s->acked_offset = 0;
    s->stream = stream;
    // stream 0 is the first bidi stream a qperf client opens on a connection, so
    // it owns that connection's reporting for the connection's whole lifetime
    s->reports = stream->stream_id == 0;
    s->report_id = s->reports ? report_counter++ : -1;
    s->report_second = 0;
    s->report_num_packets_sent = 0;
    s->report_num_packets_lost = 0;
    s->total_num_packets_sent = 0;
    s->total_num_packets_lost = 0;
    ev_timer_init(&s->report_timer, server_report_cb, 1.0, 1.0);
    s->report_timer.data = s;

    stream->data = s;
    stream->callbacks = &server_stream_callbacks;

    return 0;
}
