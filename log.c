#include "global.h"

static FILE	*logfile = NULL;

void log_init(const char *file)
{
	if(!file)
		return;

	if(!(logfile = fopen(file, "a+")))
		fprintf(stderr, "Could not open log file %s: %s (%d)\n", file, strerror(errno), errno);
}

void log_fini()
{
	if(logfile)
		fclose(logfile);
}

void log_append(enum log_level level, char *text, ...)
{
	va_list	va;

	char	timestr[15], timedatestr[30], lvl[15];

	now = time(NULL);
	strftime(timestr, sizeof(timestr), "[%H:%M:%S]", localtime(&now));
	strftime(timedatestr, sizeof(timedatestr), "[%H:%M:%S %m/%d/%Y]", localtime(&now));

	switch(level)
	{
		case LOG_DEBUG:
			snprintf(lvl, sizeof(lvl), "(debug)");
			break;
		case LOG_INFO:
			snprintf(lvl, sizeof(lvl), "(info)");
			break;
		case LOG_WARNING:
			snprintf(lvl, sizeof(lvl), "(warning)");
			break;
		case LOG_ERROR:
			snprintf(lvl, sizeof(lvl), "(error)");
			break;
		case LOG_SEND:
			snprintf(lvl, sizeof(lvl), "(send)");
			break;
		case LOG_RECEIVE:
			snprintf(lvl, sizeof(lvl), "(receive)");
			break;
		case LOG_CMD:
			snprintf(lvl, sizeof(lvl), "(cmd)");
			break;
	}

	printf("%s %s ", timestr, lvl);
	if(logfile)
		fprintf(logfile, "%s %s ", timedatestr, lvl);

	if(level == LOG_ERROR || level == LOG_WARNING)
		printf("\033[1;31m");
	else if(level == LOG_CMD)
		printf("\033[33m");

	va_start(va, text);
	vprintf(text, va);
	if(level == LOG_ERROR || level == LOG_WARNING || level == LOG_CMD)
		printf("\033[0m");
	printf("\n");
	va_end(va);

	if(logfile)
	{
		va_start(va, text);
		vfprintf(logfile, text, va);
		fprintf(logfile, "\n");
		va_end(va);

		fflush(logfile);
	}
}
