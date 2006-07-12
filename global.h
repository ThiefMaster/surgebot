#ifndef GLOBAL_H
#define GLOBAL_H

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef NULL
#define NULL 0
#endif

#undef HAVE_IPV6
#define HAVE_MMAP
#define HAVE_SSL
//#define IRC_HANDLER_DEBUG

#ifdef HAVE_SSL
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#endif

#define MAXLEN		512
#define MAXARG		250
#define MAX_LINE_SIZE	450
#define DNS_TIMEOUT	15
#define NET_BUF_SIZE	32768

extern time_t now;

#ifdef __GNUC__
#define PRINTF_LIKE(M,N) __attribute__((format (printf, M, N)))
#else
#define PRINTF_LIKE(M,N)
#endif

#include "log.h"
#include "dict.h"
#include "tools.h"
#include "tokenize.h"
#include "structs.h"

#define ArraySize(ARRAY)		(sizeof((ARRAY)) / sizeof((ARRAY)[0]))

#undef assert
#define assert(CHECK)			do { if(!(CHECK)) { log_append(LOG_ERROR, "Assertion failed in %s:%d: %s", __FILE__, __LINE__, #CHECK); return; } } while(0)
#define assert_return(CHECK, RET)	do { if(!(CHECK)) { log_append(LOG_ERROR, "Assertion failed in %s:%d: %s", __FILE__, __LINE__, #CHECK); return (RET); } } while(0)
#define assert_break(CHECK)		{ if(!(CHECK)) { log_append(LOG_ERROR, "Assertion failed in %s:%d: %s", __FILE__, __LINE__, #CHECK); break; } }

#define safestrncpy(dest, src, len)	do { char *d = (dest); const char *s = (src); size_t l = strlen(s)+1;  if ((len) < l) l = (len); memmove(d, s, l); d[l-1] = 0; } while (0)

extern struct surgebot bot;

#if __GNUC__ >= 2
#define UNUSED_ARG(ARG) ARG __attribute__((unused))
#elif defined(S_SPLINT_S)
#define UNUSED_ARG(ARG) /*@unused@*/ ARG
#define const /*@observer@*/ /*@temp@*/
#else
#define UNUSED_ARG(ARG) ARG
#endif

#endif
