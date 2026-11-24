#ifndef PRISM_APP_SHA256_H
#define PRISM_APP_SHA256_H

typedef struct
{
	unsigned int h[8];
	unsigned long long len;
	unsigned char buf[64];
	unsigned n;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_add(sha256_t *s, const void *data, unsigned long long n);
void sha256_end(sha256_t *s, unsigned char out[32]);
void sha256_hex(const unsigned char d[32], char out[65]);
#endif
