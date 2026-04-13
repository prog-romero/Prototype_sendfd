CC ?= gcc

WOLFSSL_ROOT ?= ../../../../wolfssl
WOLFSSL_LIB_DIR ?= $(WOLFSSL_ROOT)/src/.libs
WOLFSSL_INCLUDE ?= $(WOLFSSL_ROOT)

CFLAGS += -O2 -g -Wall -Wextra -std=c11 -I$(WOLFSSL_INCLUDE)
LDFLAGS += -L$(WOLFSSL_LIB_DIR) -Wl,-rpath,'$$ORIGIN/../../../../wolfssl/src/.libs'
LDLIBS += -lwolfssl -lpthread -lm -ldl

TARGET := tls_proxy

.PHONY: all clean

all: $(TARGET)

$(TARGET): tls_proxy.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
