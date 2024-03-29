#include "global.h"
#include "conf.h"

static struct
{
	const char *name;
	FILE *fd;
} logfile = { "surgebot.log", NULL };

void log_init()
{
	if(!logfile.name)
		return;

	if(!(logfile.fd = fopen(logfile.name, "a+")))
		fprintf(stderr, "Could not open log file %s: %s (%d)\n", logfile.name, strerror(errno), errno);

	reg_conf_reload_func(log_reload);
}

void log_fini()
{
	if(logfile.fd)
	{
		fclose(logfile.fd);
		logfile.fd = NULL;
	}

	unreg_conf_reload_func(log_reload);
}

void log_reload()
{
	fprintf(stderr, "Re-opening log file\n");
	logfile.fd = NULL;
	if(!(logfile.fd = fopen(logfile.name, "a+")))
		fprintf(stderr, "Could not open log file %s: %s (%d)\n", logfile.name, strerror(errno), errno);
}

void log_append(enum log_level level, const char *text, ...)
{
	va_list	va;

	char timestr[15], timedatestr[30], lvl[15];

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

	if(level == LOG_ERROR || level == LOG_WARNING)
		printf("\033[1;31m");
	else if(level == LOG_CMD)
		printf("\033[33m");
	else if(level == LOG_SEND)
		printf("\033[32m");
	else if(level == LOG_RECEIVE)
		printf("\033[1;36m");
	else if(level == LOG_INFO)
		printf("\033[1;34m");

	va_start(va, text);
	vprintf(text, va);
	if(level == LOG_ERROR || level == LOG_WARNING || level == LOG_CMD || level == LOG_SEND || level == LOG_RECEIVE || level == LOG_INFO)
		printf("\033[0m");
	printf("\n");
	va_end(va);

	if(logfile.fd)
	{
		va_start(va, text);
		fprintf(logfile.fd, "%s %s ", timedatestr, lvl);
		vfprintf(logfile.fd, text, va);
		fprintf(logfile.fd, "\n");
		va_end(va);

		fflush(logfile.fd);
	}
}
