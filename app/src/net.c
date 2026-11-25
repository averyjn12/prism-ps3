/* See net.h. HTTP/1.1 over BearSSL with one connection kept open (raw.githubusercontent.com serves
 * every cover source, so a look-up of many games pays for one TLS handshake, not one each). Only
 * GET and HEAD; no redirects followed; a body bigger than the caller's buffer is refused. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <net/net.h>
#include <net/netctl.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <lv2/system.h>
#include "net.h"
#include "webman.h"
#include "bearssl.h"
#include "ta_mozilla.c"		/* TAs[], TAs_NUM: Mozilla's roots (third_party/build_bearssl.sh) */

#define E_DNS		-1
#define E_CONNECT	-2
#define E_TLS		-3
#define E_REPLY		-4
#define E_BIG		-5
#define E_CHUNK		-6

const char *https_err(int r)
{
	return r == E_DNS ? "the name did not resolve (DNS)" : r == E_CONNECT ? "no connection" : r == E_TLS ? "the secure connection failed"
		: r == E_REPLY ? "no sensible reply" : r == E_BIG ? "the file is too big" : r == E_CHUNK ? "a broken reply" : "error";
}

int net_online(char *why, int n)
{
	s32 state = -1;

	wm_init();	/* netInitialize, once (webman.c) */
	netCtlInit();
	if (netCtlGetState(&state) != 0 || state != 3)
	{
		snprintf(why, n, "This PS3 is not connected to the internet (network state %d). Check Settings, Network Settings on the XMB.", (int)state);
		return 0;
	}
	why[0] = 0;
	return 1;
}

static int resolve(const char *host, struct in_addr *out)
{
	struct hostent *he = gethostbyname(host);

	if (!he || !he->h_addr_list || !he->h_addr_list[0])
		return 0;
	memcpy(out, he->h_addr_list[0], sizeof *out);
	return 1;
}

static int tcp_connect(const char *host, int port)
{
	struct sockaddr_in sa;
	struct timeval tv = { 15, 0 };
	struct in_addr a;
	int s;

	if (!resolve(host, &a))
		return E_DNS;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return E_CONNECT;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr = a;
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0)
	{
		close(s);
		return E_CONNECT;
	}
	return s;
}

/* the time from an HTTP "Date:" header (IMF-fixdate), as Unix seconds; 0 = none (nettest.c) */
static u64 parse_http_date(const char *hdr)
{
	static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *p = strstr(hdr, "\nDate: ");
	char m[4] = { 0 };
	int d, y, hh, mm, ss, mi;
	long long yy, era, yoe, doy, doe, days;

	if (!p)
		p = strstr(hdr, "\ndate: ");
	if (!p || sscanf(p + 7, "%*3s, %d %3s %d %d:%d:%d", &d, m, &y, &hh, &mm, &ss) != 6)
		return 0;
	for (mi = 0; mi < 12 && strncmp(mon + mi * 3, m, 3); mi++)
		;
	if (mi == 12)
		return 0;
	yy = y - (mi + 1 <= 2);
	era = (yy >= 0 ? yy : yy - 399) / 400;
	yoe = yy - era * 400;
	doy = (153 * ((mi + 1) + ((mi + 1) > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = era * 146097 + doe - 719468;
	return (u64)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

u64 net_time(const char *host)
{
	char req[200], b[1536];
	int s = tcp_connect(host, 80), n, got = 0;

	if (s < 0)
		return 0;
	snprintf(req, sizeof req, "HEAD / HTTP/1.0\r\nHost: %s\r\nUser-Agent: Prism\r\nConnection: close\r\n\r\n", host);
	send(s, req, strlen(req), 0);
	while (got < (int)sizeof b - 1 && (n = recv(s, b + got, sizeof b - 1 - got, 0)) > 0)
		got += n;
	close(s);
	b[got > 0 ? got : 0] = 0;
	return parse_http_date(b);
}

/* ---- the kept-open TLS connection ---- */

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static int fd = -1;
static char cur_host[64];
static u64 cert_time;
static char rest[4096];		/* bytes read past the headers */
static int n_rest;

void https_time(u64 unix_s)
{
	cert_time = unix_s;
}

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
	int r = recv(*(int *)ctx, buf, len, 0);

	return r > 0 ? r : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
	int r = send(*(int *)ctx, buf, len, 0);

	return r > 0 ? r : -1;
}

void https_end(void)
{
	if (fd >= 0)
		close(fd);	/* no close_notify: br_sslio_close would wait for the server's, up to the 15 s timeout */
	fd = -1;
	cur_host[0] = 0;
	n_rest = 0;
}

static int open_tls(const char *host)
{
	u8 seed[32];

	https_end();
	fd = tcp_connect(host, 443);
	if (fd < 0)
	{
		int r = fd;

		fd = -1;
		return r;
	}
	br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
	if (cert_time)
		br_x509_minimal_set_time(&xc, (uint32_t)(cert_time / 86400 + 719528), (uint32_t)(cert_time % 86400));
	br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
	sysGetRandomNumber(seed, sizeof seed);
	br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);
	if (!br_ssl_client_reset(&sc, host, 0))
	{
		https_end();
		return E_TLS;
	}
	br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
	snprintf(cur_host, sizeof cur_host, "%s", host);
	return 0;
}

/* n bytes of the body: first what came with the headers, then the connection */
static int body_read(u8 *dst, int n)
{
	int got = 0;

	if (n_rest)
	{
		int k = n < n_rest ? n : n_rest;

		memcpy(dst, rest, k);
		memmove(rest, rest + k, n_rest - k);
		n_rest -= k;
		got = k;
	}
	while (got < n)
	{
		int r = br_sslio_read(&ioc, dst + got, n - got);

		if (r <= 0)
			return -1;
		got += r;
	}
	return got;
}

static int line_read(char *out, int n)
{
	int k = 0;

	for (;;)
	{
		u8 c;

		if (body_read(&c, 1) != 1)
			return -1;
		if (c == '\n')
			break;
		if (k < n - 1 && c != '\r')
			out[k++] = (char)c;
	}
	out[k] = 0;
	return k;
}

/* a body we do not keep (a 404 page): read and thrown away, so the connection stays usable */
static int skip(u32 n)
{
	u8 junk[512];

	while (n)
	{
		int k = n > sizeof junk ? sizeof junk : (int)n;

		if (body_read(junk, k) != k)
			return -1;
		n -= k;
	}
	return 0;
}

static int one_req(const char *host, const char *path, int head, u8 *buf, u32 cap, u32 *len, int *sent)
{
	char req[400], hdr[4096];
	int code = 0, hl = 0, chunked = 0, closing = 0, keep;
	long long clen = -1;
	char *e;

	*sent = 0;
	*len = 0;
	if (fd < 0 || strcmp(cur_host, host))
	{
		int r = open_tls(host);

		if (r)
			return r;
	}
	snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Prism\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
		head ? "HEAD" : "GET", path, host);
	if (br_sslio_write_all(&ioc, req, strlen(req)) < 0 || br_sslio_flush(&ioc) < 0)
		return E_TLS;
	*sent = 1;
	/* the headers, whole, then whatever came after them */
	n_rest = 0;
	hdr[0] = 0;
	for (;;)
	{
		int r;

		if (hl >= (int)sizeof hdr - 1)
			return E_REPLY;
		r = br_sslio_read(&ioc, hdr + hl, sizeof hdr - 1 - hl);
		if (r <= 0)
			return hl ? E_REPLY : E_TLS;	/* nothing at all: a kept-open connection the server closed */
		hl += r;
		hdr[hl] = 0;
		if ((e = strstr(hdr, "\r\n\r\n")))
			break;
	}
	e += 4;
	n_rest = hl - (int)(e - hdr);
	if (n_rest > (int)sizeof rest)
		return E_REPLY;
	memcpy(rest, e, n_rest);
	e[-2] = 0;
	if (sscanf(hdr, "HTTP/1.%*d %d", &code) != 1)
		return E_REPLY;
	{
		char *p;

		for (p = hdr; *p; p++)
			if (*p >= 'A' && *p <= 'Z')
				*p = (char)(*p - 'A' + 'a');
		if ((p = strstr(hdr, "\r\ncontent-length:")))
			clen = atoll(p + 17);
		chunked = strstr(hdr, "\r\ntransfer-encoding: chunked") != NULL;
		closing = strstr(hdr, "\r\nconnection: close") != NULL;
	}
	keep = !closing;
	if (head || code == 204 || code == 304 || (code >= 100 && code < 200))
		;
	else if (chunked)
	{
		for (;;)
		{
			char ln[40];
			unsigned long sz;

			if (line_read(ln, sizeof ln) < 0)
				return E_CHUNK;
			sz = strtoul(ln, NULL, 16);
			if (!sz)
			{
				while (line_read(ln, sizeof ln) > 0)	/* trailers, up to the empty line */
					;
				break;
			}
			if (code != 200 || *len + sz > cap)
			{
				if (code == 200)
					return E_BIG;
				if (skip(sz))
					return E_CHUNK;
			}
			else if (body_read(buf + *len, (int)sz) != (int)sz)
				return E_CHUNK;
			else
				*len += sz;
			if (line_read(ln, sizeof ln) < 0)
				return E_CHUNK;
		}
	}
	else if (clen >= 0)
	{
		if (code != 200)
		{
			if (skip((u32)clen))
				return E_REPLY;
		}
		else if ((u64)clen > cap)
			return E_BIG;
		else if (body_read(buf, (int)clen) != (int)clen)
			return E_REPLY;
		else
			*len = (u32)clen;
	}
	else
	{
		/* no length: the body runs to the end of the connection */
		int r;

		keep = 0;
		while (code == 200 && *len < cap && (r = body_read(buf + *len, 1)) == 1)
			(*len)++;
	}
	if (!keep)
		https_end();
	return code;
}

int https_req(const char *host, const char *path, int head, u8 *buf, u32 cap, u32 *len)
{
	int sent, r = one_req(host, path, head, buf, cap, len, &sent);

	/* a kept-open connection the server has since closed fails before any reply: open a new one, once */
	if (r == E_TLS)
	{
		https_end();
		r = one_req(host, path, head, buf, cap, len, &sent);
	}
	if (r < 0)
		https_end();
	return r;
}
