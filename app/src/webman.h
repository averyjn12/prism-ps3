#ifndef PRISM_APP_WEBMAN_H
#define PRISM_APP_WEBMAN_H
int  wm_init(void);
void wm_end(void);
void wm_urlenc(char *out, int n, const char *path);
int  wm_get(const char *url, char *resp, int resp_n);
int  wm_poke(void);
int  wm_get_body(const char *url, char *out, int out_n);	/* the whole reply, cut to out_n */	/* connect, send, close: makes webMAN serve its request file */
#endif
