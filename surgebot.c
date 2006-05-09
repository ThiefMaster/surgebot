#include "global.h"
#include "timer.h"
#include "database.h"
#include "sock.h"
#include "irc.h"
#include "surgebot.h"

#include <libgen.h> // basename()

#define CFG_FILE "surgebot.cfg"

IMPLEMENT_LIST(loop_func_list, loop_func *)

time_t		now;
unsigned int	quit_poll = 0;
struct surgebot	bot;
static struct loop_func_list	*loop_funcs;

static void sig_exit(int n)
{
	log_append(LOG_INFO, "Received SIGQUIT or SIGINT signal. Exiting.");
	//irc_send("QUIT :Received SIGQUIT or SIGINT signal - shutting down");
	quit_poll = 1;
}

static void sig_segv(int n)
{
	log_append(LOG_ERROR, "Received SIGSEGV signal. Exiting.");
	//irc_send("QUIT :Received SIGSEGV signal - shutting down");
	//sock_poll(); // run a single poll to get quit message sent
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
}

static int bot_init()
{
	char *str;

	memset(&bot, 0, sizeof(struct surgebot));

	if((bot.cfg = database_load(CFG_FILE)) == NULL)
	{
		log_append(LOG_ERROR, "Could not parse bot config file (%s)", CFG_FILE);
		return 1;
	}

	bot.start = now;
	bot.linked = now;
	bot.lines_received = 0;
	bot.lines_sent = 0;
	bot.server_tries = 0;
	bot.server_name = NULL;

	bot.nickname	= ((str = database_fetch(bot.cfg, "bot/nick", DB_STRING)) ? strdup(str) : NULL);
	bot.username	= ((str = database_fetch(bot.cfg, "bot/username", DB_STRING)) ? strdup(str) : NULL);
	bot.realname	= ((str = database_fetch(bot.cfg, "bot/realname", DB_STRING)) ? strdup(str) : NULL);
	bot.trigger	= ((str = database_fetch(bot.cfg, "bot/trigger", DB_STRING)) ? *str : '\0');

	bot.server_host		= ((str = database_fetch(bot.cfg, "uplink/host", DB_STRING)) ? strdup(str) : NULL);
	bot.server_port		= ((str = database_fetch(bot.cfg, "uplink/port", DB_STRING)) ? atoi(str) : 6667);
	bot.server_pass		= ((str = database_fetch(bot.cfg, "uplink/pass", DB_STRING)) ? strdup(str) : NULL);
	bot.max_server_tries	= ((str = database_fetch(bot.cfg, "uplink/max_tries", DB_STRING)) ? atoi(str) : 3);
	bot.server_ssl		= (true_string(database_fetch(bot.cfg, "uplink/ssl", DB_STRING)) ? 1 : 0);

	bot.local_host = ((str = database_fetch(bot.cfg, "uplink/local_host", DB_STRING)) ? strdup(str) : NULL);

	if(bot.nickname == NULL) 	log_append(LOG_ERROR, "/bot/nick must be set");
	if(bot.username == NULL)	log_append(LOG_ERROR, "/bot/username must be set");
	if(bot.realname == NULL)	log_append(LOG_ERROR, "/bot/realname must be set");
	if(bot.server_host == NULL)	log_append(LOG_ERROR, "/uplink/host must be set");

	if(bot.nickname == NULL || bot.username == NULL || bot.realname == NULL || bot.server_host == NULL)
		return 1;

	return 0;
}

static void bot_fini()
{
	if(bot.nickname) free(bot.nickname);
	if(bot.username) free(bot.username);
	if(bot.hostname) free(bot.hostname);
	if(bot.realname) free(bot.realname);

	if(bot.server_host) free(bot.server_host);
	if(bot.server_pass) free(bot.server_pass);
	if(bot.server_name) free(bot.server_name);
	if(bot.server_sock) sock_close(bot.server_sock);

	if(bot.local_host) free(bot.local_host);

	if(bot.cfg) dict_free(bot.cfg);
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

	log_init("surgebot.log");
	log_append(LOG_INFO, "Initializing");

	timer_init();
	database_init();
	sock_init();
	loop_funcs = loop_func_list_create();

	if(bot_init() != 0)
		return 1;

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
	}

	log_append(LOG_INFO, "Left event loop");

	irc_fini();
	bot_fini();

	loop_func_list_free(loop_funcs);
	sock_fini();
	database_fini();
	timer_fini();

	log_append(LOG_INFO, "Exiting");
	log_fini();

	return 0;
}

