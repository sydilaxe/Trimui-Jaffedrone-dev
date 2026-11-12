/* Minimal blocking HTTP/1.1 client with HTTPS (Mbed TLS), redirects and
 * chunked transfers: enough to read GitHub's releases. From RadioScan. */
#pragma once
#include <stddef.h>

struct TlsSession;

typedef struct {
    int fd;
    struct TlsSession *tls; /* NULL for plain HTTP */
    int chunked;
    long chunk_left; /* bytes left in the current chunk, -1 = need its header */
    int eof;
    char buf[8192]; /* read-ahead */
    int buf_len, buf_pos;
    int status;
    char location[4096];  /* redirects: GitHub's signed download links run to ~1000 characters */
    long content_length;  /* -1 if the server didn't say */
} HttpConn;

/* Load the CA certificates (a PEM bundle). Without them HTTPS is unverified. */
int net_init(const char *ca_bundle_path);

/* Open url, following up to 5 redirects. 0 on a 2xx answer, -1 with err filled. */
int http_open(HttpConn *c, const char *url, char *err, size_t errlen);
/* Up to n body bytes: >0 read, 0 at the end, -1 on an error or 15 s of silence. */
int http_read(HttpConn *c, void *dst, int n);
void http_close(HttpConn *c);
/* A whole (small) body into a malloc'd, NUL-terminated buffer. Its length, or -1. */
long http_fetch(const char *url, char **out, long max_bytes, char *err, size_t errlen);
