#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "conf.h"
#include "timer.h"
#include "playlist.h"
#include "surgebot.h"
#include "stringbuffer.h"
#include "mp3c.h"

#include <math.h>
#include <shout/shout.h>
#include <pthread.h>

MODULE_DEPENDS("commands", NULL);

static struct
{
	const char *mp3_dir;
	const char *custom_mp3_dir;
	const char *teamchan;
	const char *stream_ip;
	unsigned int stream_port;
	const char *stream_pass;
	const char *stream_name;
	const char *stream_genre;
	const char *stream_url;
} playlistbot_conf;


COMMAND(playlist_start);
COMMAND(playlist_stop);
COMMAND(playlist_countdown);
COMMAND(playlist_next);
COMMAND(playlist_play);
COMMAND(playlist_reload);
COMMAND(playlist_status);
static void playlistbot_conf_reload();
static void *load_playlist(void *arg);
static void *load_duration(void *arg);
static void countdown_tick();

static struct module *this;
static struct playlist *playlist;
static shout_t *shout;
static FILE *mp3_fp = NULL;
static pthread_t thread = 0;
static pthread_t load_playlist_thread = 0;
static pthread_t duration_thread = 0;
static unsigned char stream_active = 0;
static unsigned char stream_next = 0;
static unsigned char stream_stop = 0;
static const char *stream_error = NULL;
static unsigned char playlist_loaded = 0;
static unsigned char playlist_loading = 0;
static time_t last_tick = 0;
static char *playlist_cd_by = NULL;
static pthread_mutex_t options_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t playlist_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t error_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t playlist_loaded_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t playlist_loading_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
	char *artist;
	char *title;
	unsigned long starttime;
	unsigned long endtime;
	unsigned long duration;
} playlist_status;

MODULE_INIT
{
	this = self;

	reg_conf_reload_func(playlistbot_conf_reload);
	playlistbot_conf_reload();

	memset(&playlist_status, 0, sizeof(playlist_status));

	pthread_create(&load_playlist_thread, NULL, load_playlist, NULL);

	DEFINE_COMMAND(this, "playlist on",	playlist_start,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist off",	playlist_stop,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist cd",	playlist_countdown,	1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist next",	playlist_next,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist play",	playlist_play,		2, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist reload",	playlist_reload,	1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist status",	playlist_status,	1, 0, "group(admins)");
}

MODULE_FINI
{
	unreg_conf_reload_func(playlistbot_conf_reload);

	timer_del_boundname(this, "countdown_finished");
	unreg_loop_func(countdown_tick);
	MyFree(playlist_cd_by);

	if(stream_active)
	{
		pthread_mutex_lock(&options_mutex);
		stream_stop = 2;
		pthread_mutex_unlock(&options_mutex);

		while(stream_stop < 3) // should finish quite fast so the delay can be ignored
			usleep(75000);

		shout_close(shout);
		shout_free(shout);
		shout_shutdown();
		shout = NULL;
	}

	while(playlist_loading)
		usleep(75000);

	if(playlist)
		playlist_free(playlist);

	pthread_mutex_lock(&status_mutex);
	MyFree(playlist_status.artist);
	MyFree(playlist_status.title);
	pthread_mutex_unlock(&status_mutex);

	pthread_mutex_destroy(&options_mutex);
	pthread_mutex_destroy(&playlist_mutex);
	pthread_mutex_destroy(&error_mutex);
	pthread_mutex_destroy(&config_mutex);
	pthread_mutex_destroy(&playlist_loaded_mutex);
	pthread_mutex_destroy(&playlist_loading_mutex);
	pthread_mutex_destroy(&status_mutex);
}

static void playlistbot_conf_reload()
{
	char *str;

	str = conf_get("playlistbot/mp3_dir", DB_STRING);
	playlistbot_conf.mp3_dir = str ? str : ".";

	str = conf_get("playlistbot/custom_mp3_dir", DB_STRING);
	playlistbot_conf.custom_mp3_dir = str ? str : playlistbot_conf.mp3_dir;

	str = conf_get("playlistbot/teamchan", DB_STRING);
	playlistbot_conf.teamchan = str;

	pthread_mutex_lock(&config_mutex);
	str = conf_get("playlistbot/stream_ip", DB_STRING);
	playlistbot_conf.stream_ip = str ? str : "127.0.0.1";

	str = conf_get("playlistbot/stream_port", DB_STRING);
	playlistbot_conf.stream_port = str ? atoi(str) : 8000;

	str = conf_get("playlistbot/stream_pass", DB_STRING);
	playlistbot_conf.stream_pass = str ? str : "secret";

	str = conf_get("playlistbot/stream_name", DB_STRING);
	playlistbot_conf.stream_name = str;

	str = conf_get("playlistbot/stream_genre", DB_STRING);
	playlistbot_conf.stream_genre = str;

	str = conf_get("playlistbot/stream_url", DB_STRING);
	playlistbot_conf.stream_url = str;
	pthread_mutex_unlock(&config_mutex);
}

static void *load_playlist(void *arg)
{
	struct playlist *new_playlist;

	pthread_mutex_lock(&playlist_loading_mutex);
	playlist_loading = 1;
	pthread_mutex_unlock(&playlist_loading_mutex);

	usleep(1000000); // for some reason the thread blocks the main process without this
	new_playlist = playlist_load(playlistbot_conf.mp3_dir, NULL);
	playlist_shuffle(new_playlist);
	playlist_freeze(new_playlist);
	pthread_mutex_lock(&playlist_mutex);
	if(playlist)
		playlist_free(playlist);
	playlist = new_playlist;
	debug("%d files in playlist", playlist->count);
	for(unsigned int i = 0; i < playlist->count; i++)
		debug("%d) %s - %s", playlist->data[i]->id, playlist->data[i]->artist, playlist->data[i]->title);
	pthread_mutex_unlock(&playlist_mutex);

	pthread_mutex_lock(&playlist_loading_mutex);
	playlist_loading = 0;
	pthread_mutex_unlock(&playlist_loading_mutex);

	pthread_mutex_lock(&playlist_loaded_mutex);
	playlist_loaded = 1;
	pthread_mutex_unlock(&playlist_loaded_mutex);
	pthread_exit(NULL);
}

static void *load_duration(void *arg)
{
	unsigned long duration;
	char *file = arg;

	duration = mp3_playtime(file);
	free(file);

	pthread_mutex_lock(&status_mutex);
	playlist_status.duration = duration;
	playlist_status.endtime = playlist_status.starttime + duration;
	pthread_mutex_unlock(&status_mutex);
	debug("Duration of current song: %lus", duration);
	pthread_exit(NULL);
}

static void set_metadata(shout_t *shout, const struct playlist_node *pl_node)
{
	shout_metadata_t *meta;
	char *songtitle;
	char *artist = pl_node->artist;
	char *title  = pl_node->title;

	if(artist && title)
		asprintf(&songtitle, "%s - %s", artist, title);
	else if(title)
		songtitle = strdup(title);
	else if(artist)
		songtitle = strdup(artist);
	else
		songtitle = strdup("Unknown Song");

	meta = shout_metadata_new();
	shout_metadata_add(meta, "song", songtitle);
	shout_metadata_add(meta, "url", "http://");
	shout_set_metadata(shout, meta);
	shout_metadata_free(meta);
	free(songtitle);
}

static void *stream_thread(void *arg)
{
	shout_init();
	shout = shout_new();
	pthread_mutex_lock(&config_mutex);
	shout_set_host(shout, playlistbot_conf.stream_ip);
	shout_set_protocol(shout, SHOUT_PROTOCOL_ICY);
	shout_set_port(shout, playlistbot_conf.stream_port);
	shout_set_password(shout, playlistbot_conf.stream_pass);
	if(playlistbot_conf.stream_name)
		shout_set_name(shout, playlistbot_conf.stream_name);
	if(playlistbot_conf.stream_genre)
		shout_set_genre(shout, playlistbot_conf.stream_genre);
	if(playlistbot_conf.stream_url)
		shout_set_url(shout, playlistbot_conf.stream_url);
	pthread_mutex_unlock(&config_mutex);
	shout_set_format(shout, SHOUT_FORMAT_MP3);
	if(shout_open(shout) != SHOUTERR_SUCCESS)
	{
		pthread_mutex_lock(&options_mutex);
		pthread_mutex_lock(&error_mutex);
		stream_error = shout_get_error(shout);
		stream_stop = 4;
		stream_active = 0;
		pthread_mutex_unlock(&error_mutex);
		pthread_mutex_unlock(&options_mutex);
		pthread_exit(NULL);
	}

	while(stream_stop < 2)
	{
		char buff[20480];
		long read, ret;
		struct playlist_node *node;

		pthread_mutex_lock(&playlist_mutex);
		node = playlist_next(playlist);

		mp3_fp = fopen(node->file, "r");
		if(!mp3_fp)
			continue;

		set_metadata(shout, node);
		pthread_mutex_lock(&status_mutex);
		MyFree(playlist_status.artist);
		MyFree(playlist_status.title);
		playlist_status.artist = strdup(node->artist ? node->artist : "NoArtist");
		playlist_status.title = strdup(node->title ? node->title : "NoTitle");
		playlist_status.starttime = now;
		playlist_status.duration = 0;
		playlist_status.endtime = 0;
		pthread_mutex_unlock(&status_mutex);

		debug("Starting duration thread");
		duration_thread = 0;
		pthread_create(&duration_thread, NULL, load_duration, strdup(node->file));

		debug("Streaming: %s - %s", node->artist, node->title);
		if(node->temp)
			playlist_free_node(node);
		pthread_mutex_unlock(&playlist_mutex);

		if(!stream_active)
		{
			pthread_mutex_lock(&options_mutex);
			stream_active = 1;
			pthread_mutex_unlock(&options_mutex);
		}

		while(1)
		{
			if(stream_next)
			{
				pthread_mutex_lock(&options_mutex);
				stream_next = 0;
				pthread_mutex_unlock(&options_mutex);
				break;
			}

			if(stream_stop == 2)
				break;

			read = fread(buff, 1, sizeof(buff), mp3_fp);

			if(read <= 0)
				break;
			ret = shout_send(shout, (unsigned char *)buff, read);
			if(ret != SHOUTERR_SUCCESS)
			{
				//debug("Send error: %s", shout_get_error(shout));
				break;
			}

			shout_sync(shout);
		}

		pthread_cancel(duration_thread);
		fclose(mp3_fp);
		mp3_fp = NULL;

		if(stream_stop == 1)
			stream_stop = 2;
	}

	pthread_mutex_lock(&options_mutex);
	stream_stop = 3;
	stream_active = 0;
	pthread_mutex_unlock(&options_mutex);
	pthread_exit(NULL);
}


COMMAND(playlist_start)
{
	if(stream_active)
	{
		reply("Die Playlist ist bereits aktiv");
		if(stream_stop == 1)
		{
			stream_stop = 0;
			reply("Playlist-Countdown angehalten.");
			irc_send("PRIVMSG %s :Die Playlist bleibt jetzt AN; Countdown von $b%s$b wurde gestoppt.", playlistbot_conf.teamchan, playlist_cd_by);
			timer_del_boundname(this, "countdown_finished");
			unreg_loop_func(countdown_tick);
			MyFree(playlist_cd_by);
		}
		return 0;
	}

	pthread_mutex_lock(&playlist_loaded_mutex);
	if(!playlist_loaded)
	{
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loaded_mutex);

	timer_del_boundname(this, "countdown_finished");
	unreg_loop_func(countdown_tick);
	MyFree(playlist_cd_by);

	thread = 0;
	pthread_create(&thread, NULL, stream_thread, NULL);

	while(!stream_active && !stream_stop)
		usleep(75000);

	if(stream_active)
		irc_send("PRIVMSG %s :Die Playlist ist jetzt $c3AN$c.", playlistbot_conf.teamchan);
	else if(stream_stop == 4)
		irc_send("PRIVMSG %s :Die Playlist konnte nicht gestartet werden: %s", playlistbot_conf.teamchan, stream_error);

	pthread_mutex_lock(&options_mutex);
	stream_stop = 0;
	stream_error = NULL;
	pthread_mutex_unlock(&options_mutex);
	return 1;
}

COMMAND(playlist_stop)
{
	if(!stream_active)
	{
		reply("Die Playlist ist nicht aktiv");
		return 0;
	}

	pthread_mutex_lock(&playlist_loaded_mutex);
	if(!playlist_loaded)
	{
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loaded_mutex);

	timer_del_boundname(this, "countdown_finished");
	unreg_loop_func(countdown_tick);
	MyFree(playlist_cd_by);

	pthread_mutex_lock(&options_mutex);
	stream_stop = 2;
	pthread_mutex_unlock(&options_mutex);

	while(stream_stop < 3) // should finish quite fast so the delay can be ignored
		usleep(75000);

	shout_close(shout);
	shout_free(shout);
	shout_shutdown();
	shout = NULL;

	stream_stop = 0;
	irc_send("PRIVMSG %s :Die Playlist ist jetzt $c4AUS$c.", playlistbot_conf.teamchan);
	return 1;
}

static void countdown_tick()
{
	pthread_mutex_lock(&status_mutex);
	if(last_tick < now && ((playlist_status.endtime - now) < 10 || ((((playlist_status.endtime - now) % 10) == 0 && playlist_status.endtime - now <= 60) || (((playlist_status.endtime - now) % 30) == 0))))
	{
		irc_send("PRIVMSG %s :Playlist wird in $b%lu$b Sekunden ausgeschaltet.", playlist_cd_by, playlist_status.endtime - now);
		last_tick = now;
	}
	pthread_mutex_unlock(&status_mutex);
}

static void countdown_tmr(void *bound, void *data)
{
	unreg_loop_func(countdown_tick);
	while(stream_stop < 3) // should finish quite fast so the delay can be ignored
		usleep(75000);

	shout_close(shout);
	shout_free(shout);
	shout_shutdown();
	shout = NULL;

	stream_stop = 0;
	irc_send("PRIVMSG %s,%s :Die Playlist ist jetzt $c4AUS$c -> ab auf den Stream, $b%s$b.", playlistbot_conf.teamchan, playlist_cd_by, playlist_cd_by);
	MyFree(playlist_cd_by);
}


COMMAND(playlist_countdown)
{
	if(!stream_active)
	{
		reply("Die Playlist ist nicht aktiv");
		return 0;
	}

	pthread_mutex_lock(&playlist_loaded_mutex);
	if(!playlist_loaded)
	{
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loaded_mutex);

	if(timer_exists_boundname(this, "countdown_finished"))
	{
		reply("Playlist-Countdown ist bereits aktiv");
		return 0;
	}

	pthread_mutex_lock(&status_mutex);
	if(!playlist_status.duration)
	{
		reply("Dauer das aktuellen Songs noch nicht berechnet. Bitte versuche es ein paar Sekunden erneut.");
		pthread_mutex_unlock(&status_mutex);
		return 0;
	}
	pthread_mutex_unlock(&status_mutex);

	pthread_mutex_lock(&options_mutex);
	stream_stop = 1;
	pthread_mutex_unlock(&options_mutex);

	pthread_mutex_lock(&status_mutex);
	timer_add(this, "countdown_finished", playlist_status.endtime, countdown_tmr, NULL, 0, 0);
	pthread_mutex_unlock(&status_mutex);
	reg_loop_func(countdown_tick);
	last_tick = now;
	MyFree(playlist_cd_by);
	playlist_cd_by = strdup(src->nick);

	pthread_mutex_lock(&status_mutex);
	irc_send("PRIVMSG %s,%s :Playlist wird in $b%lu$b Sekunden ausgeschaltet.", playlistbot_conf.teamchan, playlist_cd_by, playlist_status.endtime - now);
	pthread_mutex_unlock(&status_mutex);

	/*
	while(stream_stop < 3) // should finish quite fast so the delay can be ignored
		usleep(75000);

	shout_close(shout);
	shout_free(shout);
	shout_shutdown();
	shout = NULL;

	stream_stop = 0;
	irc_send("PRIVMSG %s :Die Playlist ist jetzt $c4AUS$c.", playlistbot_conf.teamchan);
	*/
	return 1;
}

COMMAND(playlist_next)
{
	if(!stream_active)
	{
		reply("Die Playlist ist nicht aktiv");
		return 0;
	}

	pthread_mutex_lock(&playlist_loaded_mutex);
	if(!playlist_loaded)
	{
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loaded_mutex);

	pthread_mutex_lock(&options_mutex);
	stream_next = 1;
	pthread_mutex_unlock(&options_mutex);
	irc_send("PRIVMSG %s :Die Playlist spielt jetzt das nächste Lied.", playlistbot_conf.teamchan);
	return 1;
}

COMMAND(playlist_play)
{
	char *filename, *orig;
	struct stringbuffer *file;
	int silent = 0;

	if(!stream_active)
	{
		reply("Die Playlist ist nicht aktiv");
		return 0;
	}

	pthread_mutex_lock(&playlist_loaded_mutex);
	if(!playlist_loaded)
	{
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loaded_mutex);

	orig = filename = untokenize(argc - 1, argv + 1, " ");
	if(match("*.mp3", filename))
	{
		reply("Datei muss auf *.mp3 enden");
		free(orig);
		return 0;
	}

	if(*filename == '!')
	{
		silent = 1;
		filename++;
	}

	file = stringbuffer_create();
	if(*filename != '/')
	{
		stringbuffer_append_string(file, playlistbot_conf.custom_mp3_dir);
		stringbuffer_append_char(file, '/');
	}
	stringbuffer_append_string(file, filename);

	if(!file_exists(file->string))
	{
		reply("Datei nicht gefunden");
		stringbuffer_free(file);
		free(orig);
		return 0;
	}

	pthread_mutex_lock(&playlist_mutex);
	playlist_enqueue_file(playlist, file->string);
	pthread_mutex_unlock(&playlist_mutex);

	if(silent)
		reply("Die Playlist wird als nächstes %s spielen.", filename);
	else
		irc_send("PRIVMSG %s :Die Playlist wird als nächstes %s spielen.", playlistbot_conf.teamchan, filename);
	stringbuffer_free(file);
	free(orig);
	return 1;
}

COMMAND(playlist_reload)
{
	pthread_mutex_lock(&playlist_loaded_mutex);
	pthread_mutex_lock(&playlist_loading_mutex);
	if(!playlist_loaded || playlist_loading)
	{
		pthread_mutex_unlock(&playlist_loading_mutex);
		pthread_mutex_unlock(&playlist_loaded_mutex);
		reply("Playlist ist noch nicht fertig geladen");
		return 0;
	}
	pthread_mutex_unlock(&playlist_loading_mutex);
	pthread_mutex_unlock(&playlist_loaded_mutex);

	load_playlist_thread = 0;
	pthread_create(&load_playlist_thread, NULL, load_playlist, NULL);
	reply("Playliste wird neu geladen....");
	return 1;
}

COMMAND(playlist_status)
{
	char played_string[9], duration_string[9];

	if(!stream_active)
	{
		reply("Playlist ist nicht aktiv.");
		return 1;
	}

	pthread_mutex_lock(&status_mutex);
	unsigned long elapsed = now - playlist_status.starttime;
	snprintf(played_string, sizeof(played_string), "%d:%02d", (int)floor(elapsed / 60), (int)(elapsed % 60));

	if(playlist_status.duration)
	{
		snprintf(duration_string, sizeof(duration_string), "%d:%02d", (int)floor(playlist_status.duration / 60), (int)(playlist_status.duration % 60));
		reply("Playlist ist aktiv: %s - %s [%s/%s]", playlist_status.artist, playlist_status.title, played_string, duration_string);
	}
	else
	{
		reply("Playlist ist aktiv: %s - %s [%s/???]", playlist_status.artist, playlist_status.title, played_string);
	}
	pthread_mutex_unlock(&status_mutex);

	return 1;
}
