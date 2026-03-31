CC      = gcc
CFLAGS  = -Wall -Wextra -I/usr/local/include
LDFWOLFSSL_LIB = /usr/local/lib/libwolfssl.so
LDFLAGS = $(LDFWOLFSSL_LIB) -Wl,-rpath,/usr/local/lib -lhttp_parser

all: gateway worker_sum worker_product classic_proxy

gateway: gateway.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

worker_sum: worker_sum.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

worker_product: worker_product.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

classic_proxy: classic_proxy.c common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f gateway worker_sum worker_product classic_proxy

.PHONY: all clean
