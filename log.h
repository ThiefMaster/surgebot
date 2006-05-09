#ifndef LOG_H
#define LOG_H

enum log_level
{
	LOG_DEBUG	= 0x01,
	LOG_INFO	= 0x02,
	LOG_WARNING	= 0x04,
	LOG_ERROR	= 0x08,
	LOG_SEND	= 0x10,
	LOG_RECEIVE	= 0x20,
	LOG_CMD		= 0x40
};

void log_init(const char *file);
void log_fini();
void log_append(enum log_level level, char *text, ...) PRINTF_LIKE(2, 3);

#define debug(text...)		log_append(LOG_DEBUG, ## text)

#endif
