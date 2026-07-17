#include "client_stream.h"
#include "client.h"
#include "common.h"
#include <ev.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <quicly/streambuf.h>

// the streambuf must come first: quicly_streambuf_create() allocates this whole
// struct and the streambuf callbacks cast stream->data straight to it.
typedef struct
{
    quicly_streambuf_t streambuf;
    uint64_t bytes_received;
    int index;
} client_stream;

// upload streams are unidirectional and never use a streambuf: the payload is
// synthesised in the emit callback rather than buffered.
typedef struct
{
    uint64_t bytes_acked;
    int index;
} client_upload_stream;

static int current_second = 0;
static ev_timer report_timer;
static bool first_receive = true;
static bool first_ack = true;
static int runtime_s = 10;

static client_stream *streams[QPERF_MAX_STREAMS];
static int num_streams = 0;

static client_upload_stream *upload_streams[QPERF_MAX_STREAMS];
static int num_upload_streams = 0;


void format_size(char *dst, double bytes)
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

static void report_cb(EV_P_ ev_timer *w, int revents)
{
    char size_str[100];
    uint64_t total = 0;
    bool upload = num_upload_streams > 0;
    const char *what = upload ? "bytes acked" : "bytes received";
    int n = upload ? num_upload_streams : num_streams;

    for(int i = 0; i < n; ++i) {
        total += upload ? upload_streams[i]->bytes_acked : streams[i]->bytes_received;
    }

    format_size(size_str, total);
    printf("second %i: %s (%" PRIu64 " %s)\n", current_second, size_str, total, what);

    if(n > 1) {
        for(int i = 0; i < n; ++i) {
            uint64_t b = upload ? upload_streams[i]->bytes_acked : streams[i]->bytes_received;
            int idx = upload ? upload_streams[i]->index : streams[i]->index;
            format_size(size_str, b);
            printf("  stream %i: %s (%" PRIu64 " %s)\n", idx, size_str, b, what);
        }
    }

    fflush(stdout);
    ++current_second;

    for(int i = 0; i < n; ++i) {
        if(upload) {
            upload_streams[i]->bytes_acked = 0;
        } else {
            streams[i]->bytes_received = 0;
        }
    }

    if(current_second >= runtime_s) {
        quit_client();
    }
}

static void start_report_timer()
{
    ev_timer_init(&report_timer, report_cb, 1.0, 1.0);
    ev_timer_start(ev_default_loop(0), &report_timer);
}

static void client_stream_send_stop(quicly_stream_t *stream, quicly_error_t err)
{
    fprintf(stderr, "received STOP_SENDING: %li\n", err);
}

static void client_stream_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    client_stream *s = stream->data;

    if(first_receive) {
        for(int i = 0; i < num_streams; ++i) {
            streams[i]->bytes_received = 0;
        }
        first_receive = false;
        start_report_timer();
        on_first_byte();
    }

    if(len == 0) {
        return;
    }

    s->bytes_received += len;
    quicly_stream_sync_recvbuf(stream, len);
}

static void client_stream_receive_reset(quicly_stream_t *stream, quicly_error_t err)
{
    fprintf(stderr, "received RESET_STREAM: %li\n", err);
}

static void client_stream_destroy(quicly_stream_t *stream, quicly_error_t err)
{
    client_stream *s = stream->data;

    for(int i = 0; i < num_streams; ++i) {
        if(streams[i] == s) {
            memmove(&streams[i], &streams[i + 1], (num_streams - i - 1) * sizeof(streams[0]));
            --num_streams;
            break;
        }
    }

    quicly_streambuf_destroy(stream, err);
}

static void client_upload_destroy(quicly_stream_t *stream, quicly_error_t err)
{
    client_upload_stream *s = stream->data;

    for(int i = 0; i < num_upload_streams; ++i) {
        if(upload_streams[i] == s) {
            memmove(&upload_streams[i], &upload_streams[i + 1], (num_upload_streams - i - 1) * sizeof(upload_streams[0]));
            --num_upload_streams;
            break;
        }
    }

    free(s);
}

// acked bytes, not emitted bytes: this is what actually reached the server, and
// it excludes anything still in flight or since retransmitted.
static void client_upload_send_shift(quicly_stream_t *stream, size_t delta)
{
    client_upload_stream *s = stream->data;

    if(first_ack) {
        first_ack = false;
        start_report_timer();
        on_first_ack();
    }

    s->bytes_acked += delta;
}

static void client_upload_send_emit(quicly_stream_t *stream, size_t off, void *dst, size_t *len, int *wrote_all)
{
    // never done: the run ends on the -t timer, not on a byte count
    *wrote_all = 0;
    memset(dst, 0x58, *len);
}

static void client_upload_send_stop(quicly_stream_t *stream, quicly_error_t err)
{
    fprintf(stderr, "received STOP_SENDING: %li\n", (long)err);
}

static const quicly_stream_callbacks_t client_upload_callbacks = {
    &client_upload_destroy,
    &client_upload_send_shift,
    &client_upload_send_emit,
    &client_upload_send_stop,
    &quicly_stream_noop_on_receive,
    &quicly_stream_noop_on_receive_reset
};

static const quicly_stream_callbacks_t client_stream_callbacks = {
    &client_stream_destroy,
    &quicly_streambuf_egress_shift,
    &quicly_streambuf_egress_emit,
    &client_stream_send_stop,
    &client_stream_receive,
    &client_stream_receive_reset
};


quicly_error_t client_on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    // we only ever open unidirectional streams to upload on, and we advertise no
    // uni credit to the server, so a uni stream here is always one of ours.
    if(quicly_stream_is_unidirectional(stream->stream_id)) {
        client_upload_stream *s = calloc(1, sizeof(*s));
        assert(s != NULL);
        stream->data = s;
        stream->callbacks = &client_upload_callbacks;

        if(num_upload_streams < QPERF_MAX_STREAMS) {
            s->index = num_upload_streams;
            upload_streams[num_upload_streams++] = s;
        } else {
            s->index = -1;
        }

        return 0;
    }

    int ret = quicly_streambuf_create(stream, sizeof(client_stream));
    assert(ret == 0);
    stream->callbacks = &client_stream_callbacks;

    client_stream *s = stream->data;
    s->bytes_received = 0;

    // this fires for peer-initiated streams too, which -P does not bound, so the
    // array needs a real check rather than an assert() that Release compiles out.
    // an unregistered stream still works, it just goes unreported.
    if(num_streams < QPERF_MAX_STREAMS) {
        s->index = num_streams;
        streams[num_streams++] = s;
    } else {
        s->index = -1;
    }

    return 0;
}


void client_set_quit_after(int seconds)
{
    runtime_s = seconds;
}
