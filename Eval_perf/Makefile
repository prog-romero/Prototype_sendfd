CC=gcc
WOLFSSL_LIB=/usr/local/lib/libwolfssl.so
WOLFSSL_INCLUDE=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/wolfssl
CFLAGS=-Wall -Wextra -O2 -g -I$(WOLFSSL_INCLUDE) -DQUIET
LDFLAGS=-L/usr/local/lib -lwolfssl -lpthread

all: direct_server plain_worker

direct_server: direct_server.c
	$(CC) $(CFLAGS) direct_server.c -o direct_server $(LDFLAGS)

plain_worker: plain_worker.c
	$(CC) $(CFLAGS) plain_worker.c -o plain_worker

clean:
	rm -f direct_server plain_worker
