/* v0.57: the internet, for the Game covers page (net.c). The PS3's own HTTPS cannot reach GitHub
 * (80710a06: its TLS is too old), so TLS is BearSSL (lr/third_party), proven by Prism Net Test. */
#ifndef PRISM_APP_NET_H
#define PRISM_APP_NET_H
#include <ppu-types.h>

int  net_online(char *why, int n);	/* 1 = this PS3 has an IP address; else why says why not */
/* Unix seconds from a web server's Date header (the console clock can be far off, and a TLS
 * certificate is judged against the time); 0 = no answer */
u64  net_time(const char *host);
void https_time(u64 unix_s);		/* the time certificates are judged at */
/* one request on a kept-open connection to host (reopened as needed). head: HEAD, no body.
 * The body goes to buf (cap bytes at most). Returns the HTTP status, or < 0 (see net.c) */
int  https_req(const char *host, const char *path, int head, u8 *buf, u32 cap, u32 *len);
void https_end(void);
const char *https_err(int r);		/* words for a negative https_req result */
#endif
