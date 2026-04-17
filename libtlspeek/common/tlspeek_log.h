#ifndef TLSPEEK_LOG_H
#define TLSPEEK_LOG_H

#include <stdio.h>

#ifdef TLSPEEK_ENABLE_VERBOSE_LOGS
#define TLSPEEK_VLOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define TLSPEEK_VLOG(...) ((void)0)
#endif

#endif