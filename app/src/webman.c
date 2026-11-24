/* Talk to webMAN MOD on this console (127.0.0.1:80), the way its XMB entries do.
 * /mount_ps3/<path> mounts even while an app runs. webMAN's Auto-Play stays OFF (the owner keeps
 * it off for real discs in the drive); PS3 games are started by a one-shot request instead
 * (launch.c request_play). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <net/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "webman.h"

static int net_up;

int wm_init(void)
{
	if (!net_up && netInitialize() == 0)
		net_up = 1;
	return net_up ? 0 : -1;
}

void wm_end(void)
{
	if (net_up)
		netDeinitialize();
	net_up = 0;
}

/* %-encode everything but letters, digits and / . - _ */
void wm_urlenc(char *out, int n, const char *path)
{
	static const char hex[] = "0123456789ABCDEF";
	int j = 0;

	for (; *path && j < n - 4; path++)
	{
		unsigned char c = (unsigned char)*path;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_')
			out[j++] = (char)c;
		else
		{
			out[j++] = '%';
			out[j++] = hex[c >> 4];
			out[j++] = hex[c & 15];
		}
	}
	out[j] = 0;
}

/* GET <url> (already encoded). Returns the HTTP status (200 ...) or a negative step number;
 * the start of the reply goes to resp. */
/* Open a connection to webMAN, send a request and close without waiting. Used to make webMAN
 * look at its request file (/dev_hdd0/tmp/wm_request): while that file exists, the next
 * connection it accepts is served FROM THE FILE (www_client.h do_web_command); v0.17's request
 * starts with /wait.ps3?xmb, which holds it until the app has quit and the XMB is back. */
int wm_poke(void)
{
	struct sockaddr_in sa;
	static const char req[] = "GET /cpursx.ps3 HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	int s;

	if (wm_init())
		return -1;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -2;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
	{
		close(s);
		return -3;
	}
	send(s, req, sizeof(req) - 1, 0);
	close(s);
	return 0;
}

int wm_get(const char *url, char *resp, int resp_n)
{
	struct sockaddr_in sa;
	char req[1200];
	int s, n, got = 0, status = -5;

	if (resp_n > 0)
		resp[0] = 0;
	if (wm_init())
		return -1;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -2;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
	{
		close(s);
		return -3;
	}
	n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n", url);
	if (send(s, req, n, 0) != n)
	{
		close(s);
		return -4;
	}
#ifdef SO_RCVTIMEO
	{
		struct timeval tv = { 20, 0 };

		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
#endif
	/* Stop at the end of the reply's headers. webMAN sends them once the mount is done, but
	 * keeps the connection open through its auto-play wait (www_client.h: auto_play() runs
	 * before sclose), which waits for THIS app to quit: reading to the close would hang 15 s
	 * and webMAN would give up. */
	while (!strstr(resp, "\r\n\r\n"))
	{
		char b[512];
		int r = recv(s, b, sizeof(b), 0);

		if (r <= 0)
			break;
		if (got < resp_n - 1)
		{
			int k = r < resp_n - 1 - got ? r : resp_n - 1 - got;

			memcpy(resp + got, b, k);
			got += k;
			resp[got] = 0;
		}
		else
			break;
	}
	close(s);
	if (got >= 12 && !memcmp(resp, "HTTP/1.", 7))
		status = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + (resp[11] - '0');
	return status;
}

/* GET <url> and keep the whole reply (headers and body), cut to out_n - 1 bytes; for webMAN's
 * status page (/cpursx.ps3), which ends when webMAN closes the connection. Returns bytes read. */
int wm_get_body(const char *url, char *out, int out_n)
{
	struct sockaddr_in sa;
	char req[600];
	int s, n, got = 0;

	if (out_n > 0)
		out[0] = 0;
	if (wm_init())
		return -1;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -2;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
	{
		close(s);
		return -3;
	}
	n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n", url);
	if (send(s, req, n, 0) != n)
	{
		close(s);
		return -4;
	}
#ifdef SO_RCVTIMEO
	{
		struct timeval tv = { 10, 0 };

		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
#endif
	while (got < out_n - 1)
	{
		int r = recv(s, out + got, out_n - 1 - got, 0);

		if (r <= 0)
			break;
		got += r;
		out[got] = 0;
	}
	close(s);
	return got;
}
