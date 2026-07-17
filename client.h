#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "common.h"

int run_client(const char* port, bool gso, const char *logfile, const char *cc, int iw, const char *host, int runtime_s, bool ttfb_only, int num_streams, uint64_t recv_window, bool upload);
void quit_client();

void on_first_byte();
void on_first_ack();
