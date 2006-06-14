#include "global.h"
#include "conf.h"
#include "timer.h"
#include "database.h"
#include "sock.h"
#include "irc.h"
#include "irc_handler.h"
#include "stringlist.h"
#include "surgebot.h"

#include <libgen.h> // basename()

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
	log_append(LOG_INFO, "Received SIGQUIT or SIGINT. Exiting.");
	if(bot.server_sock)
		irc_send_fast("QUIT :Received SIGQUIT or SIGINT - shutting down");
	sock_poll(); // run a single poll to get quit message sent
	quit_poll = 1;
}

static void sig_segv(int n)
{
	log_append(LOG_ERROR, "Received SIGSEGV. Exiting.");
	if(bot.server_sock)
		irc_send_fast("QUIT :Received SIGSEGV - shutting down");
	sock_poll(); // run a single poll to get quit message sent
	exit(0);
}

static void signal_init()
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sig_exit;
	sigaction(SIGQUIT, &sa, NULL);
	sa.sa_handler = sig_exit;
	sigaction(SIGINT, &sa, NULL);
	sa.sa_handler = sig_segv;
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sa.sa_handler = sig_rehash;
	sigaction(SIGHUP, &sa, NULL);
}

static int bot_init()
{
	memset(&bot, 0, sizeof(struct surgebot));

	bot.start = now;
	bot.linked = now;
	bot.sendq = stringlist_create();

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

	unreg_conf_reload_func((conf_reload_f *)bot_conf_reload);
}

static int bot_conf_reload()
{
	char *str;

	bot_conf.nickname	= ((str = conf_get("bot/nick", DB_STRING)) ? str : NULL);
	bot_conf.username	= ((str = conf_get("bot/username", DB_STRING)) ? str : NULL);
	bot_conf.realname	= ((str = conf_get("bot/realname", DB_STRING)) ? str : NULL);
	bot_conf.trigger	= ((str = conf_get("bot/trigger", DB_STRING)) ? *str : '\0');

	bot_conf.server_host		= ((str = conf_get("uplink/host", DB_STRING)) ? str : NULL);
	bot_conf.server_port		= ((str = conf_get("uplink/port", DB_STRING)) ? atoi(str) : 6667);
	bot_conf.server_pass		= ((str = conf_get("uplink/pass", DB_STRING)) ? str : NULL);
	bot_conf.max_server_tries	= ((str = conf_get("uplink/max_tries", DB_STRING)) ? atoi(str) : 3);
	bot_conf.server_ssl		= conf_bool("uplink/ssl");
	bot_conf.throttle		= conf_bool("uplink/throttle");

	bot_conf.local_host = ((str = conf_get("uplink/local_host", DB_STRING)) ? str : NULL);

	if(bot_conf.nickname == NULL)	 	log_append(LOG_ERROR, "/bot/nick must be set");
	if(bot_conf.username == NULL)		log_append(LOG_ERROR, "/bot/username must be set");
	if(bot_conf.realname == NULL)		log_append(LOG_ERROR, "/bot/realname must be set");
	if(bot_conf.server_host == NULL)	log_append(LOG_ERROR, "/uplink/host must be set");

	if(bot_conf.nickname == NULL || bot_conf.username == NULL || bot_conf.realname == NULL || bot_conf.server_host == NULL ||
	   bot_conf.server_port <= 0)
		return 1;

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

int main(int argc, char **argv)
{
	int i;

	chdir(dirname(argv[0])); // make sure we are in the bot's main directory
	now = time(NULL);

	signal_init();
	if(conf_init() != 0)
		return 1;

	log_init("surgebot.log");
	log_append(LOG_INFO, "Initializing");

	timer_init();
	database_init();
	sock_init();
	loop_funcs = loop_func_list_create();

	if(bot_init() != 0)
		return 1;

	irc_handler_init();
	irc_init();

	if(irc_connect() != 0)
		return 1;

	// start event loop
	while(!quit_poll)
	{
		now = time(NULL);
		sock_poll();
		timer_poll();

		for(i = 0; i < loop_funcs->count; i++) // call loop funcs
			loop_funcs->data[i]();

		if(reload_conf)
		{
			conf_reload();
			reload_conf = 0;
		}
	}

	log_append(LOG_INFO, "Left event loop");

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

	return 0;
}

