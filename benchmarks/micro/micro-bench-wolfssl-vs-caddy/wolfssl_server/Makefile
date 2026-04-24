CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -I/usr/local/include
LDFLAGS := -L/usr/local/lib -lwolfssl -lpthread

BIN := wolfssl_bench_server
SRC := wolfssl_bench_server.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) ../common/bench2_rdtsc.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)