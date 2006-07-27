#ifndef SHA1_H
#define SHA1_H

typedef struct {
	unsigned int state[5];		/* state (ABCD) */
	unsigned int count[2];		/* number of bits, modulo 2^64 (lsb first) */
	unsigned char buffer[64];	/* input buffer */
} SHA1_CTX;

void SHA1Init(SHA1_CTX *);
void SHA1Update(SHA1_CTX *, const unsigned char *, unsigned int);
void SHA1Final(unsigned char[20], SHA1_CTX *);

const char *sha1(const char *str);

#endif

