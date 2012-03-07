#ifndef RADIOBOT_H
#define RADIOBOT_H

struct radiobot_conf
{
	const char *stream_ip;
	unsigned int stream_port;
	const char *stream_pass;
	const char *stream_ip_stats;
	unsigned int stream_port_stats;
	const char *stream_pass_stats;
	const char *site_url;
	const char *schedule_url;
	const char *teamspeak_url;
	struct stringlist *sanitize_nick_regexps;
	const char *stream_url;
	const char *stream_url_pls;
	const char *stream_url_asx;
	const char *stream_url_ram;
	const char *radiochan;
	const char *teamchan;
	const char *adminchan;
	const char *cmd_sock_host;
	unsigned int cmd_sock_port;
	const char *cmd_sock_pass;
	const char *cmd_sock_read_pass;
	unsigned int rrd_enabled;
	const char *rrdtool_path;
	const char *rrd_dir;
	const char *graph_dir;
	const char *gadget_update_url;
	const char *gadget_current_version;
	const char *memcached_config;
	struct {
		const char *key;
		const char *setmod;
	} api;
};

typedef void (radiobot_notify_func)(struct radiobot_conf *conf, const char *action, ...);
void radiobot_set_notify_func(radiobot_notify_func *func);

#endif
