CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -I/usr/local/include
LDFLAGS := -L/usr/local/lib -lwolfssl -lpthread

BIN := bench2_vanilla_proxy
SRC := bench2_vanilla_proxy.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) ../common/bench2_rdtsc.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)
