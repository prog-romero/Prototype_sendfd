CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -I/usr/local/include
LDFLAGS := -L/usr/local/lib -lwolfssl -lpthread -lm

BIN := bench_tls_decrypt_memio
SRC := bench_tls_decrypt_memio.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) common/bench2_rdtsc.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)
