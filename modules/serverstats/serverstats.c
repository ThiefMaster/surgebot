#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "irc_handler.h"
#include "timer.h"
#include "conf.h"

#include "html-template.h"

#define DEF_COLORS "-c BACK#FFFFFF -c CANVAS#F3F3F3 -c SHADEA#C8C8C8 -c SHADEB#969696 -c GRID#8C8C8C -c MGRID#821E1E -c FONT#000000 -c FRAME#000000 -c ARROW#FF0000"

MODULE_DEPENDS("commands", NULL);

static struct
{
	const char *rrdtool_path;
	const char *rrd_dir;
	const char *output_dir;
	unsigned int stats_update_freq;
	unsigned int graph_update_freq;
	const char *network_name;
	struct stringlist *ignore_servers;
} serverstats_conf;

struct irc_server
{
	char *name;
	char *uplink;
	unsigned int is_burst : 1;
	unsigned int is_burst_ack : 1;
	unsigned int is_hub : 1;
	unsigned int is_service : 1;
	unsigned int hops;
	char *numeric;
	unsigned int numeric_int;
	int lag;
	int asll_rtt;
	int asll_to;
	int asll_from;
	unsigned int clients;
	unsigned int max_clients;
	char *proto;
	time_t timestamp;
	char *info;
};

static void server_free(struct irc_server *server);
static void serverstats_conf_reload();
IRC_HANDLER(num_statsverbose);
IRC_HANDLER(num_endofstats);
IRC_HANDLER(num_luserchannels);
IRC_HANDLER(num_servicesdown);
IRC_HANDLER(msg);
COMMAND(serverstats_update);
COMMAND(serverstats_graphs);
static void update_stats_tmr(void *bound, void *data);
static void update_graphs_tmr(void *bound, void *data);
static void stats_collected();
static unsigned int rrd_exists(const char *name);
static void rrd_server_update(const struct irc_server *server);
static void rrd_network_update();
static void rrd_server_graph(const char *name);
static void rrd_network_graph();

static struct module *this;
static unsigned int stats_requested = 0;
static unsigned long total_channels = 0;
static unsigned long registered_channels = 0;
static unsigned long total_clients = 0;
static unsigned long total_clients_tmp = 0;
static struct dict *servers = NULL;
static struct dict *servers_tmp = NULL;

MODULE_INIT
{
	this = self;

	reg_conf_reload_func(serverstats_conf_reload);
	serverstats_conf_reload();

	reg_irc_handler("236", num_statsverbose);
	reg_irc_handler("219", num_endofstats);
	reg_irc_handler("254", num_luserchannels);
	reg_irc_handler("440", num_servicesdown);
	reg_irc_handler("PRIVMSG", msg);
	reg_irc_handler("NOTICE", msg);

	DEFINE_COMMAND(this, "serverstats update",	serverstats_update,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(this, "serverstats graphs",	serverstats_graphs,	1, CMD_REQUIRE_AUTHED, "group(admins)");
}

MODULE_FINI
{
	if(servers_tmp)
		dict_free(servers_tmp);
	if(servers)
		dict_free(servers);

	unreg_conf_reload_func(serverstats_conf_reload);

	unreg_irc_handler("236", num_statsverbose);
	unreg_irc_handler("219", num_endofstats);
	unreg_irc_handler("254", num_luserchannels);
	unreg_irc_handler("440", num_servicesdown);
	unreg_irc_handler("PRIVMSG", msg);
	unreg_irc_handler("NOTICE", msg);

	timer_del_boundname(this, "update_stats");
	timer_del_boundname(this, "update_graphs");
}

static void server_free(struct irc_server *server)
{
	free(server->name);
	free(server->uplink);
	free(server->numeric);
	free(server->proto);
	free(server->info);
	free(server);
}

static void serverstats_conf_reload()
{
	char *str;

	str = conf_get("serverstats/rrdtool_path", DB_STRING);
	serverstats_conf.rrdtool_path = str ? str : "/usr/bin/rrdtool";

	str = conf_get("serverstats/rrd_dir", DB_STRING);
	serverstats_conf.rrd_dir = str ? str : ".";

	str = conf_get("serverstats/output_dir", DB_STRING);
	serverstats_conf.output_dir = str ? str : ".";

	str = conf_get("serverstats/stats_update_freq", DB_STRING);
	serverstats_conf.stats_update_freq = str ? atoi(str) : 60;

	str = conf_get("serverstats/graph_update_freq", DB_STRING);
	serverstats_conf.graph_update_freq = str ? atoi(str) : 300;

	str = conf_get("serverstats/network_name", DB_STRING);
	serverstats_conf.network_name = str ? str : "NoNameNetwork";

	serverstats_conf.ignore_servers = conf_get("serverstats/ignore_servers", DB_STRINGLIST);

	timer_del_boundname(this, "update_stats");
	timer_del_boundname(this, "update_graphs");
	timer_add(this, "update_stats", now + serverstats_conf.stats_update_freq, update_stats_tmr, NULL, 0);
	timer_add(this, "update_graphs", now + serverstats_conf.graph_update_freq, update_graphs_tmr, NULL, 0);
}

static void update_stats_tmr(void *bound, void *data)
{
	if(stats_requested)
		return;

	stats_requested = 1;
	irc_send("STATS V");
}

static void update_graphs_tmr(void *bound, void *data)
{
	char str[MAXLEN];
	FILE *out, *out2;

	rrd_network_graph();

	if(servers)
	{
		dict_iter(node, servers)
		{
			if(serverstats_conf.ignore_servers && stringlist_find(serverstats_conf.ignore_servers, node->key) != -1)
				continue;
			rrd_server_graph(node->key);
		}
	}

	snprintf(str, sizeof(str), "%s/index.html", serverstats_conf.output_dir);
	out = fopen(str, "w");
	fprintf(out, HEADER, serverstats_conf.graph_update_freq, serverstats_conf.network_name, time2string(now), serverstats_conf.network_name, time2string(now));

	if(servers)
	{
		dict_iter(node, servers)
		{
			if(serverstats_conf.ignore_servers && stringlist_find(serverstats_conf.ignore_servers, node->key) != -1)
				continue;

			fprintf(out, SERVER_LINE, node->key, node->key, "day");

			snprintf(str, sizeof(str), "%s/%s.html", serverstats_conf.output_dir, node->key);
			out2 = fopen(str, "w");
			fprintf(out2, HEADER, serverstats_conf.graph_update_freq, node->key, time2string(now), node->key, time2string(now));
			fprintf(out2, SERVER_LINE, node->key, node->key, "day");
			fprintf(out2, SERVER_LINE, node->key, node->key, "week");
			fprintf(out2, SERVER_LINE, node->key, node->key, "month");
			fprintf(out2, SERVER_LINE, node->key, node->key, "year");
			fputs(FOOTER, out2);
			fclose(out2);
		}
	}

	fprintf(out, SERVER_LINE, "network", "network", "day");
	fputs(FOOTER, out);
	fclose(out);

	snprintf(str, sizeof(str), "%s/network.html", serverstats_conf.output_dir);
	out2 = fopen(str, "w");
	fprintf(out2, HEADER, serverstats_conf.graph_update_freq, serverstats_conf.network_name, time2string(now), serverstats_conf.network_name, time2string(now));
	fprintf(out2, SERVER_LINE, "network", "network", "day");
	fprintf(out2, SERVER_LINE, "network", "network", "week");
	fprintf(out2, SERVER_LINE, "network", "network", "month");
	fprintf(out2, SERVER_LINE, "network", "network", "year");
	fputs(FOOTER, out2);
	fclose(out2);


	timer_add(this, "update_graphs", now + serverstats_conf.graph_update_freq, update_graphs_tmr, NULL, 0);
}

static unsigned int rrd_exists(const char *name)
{
	char path[MAXLEN];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s.rrd", serverstats_conf.rrd_dir, name);
	if(!(fp = fopen(path, "r")))
		return 0;

	fclose(fp);
	return 1;
}

static void rrd_server_update(const struct irc_server *server)
{
	char str[MAXLEN];

	if(!rrd_exists(server->name))
		snprintf(str, sizeof(str), "%s create %s/%s.rrd DS:localusers:GAUGE:600:U:U DS:totalusers:GAUGE:600:U:U RRA:AVERAGE:0.5:1:600 RRA:AVERAGE:0.5:6:700 RRA:AVERAGE:0.5:24:775 RRA:AVERAGE:0.5:288:797 RRA:MAX:0.5:1:600 RRA:MAX:0.5:6:700 RRA:MAX:0.5:24:775 RRA:MAX:0.5:288:797 >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.rrd_dir, server->name);
	else
		snprintf(str, sizeof(str), "%s update %s/%s.rrd N:%d:%lu >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.rrd_dir, server->name, server->clients, total_clients);

	debug("Updating RRD for %s", server->name);
	system(str);
}

static void rrd_network_update()
{
	char str[MAXLEN];

	if(!rrd_exists(serverstats_conf.network_name))
		snprintf(str, sizeof(str), "%s create %s/%s.rrd DS:users:GAUGE:600:U:U DS:channels:GAUGE:600:U:U DS:regchannels:GAUGE:600:U:U RRA:AVERAGE:0.5:1:600 RRA:AVERAGE:0.5:24:775 RRA:AVERAGE:0.5:288:797 RRA:MAX:0.5:1:600 RRA:MAX:0.5:6:700 RRA:MAX:0.5:24:775 RRA:MAX:0.5:288:797", serverstats_conf.rrdtool_path, serverstats_conf.rrd_dir, serverstats_conf.network_name);
	else if(registered_channels > 0)
		snprintf(str, sizeof(str), "%s update %s/%s.rrd N:%lu:%lu:%lu", serverstats_conf.rrdtool_path, serverstats_conf.rrd_dir, serverstats_conf.network_name, total_clients, total_channels, registered_channels);
	else // When we do not know the registered channels for some reason, update it with UNKNOWN so we don't get a zero there.
		snprintf(str, sizeof(str), "%s update %s/%s.rrd N:%lu:%lu:U", serverstats_conf.rrdtool_path, serverstats_conf.rrd_dir, serverstats_conf.network_name, total_clients, total_channels);

	debug("Updating RRD for %s", serverstats_conf.network_name);
	system(str);
}

static void rrd_server_graph(const char *name)
{
	char str[4096];

	if(!rrd_exists(name))
		return;

	debug("Creating graphs for %s", name);
	snprintf(str, sizeof(str), "%s graph %s/%s-day.png %s -a PNG -v \"local users\" -t \"Users connected to %s (1 day)\" DEF:lastglobal=%s/%s.rrd:totalusers:AVERAGE DEF:localusers=%s/%s.rrd:localusers:AVERAGE \"CDEF:perclocal=localusers,100,*,lastglobal,/\" AREA:localusers#FF0000:\"Users connected to %s\\l\" DEF:maxlocal=%s/%s.rrd:localusers:MAX GPRINT:maxlocal:MAX:'Users -> Max\\: %%.lf' GPRINT:localusers:AVERAGE:'Average\\: %%.lf' GPRINT:localusers:LAST:'Current\\: %%.lf\\l' GPRINT:perclocal:LAST:'Percentage\\: %s has %%.2lf%%%% of global users\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, name, DEF_COLORS, name, serverstats_conf.rrd_dir, name, serverstats_conf.rrd_dir, name, name, serverstats_conf.rrd_dir, name, name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/%s-week.png %s -a PNG -s -1week -v \"local users\" -t \"Users connected to %s (1 week)\" DEF:localusers=%s/%s.rrd:localusers:AVERAGE AREA:localusers#FF0000:\"Users connected to %s\\l\" DEF:maxlocals=%s/%s.rrd:localusers:MAX GPRINT:maxlocals:MAX:'Users -> Max\\: %%.lf' GPRINT:localusers:AVERAGE:'Average\\: %%.lf' GPRINT:localusers:LAST:'Current\\: %%.lf users\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, name, DEF_COLORS, name, serverstats_conf.rrd_dir, name, name, serverstats_conf.rrd_dir, name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/%s-month.png %s -a PNG -s -1month -v \"local users\" -t \"Users connected to %s (1 month)\" DEF:localusers=%s/%s.rrd:localusers:AVERAGE AREA:localusers#FF0000:\"Users connected to %s\\l\" DEF:maxlocals=%s/%s.rrd:localusers:MAX GPRINT:maxlocals:MAX:'Users -> Max\\: %%.lf' GPRINT:localusers:AVERAGE:'Average\\: %%.lf' GPRINT:localusers:LAST:'Current\\: %%.lf users\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, name, DEF_COLORS, name, serverstats_conf.rrd_dir, name, name, serverstats_conf.rrd_dir, name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/%s-year.png %s -a PNG -s -1year -v \"local users\" -t \"Users connected to %s (1 year)\" DEF:localusers=%s/%s.rrd:localusers:AVERAGE AREA:localusers#FF0000:\"Users connected to %s\\l\" DEF:maxlocals=%s/%s.rrd:localusers:MAX GPRINT:maxlocals:MAX:'Users -> Max\\: %%.lf' GPRINT:localusers:AVERAGE:'Average\\: %%.lf' GPRINT:localusers:LAST:'Current\\: %%.lf users\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, name, DEF_COLORS, name, serverstats_conf.rrd_dir, name, name, serverstats_conf.rrd_dir, name);
	system(str);
}

static void rrd_network_graph()
{
	char str[4096];

	if(!rrd_exists(serverstats_conf.network_name))
		return;

	debug("Creating graphs for %s", serverstats_conf.network_name);
	snprintf(str, sizeof(str), "%s graph %s/network-day.png %s -a PNG -h 200 -s -1day -v \"users/channels\" -t \"Users and channels in the network (1 day)\" DEF:users=%s/%s.rrd:users:AVERAGE DEF:channels=%s/%s.rrd:channels:AVERAGE DEF:regchannels=%s/%s.rrd:regchannels:AVERAGE \"CDEF:percregistered=regchannels,100,*,channels,/\" \"CDEF:usersperchannel=users,channels,/\" AREA:users#FF0000:\"Users connected\" LINE2:channels#0000FF:\"Channels created\" LINE2:regchannels#00FF00:\"Channels registered\" DEF:maxusers=%s/%s.rrd:users:MAX DEF:maxchannels=%s/%s.rrd:channels:MAX DEF:maxregchannels=%s/%s.rrd:regchannels:MAX GPRINT:maxusers:MAX:'Users -> Max\\: %%.lf' GPRINT:users:AVERAGE:'Average\\: %%.lf' GPRINT:users:LAST:'Current\\: %%.lf\\l' GPRINT:maxchannels:MAX:'Channels -> Max\\: %%.lf' GPRINT:channels:AVERAGE:'Average\\: %%.lf' GPRINT:channels:LAST:'Current\\: %%.lf\\l' GPRINT:maxregchannels:MAX:'Channels (reg) -> Max\\: %%.lf' GPRINT:regchannels:AVERAGE:'Average\\: %%.lf' GPRINT:regchannels:LAST:'Current\\: %%.lf\\l' GPRINT:percregistered:LAST:'Registered\\: %%.2lf%%%% of all channels\\l' GPRINT:usersperchannel:LAST:'Users per channel\\: %%.2lf\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, DEF_COLORS, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/network-week.png %s -a PNG -h 200 -s -1week -v \"users/channels\" -t \"Users and channels in the network (1 week)\" DEF:users=%s/%s.rrd:users:AVERAGE DEF:channels=%s/%s.rrd:channels:AVERAGE DEF:regchannels=%s/%s.rrd:regchannels:AVERAGE AREA:users#FF0000:\"Users connected\" LINE2:channels#0000FF:\"Channels created\" LINE2:regchannels#00FF00:\"Channels registered\" DEF:maxusers=%s/%s.rrd:users:MAX DEF:maxchannels=%s/%s.rrd:channels:MAX DEF:maxregchannels=%s/%s.rrd:regchannels:MAX GPRINT:maxusers:MAX:'Users -> Max\\: %%.lf' GPRINT:users:AVERAGE:'Average\\: %%.lf' GPRINT:users:LAST:'Current\\: %%.lf\\l' GPRINT:maxchannels:MAX:'Channels -> Max\\: %%.lf' GPRINT:channels:AVERAGE:'Average\\: %%.lf' GPRINT:channels:LAST:'Current\\: %%.lf\\l' GPRINT:maxregchannels:MAX:'Channels (reg) -> Max\\: %%.lf' GPRINT:regchannels:AVERAGE:'Average\\: %%.lf' GPRINT:regchannels:LAST:'Current\\: %%.lf\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, DEF_COLORS, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/network-month.png %s -a PNG -h 200 -s -1month -v \"users/channels\" -t \"Users and channels in the network (1 month)\" DEF:users=%s/%s.rrd:users:AVERAGE DEF:channels=%s/%s.rrd:channels:AVERAGE DEF:regchannels=%s/%s.rrd:regchannels:AVERAGE AREA:users#FF0000:\"Users connected\" LINE2:channels#0000FF:\"Channels created\" LINE2:regchannels#00FF00:\"Channels registered\" DEF:maxusers=%s/%s.rrd:users:MAX DEF:maxchannels=%s/%s.rrd:channels:MAX DEF:maxregchannels=%s/%s.rrd:regchannels:MAX GPRINT:maxusers:MAX:'Users -> Max\\: %%.lf' GPRINT:users:AVERAGE:'Average\\: %%.lf' GPRINT:users:LAST:'Current\\: %%.lf\\l' GPRINT:maxchannels:MAX:'Channels -> Max\\: %%.lf' GPRINT:channels:AVERAGE:'Average\\: %%.lf' GPRINT:channels:LAST:'Current\\: %%.lf\\l' GPRINT:maxregchannels:MAX:'Channels (reg) -> Max\\: %%.lf' GPRINT:regchannels:AVERAGE:'Average\\: %%.lf' GPRINT:regchannels:LAST:'Current\\: %%.lf\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, DEF_COLORS, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name);
	system(str);
	snprintf(str, sizeof(str), "%s graph %s/network-year.png %s -a PNG -h 200 -s -1year -v \"users/channels\" -t \"Users and channels in the network (1 year)\" DEF:users=%s/%s.rrd:users:AVERAGE DEF:channels=%s/%s.rrd:channels:AVERAGE DEF:regchannels=%s/%s.rrd:regchannels:AVERAGE AREA:users#FF0000:\"Users connected\" LINE2:channels#0000FF:\"Channels created\" LINE2:regchannels#00FF00:\"Channels registered\" DEF:maxusers=%s/%s.rrd:users:MAX DEF:maxchannels=%s/%s.rrd:channels:MAX DEF:maxregchannels=%s/%s.rrd:regchannels:MAX GPRINT:maxusers:MAX:'Users -> Max\\: %%.lf' GPRINT:users:AVERAGE:'Average\\: %%.lf' GPRINT:users:LAST:'Current\\: %%.lf\\l' GPRINT:maxchannels:MAX:'Channels -> Max\\: %%.lf' GPRINT:channels:AVERAGE:'Average\\: %%.lf' GPRINT:channels:LAST:'Current\\: %%.lf\\l' GPRINT:maxregchannels:MAX:'Channels (reg) -> Max\\: %%.lf' GPRINT:regchannels:AVERAGE:'Average\\: %%.lf' GPRINT:regchannels:LAST:'Current\\: %%.lf\\l' >> /dev/null", serverstats_conf.rrdtool_path, serverstats_conf.output_dir, DEF_COLORS, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name, serverstats_conf.rrd_dir, serverstats_conf.network_name);
	system(str);
}

IRC_HANDLER(num_statsverbose)
{
	struct irc_server *server;

	if(stats_requested != 1)
		return;

	assert(argc > 16);

	if(!servers_tmp)
	{
		total_clients_tmp = 0;
		servers_tmp = dict_create();
		dict_set_free_funcs(servers_tmp, NULL, (dict_free_f*)server_free);
	}

	server = malloc(sizeof(struct irc_server));
	memset(server, 0, sizeof(struct irc_server));

	server->name		= strdup(argv[2]);
	server->uplink		= strdup(argv[3]);
	server->is_burst 	= (argv[3][0] == 'B');
	server->is_burst_ack	= (argv[3][1] == 'A');
	server->is_hub		= (argv[3][2] == 'H');
	server->is_service	= (argv[3][3] == 'S');
	server->hops		= atoi(argv[5]);
	server->numeric		= strdup(argv[6]);
	server->numeric_int	= atoi(argv[7]);
	server->lag		= atoi(argv[8]);
	server->asll_rtt	= atoi(argv[9]);
	server->asll_to		= atoi(argv[10]);
	server->asll_from	= atoi(argv[11]);
	server->clients		= atoi(argv[12]);
	server->max_clients	= atoi(argv[13]);
	server->proto		= strdup(argv[14]);
	server->timestamp	= strtoul(argv[15], NULL, 10);
	server->info		= strdup(argv[16]);

	dict_insert(servers_tmp, server->name, server);
	total_clients_tmp += server->clients;
}

IRC_HANDLER(num_endofstats)
{
	if(stats_requested != 1)
		return;

	assert(argc > 2);
	if(strcmp(argv[2], "V"))
		return;

	if(servers)
		dict_free(servers);
	servers = servers_tmp;
	servers_tmp = NULL;

	total_clients = total_clients_tmp;
	total_clients_tmp = 0;

	stats_requested = 2;
	irc_send("LUSERS");
}

IRC_HANDLER(num_luserchannels)
{
	if(stats_requested != 2)
		return;

	assert(argc > 2);
	total_channels = strtoul(argv[2], NULL, 10);

	stats_requested = 3;
	irc_send("CHANSERV NETINFO");
}


IRC_HANDLER(num_servicesdown)
{
	if(stats_requested != 3)
		return;

	debug("(srvdown) Registered channels: %lu", registered_channels);
	stats_collected();
}

IRC_HANDLER(msg)
{
	char *c;

	if(stats_requested != 3)
		return;

	assert(argc > 2);

	if(!src || strcasecmp(src->nick, "ChanServ") || strcasecmp(argv[1], bot.nickname))
		return;

	if(strstr(argv[2], "Registered Channels:") == NULL)
		return;

	// Looks like we have the right line from chanserv
	for(c = argv[2]; (c && !isdigit(*c)); c++)
		; // Empty body

	if(*c != '\0')
		registered_channels = strtoul(c, NULL, 10);
	debug("Registered channels: %lu", registered_channels);
	stats_collected();
}

COMMAND(serverstats_update)
{
	if(stats_requested)
	{
		reply("Stats are already being updated.");
		return 0;
	}

	timer_del_boundname(this, "update_stats");
	update_stats_tmr(this, NULL);
	reply("Updating server stats.");
	return 1;
}

COMMAND(serverstats_graphs)
{
	timer_del_boundname(this, "update_graphs");
	update_graphs_tmr(this, NULL);
	reply("Created stat graphs.");
	return 1;
}

static void stats_collected()
{
	rrd_network_update();

	if(servers)
	{
		dict_iter(node, servers)
		{
			struct irc_server *server = node->data;
			if(serverstats_conf.ignore_servers && stringlist_find(serverstats_conf.ignore_servers, server->name) != -1)
				continue;
			rrd_server_update(server);
		}
	}

	stats_requested = 0;
	timer_add(this, "update_stats", now + serverstats_conf.stats_update_freq, update_stats_tmr, NULL, 0);
}
