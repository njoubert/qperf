#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "client.h"

#define DEFAULT_RECV_WINDOW (16 * 1024 * 1024)

static void usage(const char *cmd)
{
    printf("Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -c target            run as client and connect to target server\n"
            "  --cc [reno,cubic]    congestion control algorithm to use (default reno)\n"
            "  -e                   measure time for connection establishment and first byte only\n"
            "  -g                   enable UDP generic segmentation offload\n"
            "  --iw initial-window  initial window to use (default 10)\n"
            "  -l log-file          file to log tls secrets\n"
            "  -p                   port to listen on/connect to (default 18080)\n"
            "  -P streams           number of parallel streams to request over the single\n"
            "                       connection (default 1, max %i). All streams share one\n"
            "                       congestion window; the per-second report shows each\n"
            "                       stream plus the total.\n"
            "  --recv-window bytes  how much data the server may send us before waiting for\n"
            "                       our acknowledgements -- a window on data in flight, NOT a\n"
            "                       cap on total bytes transferred. Throughput cannot exceed\n"
            "                       this divided by the round-trip time, so raise it on long\n"
            "                       fat links. Client-side only; this is the QUIC\n"
            "                       initial_max_data transport parameter. Accepts K/M/G\n"
            "                       suffixes (default 16M)\n"
            "  -s  address          listen as server on address\n"
            "  -t time (s)          run for X seconds (default 10s)\n"
            "  -u                   upload instead of download: the client sends and the\n"
            "                       SERVER measures and reports throughput. Default is\n"
            "                       download (server sends, client reports). Upload streams\n"
            "                       are unidirectional and require a server built with\n"
            "                       upload support.\n"
            "  -h                   print this help\n"
            "\n",
           cmd, QPERF_MAX_STREAMS);
}

static struct option long_options[] =
{
    {"cc", required_argument, NULL, 0},
    {"iw", required_argument, NULL, 1},
    {"recv-window", required_argument, NULL, 2},
    {NULL, 0, NULL, 0}
};

// accepts a plain byte count with an optional K/M/G (1024-based) suffix
static bool parse_size(const char *str, uint64_t *out)
{
    char *end;
    unsigned long long value;
    uint64_t multiplier = 1;

    errno = 0;
    value = strtoull(str, &end, 10);
    if(errno != 0 || end == str) {
        return false;
    }

    if(*end != '\0') {
        switch(*end) {
        case 'k': case 'K': multiplier = 1024; break;
        case 'm': case 'M': multiplier = 1024 * 1024; break;
        case 'g': case 'G': multiplier = 1024 * 1024 * 1024; break;
        default: return false;
        }
        if(end[1] != '\0') {
            return false;
        }
    }

    if(value > UINT64_MAX / multiplier) {
        return false;
    }

    *out = (uint64_t)value * multiplier;
    return true;
}

int main(int argc, char** argv)
{
    int port = 18080;
    bool server_mode = false;
    const char *host = NULL;
    const char *address = NULL;
    int runtime_s = 10;
    int ch;
    bool ttfb_only = false;
    bool gso = false;
    const char *logfile = NULL;
    const char *cc = "reno";
    int iw = 10;
    int num_streams = 1;
    uint64_t recv_window = DEFAULT_RECV_WINDOW;
    bool upload = false;

    while ((ch = getopt_long(argc, argv, "c:egl:p:P:s:t:uh", long_options, NULL)) != -1) {
        switch (ch) {
        case 0:
            if(strcmp(optarg, "reno") != 0 && strcmp(optarg, "cubic") != 0) {
                fprintf(stderr, "invalid argument passed to --cc\n");
                exit(1);
            }
            cc = optarg;
            break;
        case 1:
            iw = (intptr_t)optarg;
            if (sscanf(optarg, "%" SCNu32, &iw) != 1) {
                fprintf(stderr, "invalid argument passed to --iw\n");
                exit(1);
            }
            break;
        case 2:
            // 1024 is quicly's minimum useful window; below it the connection stalls
            if(!parse_size(optarg, &recv_window) || recv_window < 1024 || recv_window > (1ULL << 62) - 1) {
                fprintf(stderr, "invalid argument passed to --recv-window, expected 1024..2^62-1 bytes with an optional K/M/G suffix\n");
                exit(1);
            }
            break;
        case 'c':
            host = optarg;
            break;
        case 'e':
            ttfb_only = true;
            break;
        case 'g':
            #ifdef __linux__
                gso = true;
                printf("using UDP GSO, requires kernel >= 4.18\n");
            #else
                fprintf(stderr, "UDP GSO only supported on linux\n");
                exit(1);
            #endif
            break;
        case 'l':
            logfile = optarg;
            break;
        case 'p':
            if(sscanf(optarg, "%u", &port) < 0 || port > 65535) {
                fprintf(stderr, "invalid argument passed to -p\n");
                exit(1);
            }
            break;
        case 'P':
            if(sscanf(optarg, "%u", &num_streams) != 1 || num_streams < 1 || num_streams > QPERF_MAX_STREAMS) {
                fprintf(stderr, "invalid argument passed to -P, expected 1..%i\n", QPERF_MAX_STREAMS);
                exit(1);
            }
            break;
        case 's':
            address = optarg;
            server_mode = true;
            break;
        case 't':
            if(sscanf(optarg, "%u", &runtime_s) != 1 || runtime_s < 1) {
                fprintf(stderr, "invalid argument passed to -t\n");
                exit(1);
            }
            break;
        case 'u':
            upload = true;
            break;
        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if(server_mode && host != NULL) {
        printf("cannot use -c in server mode\n");
        exit(1);
    }

    if(server_mode && num_streams != 1) {
        printf("cannot use -P in server mode, the client requests the streams\n");
        exit(1);
    }

    if(server_mode && recv_window != DEFAULT_RECV_WINDOW) {
        printf("cannot use --recv-window in server mode, it is what the client advertises to the server\n");
        exit(1);
    }

    if(server_mode && upload) {
        printf("cannot use -u in server mode, the client chooses the direction\n");
        exit(1);
    }

    if(upload && ttfb_only) {
        printf("cannot use -e with -u, there is no first byte to receive when uploading\n");
        exit(1);
    }

    if(!server_mode && host == NULL) {
        usage(argv[0]);
        exit(1);
    }


    char port_char[16];
    sprintf(port_char, "%d", port);
    return server_mode ?
                run_server(address, port_char, gso, logfile, cc, iw, "server.crt", "server.key") :
                run_client(port_char, gso, logfile, cc, iw, host, runtime_s, ttfb_only, num_streams, recv_window, upload);
}