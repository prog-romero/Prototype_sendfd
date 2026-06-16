/*
 * timing_fn_ka_worker_https.c — Keep-Alive HTTPS integration worker (Epoll optimized)
 */
#define _GNU_SOURCE

#ifndef HAVE_SECRET_CALLBACK
#define HAVE_SECRET_CALLBACK
#endif
#ifndef WOLFSSL_KEYLOG_EXPORT
#define WOLFSSL_KEYLOG_EXPORT
#endif

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <tlspeek/tlspeek.h>
#include <tlspeek/sendfd.h>
#include <tlspeek/unix_socket.h>

/* ── Constants matching gateway's payload_https.go ── */
#define HTTPMIGRATE_HTTPS_MAGIC    0x484D4B53U   /* 'HMKS' */
#define HTTPMIGRATE_MAGIC_HTTP     0x484D4B41U   /* 'HMKA' — fallback */
#define HTTPMIGRATE_VERSION        1U
#define HTTPMIGRATE_TARGET_LEN     128

/* Base payload: first 160 bytes — matches KAPayload in Go */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t  top1_set;
    uint8_t  _pad[7];
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} httpmigrate_ka_base_t;

/* Full HTTPS payload = base + tlspeek_serial_t */
typedef struct {
    httpmigrate_ka_base_t base;
    tlspeek_serial_t      serial;
} httpmigrate_ka_payload_https_t;

#define CNTFRQ 1000000000ULL

static uint64_t get_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int recvfd2_https(int unix_sock, int *fd1, int *fd2, httpmigrate_ka_payload_https_t *pl)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 2)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct iovec iov = { .iov_base = pl, .iov_len = sizeof(*pl) };
    struct msghdr msg = {
        .msg_iov        = &iov,  .msg_iovlen     = 1,
        .msg_control    = cmsg_buf, .msg_controllen = sizeof(cmsg_buf),
    };
    ssize_t n = recvmsg(unix_sock, &msg, 0);
    if (n <= 0) return -1;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
        return -1;
    int *fds = (int *)CMSG_DATA(cm);
    size_t nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    if (nfds < 2) { for (size_t i=0;i<nfds;i++) close(fds[i]); return -1; }
    *fd1 = fds[0]; *fd2 = fds[1];
    for (size_t i = 2; i < nfds; i++) close(fds[i]);
    return 0;
}

static int get_container_ip(char *buf, size_t sz)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return -1;
    int found = 0;
    for (struct ifaddrs *a = ifa; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(a->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)a->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, (socklen_t)sz)) { found=1; break; }
    }
    freeifaddrs(ifa);
    return found ? 0 : -1;
}

static ssize_t find_subseq(const unsigned char *b,size_t l,const char *n){size_t nl=strlen(n);if(!nl||l<nl)return -1;for(size_t i=0;i+nl<=l;i++)if(memcmp(b+i,n,nl)==0)return(ssize_t)i;return -1;}
static long long parse_cl(const char *h,size_t hl){const char*p=h,*e=h+hl;while(p<e){const char*nl=memchr(p,'\n',(size_t)(e-p));size_t ll=nl?(size_t)(nl-p):(size_t)(e-p);const char*nd="content-length:";size_t nlen=15;if(ll>=nlen){int m=1;for(size_t k=0;k<nlen&&m;k++){char c=p[k];if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');m=(c==nd[k]);}if(m){const char*v=p+nlen;while(v<e&&(*v==' '||*v=='\t'))v++;char*ep=NULL;long long val=strtoll(v,&ep,10);if(ep&&ep>v)return val;}}p=nl?nl+1:e;}return -1;}
static int has_close(const char *h,size_t hl){return find_subseq((const unsigned char*)h,hl,"Connection: close")>=0||find_subseq((const unsigned char*)h,hl,"connection: close")>=0;}

static bool parse_fn_name(const unsigned char *buf, size_t len, char *out, size_t outsz)
{
    if (!buf||!len||!out||!outsz) return false;
    size_t eol=0; while(eol<len&&buf[eol]!='\n') eol++;
    const unsigned char *sp1=memchr(buf,' ',eol); if(!sp1) return false;
    const unsigned char *path=sp1+1;
    static const char prefix[]="/function/";
    size_t plen=eol-(size_t)(path-buf);
    const unsigned char *sp2=memchr(path,' ',plen); if(!sp2) return false;
    size_t rlen=(size_t)(sp2-path);
    if(rlen<=sizeof(prefix)-1||memcmp(path,prefix,sizeof(prefix)-1)!=0) return false;
    const unsigned char *name=path+sizeof(prefix)-1;
    size_t nlen=rlen-(sizeof(prefix)-1);
    for(size_t i=0;i<nlen;i++) if(name[i]=='/'||name[i]=='?'||name[i]==' '){nlen=i;break;}
    if(!nlen||nlen+1>outsz) return false;
    memcpy(out,name,nlen); out[nlen]='\0'; return true;
}

static void set_cipher(tlspeek_serial_t *s, const char *n)
{
    if (!s) return;
    if (!n) { s->cipher_suite=TLSPEEK_AES_256_GCM; return; }
    if (strstr(n,"CHACHA20")) s->cipher_suite=TLSPEEK_CHACHA20_POLY;
    /* wolfSSL reports "TLS13-AES128-GCM-SHA256" (no hyphen before 128). Match
     * both spellings so an AES-128 session is never mis-tagged as AES-256, which
     * would make tls_read_peek use a 32-byte key on a 16-byte session
     * (AES_GCM_AUTH_E). */
    else if (strstr(n,"AES128") || strstr(n,"AES-128")) s->cipher_suite=TLSPEEK_AES_128_GCM;
    else s->cipher_suite=TLSPEEK_AES_256_GCM;
}

static int export_serial(WOLFSSL *ssl, tlspeek_serial_t *serial)
{
    if (!ssl||!serial) return -1;
    memset(serial,0,sizeof(*serial));
    serial->magic=TLSPEEK_MAGIC;
    set_cipher(serial, wolfSSL_get_cipher_name(ssl));
    unsigned int sz=TLSPEEK_MAX_EXPORT_SZ;
    int rc=wolfSSL_tls_export(ssl,serial->tls_blob,&sz);
    if (rc>0) serial->blob_sz=sz; else serial->blob_sz=0;
    const unsigned char *k=wolfSSL_GetClientWriteKey(ssl);
    const unsigned char *iv=wolfSSL_GetClientWriteIV(ssl);
    int ksz=wolfSSL_GetKeySize(ssl), ivsz=wolfSSL_GetIVSize(ssl);
    if(!k||!iv||ksz<=0||ivsz<=0) return -1;
    memcpy(serial->client_write_key, k, (size_t)ksz < sizeof(serial->client_write_key) ? (size_t)ksz : sizeof(serial->client_write_key));
    memcpy(serial->client_write_iv,  iv,(size_t)ivsz< sizeof(serial->client_write_iv)  ? (size_t)ivsz : sizeof(serial->client_write_iv));
    word64 seq=0;
    if(wolfSSL_GetPeerSequenceNumber(ssl,&seq)<0) return -1;
    serial->read_seq_num=(uint64_t)seq;
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#define MAX_FDS 10240

typedef enum {
    WS_PEEK_OWNER,
    WS_READ_REQ,
    WS_WRITE_RESP
} worker_state_t;

typedef struct {
    int fd;
    int pipe_fd;
    bool pipe_sent;
    httpmigrate_ka_payload_https_t payload;
    WOLFSSL *ssl;
    
    worker_state_t state;
    
    unsigned char *buf;
    size_t cap;
    size_t len;
    
    ssize_t hdr_end;
    size_t hdr_sep;
    size_t hdr_sz;
    
    size_t body_target;
    size_t body_in;
    
    char resp[1024];
    size_t resp_len;
    size_t resp_off;
    
    uint64_t req_no;
    uint64_t top1;
    uint64_t top2;
    int should_close;
    bool first_request;
} worker_session_t;

static worker_session_t *s_sessions[MAX_FDS];
static int s_epoll_fd = -1;
static const char *s_fn_name = NULL;
static const char *s_relay_sock = NULL;
static WOLFSSL_CTX *s_wctx = NULL;

/* 0 = sum, 1 = product, 2 = timing */
static int s_calc_mode = 2; 

static void session_close(worker_session_t *s)
{
    if (!s) return;
    if (s->fd >= 0 && s->fd < MAX_FDS) s_sessions[s->fd] = NULL;
    if (s->fd >= 0) epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
    
    if (s->ssl) {
        wolfSSL_set_quiet_shutdown(s->ssl, 1);
        wolfSSL_free(s->ssl);
    }
    if (s->fd >= 0) close(s->fd);
    
    if (!s->pipe_sent && s->pipe_fd >= 0) {
        uint64_t ts_le = get_ns();
        (void)write(s->pipe_fd, &ts_le, sizeof(ts_le));
        close(s->pipe_fd);
    }
    if (s->buf) free(s->buf);
    free(s);
}

static int epoll_mod(int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(s_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static void session_build_resp(worker_session_t *s)
{
    int num1 = 0, num2 = 0;
    int result = 0;
    int parse_ok = 0;
    if (s_calc_mode != 2) {
        if (sscanf((const char *)(s->buf + s->hdr_sz), "%d %d", &num1, &num2) >= 2) {
            result = (s_calc_mode == 0) ? (num1 + num2) : (num1 * num2);
            parse_ok = 1;
        }
    }

    char json[512]; int jl = 0;
    uint64_t delta = (s->top2 > s->top1) ? s->top2 - s->top1 : 0;
    
    if (s_calc_mode != 2) {
        if (parse_ok) {
            jl = snprintf(json, sizeof(json),
                "{\n"
                "  \"status\": \"success\",\n"
                "  \"result\": %d\n"
                "}\n", result);
        } else {
            jl = snprintf(json, sizeof(json),
                "{\n  \"status\": \"error\"\n}\n");
        }
    } else {
        jl = snprintf(json, sizeof(json),
            "{\"worker\":\"%s\",\"request_no\":%" PRIu64
            ",\"path\":\"integration-https-ka\""
            ",\"top1_rdtsc\":%" PRIu64 ",\"top2_rdtsc\":%" PRIu64
            ",\"delta_ns\":%" PRIu64 ",\"cntfrq\":%" PRIu64 "}",
            s_fn_name, s->req_no, s->top1, s->top2, delta, (uint64_t)CNTFRQ);
    }

    int rl = snprintf(s->resp, sizeof(s->resp),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: %s\r\n\r\n%s",
        jl, s->should_close ? "close" : "keep-alive", json);
    
    s->resp_len = (size_t)rl;
    s->resp_off = 0;
}

static int session_relay(worker_session_t *s, const char *owner)
{
    memset(s->payload.base.target_function, 0, sizeof(s->payload.base.target_function));
    strncpy(s->payload.base.target_function, owner, sizeof(s->payload.base.target_function)-1);
    s->payload.base.top1_rdtsc = s->top1;
    s->payload.base.cntfrq = CNTFRQ;
    s->payload.base.top1_set = 1;
    s->payload.base.magic = HTTPMIGRATE_HTTPS_MAGIC;
    
    export_serial(s->ssl, &s->payload.serial);
    wolfSSL_set_fd(s->ssl, -1);
    wolfSSL_free(s->ssl);
    s->ssl = NULL;

    int rfd = socket(AF_UNIX, SOCK_SEQPACKET, 0); 
    if(rfd >= 0) {
        struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX; strncpy(addr.sun_path, s_relay_sock, sizeof(addr.sun_path)-1);
        if(connect(rfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            char cmsg_buf[CMSG_SPACE(sizeof(int))]; memset(cmsg_buf,0,sizeof(cmsg_buf));
            struct iovec iov={.iov_base=&s->payload, .iov_len=sizeof(s->payload)};
            struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=cmsg_buf,.msg_controllen=sizeof(cmsg_buf)};
            struct cmsghdr *cm=CMSG_FIRSTHDR(&msg);
            cm->cmsg_level=SOL_SOCKET; cm->cmsg_type=SCM_RIGHTS; cm->cmsg_len=CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &s->fd, sizeof(int));
            sendmsg(rfd, &msg, 0);
        }
        close(rfd);
    }
    
    if (s->fd >= 0) {
        epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
        if (s->fd < MAX_FDS) s_sessions[s->fd] = NULL;
        close(s->fd);
    }
    s->fd = -1; 
    session_close(s);
    return 0;
}

static void handle_session(worker_session_t *s)
{
    for (;;) {
        if (s->state == WS_PEEK_OWNER) {
            tlspeek_ctx_t peek_ctx;
            if (tlspeek_restore_peek_ctx(&peek_ctx, s->fd, &s->payload.serial) != 0) { session_close(s); return; }
            uint8_t peek_buf[4096];
            int pn = tls_read_peek(&peek_ctx, peek_buf, sizeof(peek_buf)-1);
            tlspeek_free(&peek_ctx);
            if (pn < 0) {
                session_close(s); return;
            }
            if (pn == 0) { return; } // Need more data (prevent close)
            peek_buf[pn] = '\0';
            
            if (!memchr(peek_buf, '\n', pn)) return; 
            
            char owner[HTTPMIGRATE_TARGET_LEN];
            if (!parse_fn_name(peek_buf, (size_t)pn, owner, sizeof(owner))) { session_close(s); return; }
            
            if (strcmp(owner, s_fn_name) != 0) {
                session_relay(s, owner);
                return;
            }
            
            s->state = WS_READ_REQ;
            epoll_mod(s->fd, EPOLLIN); // Switch back to Level-Triggered
            s->len = 0; s->hdr_end = -1; s->body_in = 0; s->body_target = 0; s->hdr_sz = 0;
        }
        
        if (s->state == WS_READ_REQ) {
            /* Replay data pre-buffered by the gateway (wolfSSL pipelining fix).
             * When wolfSSL_accept() consumed the first HTTP request from the TCP
             * socket during the TLS handshake, the gateway stored it in
             * serial.http_request.  wolfSSL_read would return WANT_READ waiting
             * for data that never arrives on the socket.  Pre-fill s->buf here
             * so the header-search loop picks it up without blocking. */
            if (s->first_request && s->len == 0 && s->payload.serial.request_len > 0) {
                int pre = s->payload.serial.request_len;
                if (s->cap < (size_t)pre + 1) {
                    size_t nc = s->cap ? s->cap : 16384;
                    while (nc < (size_t)pre + 1) nc *= 2;
                    unsigned char *nb = realloc(s->buf, nc);
                    if (nb) { s->buf = nb; s->cap = nc; }
                }
                if (s->cap > (size_t)pre) {
                    memcpy(s->buf, s->payload.serial.http_request, (size_t)pre);
                    s->len = (size_t)pre;
                    s->buf[s->len] = '\0';
                    s->payload.serial.request_len = 0; /* consume once */
                    ssize_t e4 = find_subseq(s->buf, s->len, "\r\n\r\n");
                    if (e4 >= 0) { s->hdr_end = e4; s->hdr_sep = 4; }
                    else {
                        ssize_t e2 = find_subseq(s->buf, s->len, "\n\n");
                        if (e2 >= 0) { s->hdr_end = e2; s->hdr_sep = 2; }
                    }
                }
            }

            while (s->hdr_end < 0) {
                if (s->cap < s->len + 4097) {
                    size_t nc = s->cap ? s->cap * 2 : 16384;
                    unsigned char *nb = realloc(s->buf, nc);
                    if (!nb) { session_close(s); return; }
                    s->buf = nb; s->cap = nc;
                }
                int n = wolfSSL_read(s->ssl, (char*)(s->buf + s->len), s->cap - s->len - 1);
                if (n <= 0) {
                    int err = wolfSSL_get_error(s->ssl, n);
                    if (err == WOLFSSL_ERROR_WANT_READ) return;
                    if (err == WOLFSSL_ERROR_WANT_WRITE) { epoll_mod(s->fd, EPOLLIN|EPOLLOUT); return; }
                    session_close(s); return;
                }
                s->len += n;
                s->buf[s->len] = '\0';
                ssize_t e4 = find_subseq(s->buf, s->len, "\r\n\r\n"); if(e4>=0){ s->hdr_end=e4; s->hdr_sep=4; break; }
                ssize_t e2 = find_subseq(s->buf, s->len, "\n\n");   if(e2>=0){ s->hdr_end=e2; s->hdr_sep=2; break; }
            }
            
            if (s->hdr_end >= 0 && s->body_target == 0) {
                s->hdr_sz = s->hdr_end + s->hdr_sep;
                long long cl = parse_cl((const char*)s->buf, s->hdr_sz);
                s->body_target = cl < 0 ? 0 : (size_t)cl;
                s->body_in = s->len > s->hdr_sz ? s->len - s->hdr_sz : 0;
                s->should_close = has_close((const char*)s->buf, s->hdr_sz);
            }
            
            if (s->hdr_end >= 0) {
                while (s->body_in < s->body_target) {
                    size_t want = s->body_target - s->body_in;
                    if (s->cap < s->len + want + 1) {
                        size_t nc = s->cap ? s->cap * 2 : 16384;
                        while(nc < s->len + want + 1) nc *= 2;
                        unsigned char *nb = realloc(s->buf, nc);
                        if (!nb) { session_close(s); return; }
                        s->buf = nb; s->cap = nc;
                    }
                    int n = wolfSSL_read(s->ssl, (char*)(s->buf + s->len), want);
                    if (n <= 0) {
                        int err = wolfSSL_get_error(s->ssl, n);
                        if (err == WOLFSSL_ERROR_WANT_READ) return;
                        if (err == WOLFSSL_ERROR_WANT_WRITE) { epoll_mod(s->fd, EPOLLIN|EPOLLOUT); return; }
                        session_close(s); return;
                    }
                    s->len += n; s->buf[s->len] = '\0';
                    s->body_in += n;
                }
            }
            
            if (s->hdr_end >= 0 && s->body_in >= s->body_target) {
                s->top2 = get_ns();
                session_build_resp(s);
                s->state = WS_WRITE_RESP;
                epoll_mod(s->fd, EPOLLIN|EPOLLOUT);
            }
        }
        
        if (s->state == WS_WRITE_RESP) {
            while (s->resp_off < s->resp_len) {
                int n = wolfSSL_write(s->ssl, s->resp + s->resp_off, s->resp_len - s->resp_off);
                if (n <= 0) {
                    int err = wolfSSL_get_error(s->ssl, n);
                    if (err == WOLFSSL_ERROR_WANT_READ) { epoll_mod(s->fd, EPOLLIN); return; }
                    if (err == WOLFSSL_ERROR_WANT_WRITE) { epoll_mod(s->fd, EPOLLIN|EPOLLOUT); return; }
                    session_close(s); return;
                }
                s->resp_off += n;
            }
            
            if (!s->pipe_sent && s->pipe_fd >= 0) {
                uint64_t ts = s->top2;
                (void)write(s->pipe_fd, &ts, sizeof(ts));
                close(s->pipe_fd); s->pipe_fd = -1; s->pipe_sent = true;
            }
            
            if (s->should_close) { session_close(s); return; }
            
            export_serial(s->ssl, &s->payload.serial);
            s->req_no++;
            s->first_request = false;
            
            size_t req_sz = s->hdr_sz + s->body_target;
            if (s->len > req_sz) {
                size_t left = s->len - req_sz;
                memmove(s->buf, s->buf + req_sz, left);
                s->len = left;
                s->state = WS_READ_REQ;
            } else {
                s->len = 0;
                s->state = WS_PEEK_OWNER;
                epoll_mod(s->fd, EPOLLIN | EPOLLET); // Edge-Triggered to prevent MSG_PEEK spin
            }
            
            s->hdr_end = -1; s->body_in = 0; s->body_target = 0; s->hdr_sz = 0;
            s->top1 = get_ns();
        }
    }
}

static void register_client(int client_fd, int pipe_fd, httpmigrate_ka_payload_https_t *payload)
{
    if (client_fd < 0 || client_fd >= MAX_FDS) {
        if (client_fd >= 0) close(client_fd);
        if (pipe_fd >= 0) close(pipe_fd);
        return;
    }
    set_nonblocking(client_fd);
    
    worker_session_t *s = calloc(1, sizeof(*s));
    if (!s) { close(client_fd); if (pipe_fd >= 0) close(pipe_fd); return; }
    
    s->fd = client_fd; s->pipe_fd = pipe_fd;
    s->payload = *payload;
    s->cap = 16384; s->buf = malloc(s->cap);
    if (!s->buf) { free(s); close(client_fd); if (pipe_fd >= 0) close(pipe_fd); return; }
    
    s->ssl = wolfSSL_new(s_wctx);
    if (!s->ssl) { free(s->buf); free(s); close(client_fd); if (pipe_fd >= 0) close(pipe_fd); return; }
    
    wolfSSL_set_fd(s->ssl, client_fd);
    if (tlspeek_restore(s->ssl, &s->payload.serial) != 0) {
        wolfSSL_free(s->ssl); free(s->buf); free(s); close(client_fd); if (pipe_fd >= 0) close(pipe_fd); return;
    }
    wolfSSL_set_fd(s->ssl, client_fd);
    
    s->req_no = 1;
    s->first_request = true;
    s->hdr_end = -1;
    
    if (s->payload.base.top1_set) {
        s->top1 = s->payload.base.top1_rdtsc;
        s->state = WS_READ_REQ;
    } else {
        s->top1 = get_ns();
        s->state = WS_PEEK_OWNER;
    }
    
    s_sessions[client_fd] = s;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = client_fd };
    epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
    
    handle_session(s);
}

int main(void)
{
    signal(SIGPIPE,SIG_IGN);

    s_fn_name    = getenv("HTTPMIGRATE_KA_FUNCTION_NAME");
    const char *socket_dir = getenv("SENDFD_SOCKET_DIR");
    const char *cert_file  = getenv("HTTPS_TLS_CERT");
    const char *key_file   = getenv("HTTPS_TLS_KEY");

    if(!s_fn_name   ||!s_fn_name[0])   s_fn_name    ="timing-fn-a";
    if(!socket_dir||!socket_dir[0])socket_dir ="/run/tlsmigrate";
    if(!cert_file ||!cert_file[0]) cert_file  ="/certs/server.crt";
    if(!key_file  ||!key_file[0])  key_file   ="/certs/server.key";
    
    if (strstr(s_fn_name, "sumprod-timing-fn-a")) s_calc_mode = 0;
    else if (strstr(s_fn_name, "sumprod-timing-fn-b")) s_calc_mode = 1;
    else s_calc_mode = 2;

    char own_ip[INET_ADDRSTRLEN];
    if(get_container_ip(own_ip,sizeof(own_ip))!=0) return 1;

    char fn_sock[256],relay_sock[256];
    snprintf(fn_sock,   sizeof(fn_sock),   "%s/%s-fn.sock",    socket_dir,own_ip);
    snprintf(relay_sock,sizeof(relay_sock),"%s/%s-relay.sock", socket_dir,own_ip);
    s_relay_sock = relay_sock;

    wolfSSL_Init();
    s_wctx=wolfSSL_CTX_new(wolfSSLv23_server_method());
    if(!s_wctx) return 1;
    if(wolfSSL_CTX_use_certificate_file(s_wctx,cert_file,SSL_FILETYPE_PEM)!=SSL_SUCCESS||
       wolfSSL_CTX_use_PrivateKey_file(s_wctx,key_file,  SSL_FILETYPE_PEM)!=SSL_SUCCESS){
        wolfSSL_CTX_free(s_wctx); return 1;
    }

    umask(0);
    mkdir(socket_dir,0777);
    unlink(fn_sock);
    int listen_fd=unix_server_socket(fn_sock,4096);
    if(listen_fd<0) return 1;
    set_nonblocking(listen_fd);
    chmod(fn_sock,0777);

    char name_sock[256];
    snprintf(name_sock,sizeof(name_sock),"%s/%s.sock",socket_dir,s_fn_name);
    unlink(name_sock); symlink(fn_sock,name_sock);

    s_epoll_fd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
    epoll_ctl(s_epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    fprintf(stderr,"[fn-worker-https-epoll] %s ready on %s relay=%s mode=%d\n",s_fn_name,fn_sock,relay_sock,s_calc_mode);

    struct epoll_event events[64];
    for(;;){
        int n = epoll_wait(s_epoll_fd, events, 64, -1);
        for(int i=0; i<n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                for (;;) {
                    int conn_fd = unix_accept(listen_fd);
                    if (conn_fd < 0) break;
                    
                    httpmigrate_ka_payload_https_t payload; memset(&payload,0,sizeof(payload));
                    int client_fd=-1, pipe_fd=-1;
                    if(recvfd2_https(conn_fd,&client_fd,&pipe_fd,&payload)!=0){ close(conn_fd); continue; }
                    close(conn_fd);
                    
                    if(client_fd<0){ if(pipe_fd>=0)close(pipe_fd); continue; }
                    register_client(client_fd, pipe_fd, &payload);
                }
            } else {
                worker_session_t *s = s_sessions[fd];
                if (s) {
                    if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                        session_close(s);
                    } else {
                        handle_session(s);
                    }
                } else {
                    epoll_ctl(s_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }
}
