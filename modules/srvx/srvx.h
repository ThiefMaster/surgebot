#ifndef SRVX_H
#define SRVX_H

struct srvx_request;
typedef void (srvx_response_f)(struct srvx_request *r, void *ctx);

struct srvx_response_line
{
	char *nick;
	char *msg;
};


struct srvx_request
{
	srvx_response_f *callback;
	void *ctx;
	unsigned int free_ctx : 1;
	char *token;
	unsigned int count;
	unsigned int size;
	char *nick;
	struct srvx_response_line **lines;
};

#define srvx_send(FUNC, FMT, ...)	srvx_send_ctx(FUNC, NULL, 0, FMT, ##__VA_ARGS__)
#define srvx_sendonly(FMT, ...)		srvx_send_ctx(NULL, NULL, 0, FMT, ##__VA_ARGS__)
void srvx_send_ctx(srvx_response_f *func, void *ctx, unsigned int free_ctx, const char *format, ...) PRINTF_LIKE(4,5);
void srvx_send_ctx_noqserver(srvx_response_f *func, void *ctx, unsigned int free_ctx, const char *format, ...) PRINTF_LIKE(4,5);
void srvx_vsend_ctx(srvx_response_f *func, void *ctx, unsigned int free_ctx, unsigned int no_qserver, const char *format, va_list args);

#endif
