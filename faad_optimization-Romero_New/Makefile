CC      = gcc
CFLAGS  = -Wall -Wextra -DWOLFSSL_SESSION_EXPORT -I/usr/local/include
LDFLAGS = /usr/local/lib/libwolfssl.so -Wl,-rpath,/usr/local/lib

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
