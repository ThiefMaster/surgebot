#include "global.h"
#include "conf.h"
#include "timer.h"
#include "database.h"
#include "sock.h"
#include "log.h"
#include "irc.h"
#include "irc_handler.h"
#include "chanuser.h"
#include "chanuser_irc.h"
#include "module.h"
#include "account.h"
#include "group.h"
#include "stringlist.h"
#include "surgebot.h"

#include <libgen.h> // basename()
#include <sys/resource.h> // rlimit

#define LOGFILE "surgebot.log"

IMPLEMENT_LIST(loop_func_list, loop_func *)

time_t	now;
int	quit_poll = 0;
int	reload_conf = 0;

struct surgebot		bot;
struct surgebot_conf	bot_conf;

static struct loop_func_list	*loop_funcs;

static int bot_conf_reload();

static void sig_rehash(int n)
{
	log_append(LOG_INFO, "Received SIGHUP signal. Reloading config.");
	reload_conf = 1;
}

static void sig_exit(int n)
{
	log_append(LOG_INFO, "Received SIGQUIT, SIGTERM or SIGINT. Exiting.");
	if(bot.server_sock)
		irc_send_fast("QUIT :Received SIGQUIT, SIGTERM or SIGINT - shutting down");
	sock_poll(); // run a single poll to get quit message sent
	quit_poll = 1;
}

static void sig_segv(int n)
{
	log_append(LOG_ERROR, "Received SIGSEGV. Exiting.");
	if(bot.server_sock)
		irc_send_fast("QUIT :Received SIGSEGV - shutting down");
	sock_poll(); // run a single poll to get quit message sent
	// we must NOT exit() here or no core dump is created
}

static void sig_chld(int n)
{
	while(waitpid(-1, NULL, WNOHANG) > 0)
		/* empty */;
}

static void sig_usr1(int n)
{
	struct stat buf;
	log_append(LOG_INFO, "Received SIGUSR1 signal. Checking main logfile.");

	// see if the current logfile still exists
	if(stat(LOGFILE, &buf) != 0)
	{
		log_append(LOG_INFO, "Could not stat " LOGFILE ": %s", strerror(errno));
		log_reload();
		log_append(LOG_INFO, "Logfile has been reopened.");
	}
}

static void signal_init()
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = sig_rehash;
	sigaction(SIGHUP, &sa, NULL);

	sa.sa_handler = sig_chld;
	sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = sig_usr1;
	sigaction(SIGUSR1, &sa, NULL);

	// Reset handler after calling it so the program creates a core dump after a SEGV
	// and always exists after a second SIGINT etc.
	sa.sa_flags = SA_RESETHAND;

	sa.sa_handler = sig_segv;
	sigaction(SIGSEGV, &sa, NULL);

	sa.sa_handler = sig_exit;
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static int bot_init()
{
	memset(&bot, 0, sizeof(struct surgebot));

	bot.start = now;
	bot.linked = now;
	bot.sendq = stringlist_create();
	bot.burst_lines = stringlist_create();
	bot.server.capabilities = dict_create();
	dict_set_free_funcs(bot.server.capabilities, free, free);

	reg_conf_reload_func((conf_reload_f *)bot_conf_reload);
	return bot_conf_reload();
}

static void bot_fini()
{
	if(bot.nickname) free(bot.nickname);
	if(bot.username) free(bot.username);
	if(bot.hostname) free(bot.hostname);
	if(bot.realname) free(bot.realname);

	if(bot.server_name) free(bot.server_name);
	if(bot.server_sock) sock_close(bot.server_sock);

	stringlist_free(bot.sendq);
	stringlist_free(bot.burst_lines);
	dict_free(bot.server.capabilities);

	unreg_conf_reload_func((conf_reload_f *)bot_conf_reload);
}

static int bot_conf_reload()
{
	char *str;

	bot_conf.nickname	= ((str = conf_get("bot/nick", DB_STRING)) ? str : NULL);
	bot_conf.username	= ((str = conf_get("bot/username", DB_STRING)) ? str : NULL);
	bot_conf.realname	= ((str = conf_get("bot/realname", DB_STRING)) ? str : NULL);
	bot_conf.trigger	= ((str = conf_get("bot/trigger", DB_STRING)) ? str : "");

	bot_conf.server_host		= ((str = conf_get("uplink/host", DB_STRING)) ? str : NULL);
	bot_conf.server_port		= ((str = conf_get("uplink/port", DB_STRING)) ? atoi(str) : 6667);
	bot_conf.server_pass		= ((str = conf_get("uplink/pass", DB_STRING)) ? str : NULL);
	bot_conf.max_server_tries	= ((str = conf_get("uplink/max_tries", DB_STRING)) ? atoi(str) : 3);
	bot_conf.server_ssl		= conf_bool("uplink/ssl");
	bot_conf.server_ipv6		= conf_bool("uplink/ipv6");
	bot_conf.throttle		= conf_bool("uplink/throttle");

	bot_conf.local_host = ((str = conf_get("uplink/local_host", DB_STRING)) ? str : NULL);

	if(bot_conf.nickname == NULL)	 	log_append(LOG_ERROR, "/bot/nick must be set");
	if(bot_conf.username == NULL)		log_append(LOG_ERROR, "/bot/username must be set");
	if(bot_conf.realname == NULL)		log_append(LOG_ERROR, "/bot/realname must be set");
	if(bot_conf.server_host == NULL)	log_append(LOG_ERROR, "/uplink/host must be set");

	if(bot_conf.nickname == NULL || bot_conf.username == NULL || bot_conf.realname == NULL || bot_conf.server_host == NULL ||
	   bot_conf.server_port <= 0)
		return 1;

#ifndef HAVE_IPV6
	if(bot_conf.server_ipv6)
	{
		log_append(LOG_ERROR, "IPv6 not supported; define HAVE_IPV6 in global.h if you want IPv6 support");
		return 1;
	}
#endif

	if(bot_conf.nickname && bot.nickname && strcmp(bot.nickname, bot_conf.nickname))
	{
		debug("Changing nick from %s to %s", bot.nickname, bot_conf.nickname);
		irc_send("NICK %s", bot_conf.nickname);
	}

	return 0;
}

void reg_loop_func(loop_func *func)
{
	loop_func_list_add(loop_funcs, func);
}

void unreg_loop_func(loop_func *func)
{
	loop_func_list_del(loop_funcs, func);
}

unsigned char write_pid_file(const char *filename)
{
	// get own PID
	char *pid = int2string(getpid());
	size_t pid_len = strlen(pid);

	// write PID file
	FILE *pid_fd = fopen(filename, "w");
	if(pid_fd == NULL) {
		log_append(LOG_ERROR, "Could not open file to write PID: %s: %s", filename, strerror(errno));
		return 1;
	}
	size_t written = fwrite(pid, sizeof(char), pid_len, pid_fd);
	fclose(pid_fd);

	if(written != pid_len) {
		log_append(LOG_ERROR, "Could not write to PID file: %s", filename);
		return 1;
	}
	return 0;
}

unsigned char delete_pid_file(const char *filename)
{
	// remove PID file
	if(remove(filename) != 0) {
		log_append(LOG_ERROR, "Could not delete PID file: %s: %s", filename, strerror(errno));
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	chdir(dirname(argv[0])); // make sure we are in the bot's main directory
	now = time(NULL);
	srand(now);

	// Always generate core dumps when crashing
	struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
	setrlimit(RLIMIT_CORE, &rl);

	// write PID file
	const char *pid_filename = "surgebot.pid";
	if(write_pid_file(pid_filename) != 0)
		return 1;

	signal_init();
	tools_init();
	if(conf_init() != 0)
		return 1;

	log_init(LOGFILE);
	log_append(LOG_INFO, "Initializing");

	timer_init();
	database_init();
	sock_init();
	loop_funcs = loop_func_list_create();

	if(bot_init() != 0)
		return 1;

	irc_handler_init();
	irc_init();
	account_init();
	group_init();
	chanuser_init();
	chanuser_irc_init();
	module_init();

	if(irc_connect() != 0)
		return 1;

	// start event loop
	while(!quit_poll || quit_poll > now)
	{
		now = time(NULL);

		for(unsigned int i = 0; i < loop_funcs->count; i++) // call loop funcs
			loop_funcs->data[i]();

		sock_poll();
		timer_poll();

		if(reload_conf)
		{
			conf_reload();
			reload_conf = 0;
		}
	}

	log_append(LOG_INFO, "Left event loop");

	module_fini();
	chanuser_irc_fini();
	chanuser_fini();
	group_fini();
	account_fini();
	irc_fini();
	irc_handler_fini();
	bot_fini();

	loop_func_list_free(loop_funcs);
	sock_fini();
	database_fini();
	timer_fini();

	log_append(LOG_INFO, "Exiting");
	log_fini();
	conf_fini();
	tools_fini();
	delete_pid_file(pid_filename);
	return 0;
}

