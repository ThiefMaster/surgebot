#ifndef BITLY_H
#define BITLY_H

typedef void (bitly_shortened_f)(const char *url, int success, void *ctx);
typedef void (bitly_free_ctx_f)(void *ctx);

void bitly_shorten(const char *url, bitly_shortened_f *callback, void *ctx, bitly_free_ctx_f *free_f);

#endif
