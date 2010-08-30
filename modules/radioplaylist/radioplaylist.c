#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "conf.h"
#include "surgebot.h"

#include "pgsql.h"
#include "playlist.h"

#include <pthread.h>
#include <sys/mman.h>

#include <mad.h>
#include <lame/lame.h>
#include <shout/shout.h>


struct stream_ctx {
	// reset for each song
	unsigned char const *start;
	unsigned long length;
	unsigned int samplerate;
	// kept between songs
	shout_metadata_t *meta;
	lame_t lame;
	shout_t *shout;
	unsigned int last_samplerate;
};

struct stream_state {
	uint8_t terminate; // stream thread should terminate asap
	uint8_t play; // 0 = stop, 1 = playing, 2 = play until end of song
	struct playlist *playlist;
	uint8_t playing; // 0 = not playing, 1 = playing
	uint8_t skip; // skip current song
	uint16_t duration;
	time_t starttime;
	time_t endtime;
};

struct scan_state {
	enum {
		SCAN_IDLE	= 0, // scan thread not running, no results waiting
		SCAN_ACTIVE	= 1, // scan thread running
		SCAN_FINISHED	= 2 // scan thread not running or terminating, results waiting
	} state;

	char *path;
	char *nick;
	int32_t count;
};

COMMAND(playlist_on);
COMMAND(playlist_off);
COMMAND(playlist_countdown);
COMMAND(playlist_next);
COMMAND(playlist_status);
COMMAND(playlist_play);
COMMAND(playlist_blacklist);
COMMAND(playlist_reload);
COMMAND(playlist_add);
COMMAND(playlist_scan);
COMMAND(playlist_check);
COMMAND(playlist_truncate);
static void check_countdown();
static void check_scan_result();
static void conf_reload_hook();
// scan thread functions
static void *scan_thread_main(void *arg);
// stream thread functions
static void *stream_thread_main(void *arg);
static int8_t stream_init(struct stream_ctx *stream);
static int8_t stream_song(struct stream_ctx *stream, const char *filename);
static void stream_fini(struct stream_ctx *stream);
static enum mad_flow input_cb(void *data, struct mad_stream *stream);
static enum mad_flow output_cb(void *data, struct mad_header const *header, struct mad_pcm *pcm);
static enum mad_flow error_cb(void *data, struct mad_stream *stream, struct mad_frame *frame);
static int decode_mp3(struct stream_ctx *stream);

static struct module *this;
static struct stream_state stream_state;
static struct scan_state scan_state;
static struct pgsql *pg_conn;
static char *playlist_cd_by;
static time_t playlist_cd_tick;
static pthread_cond_t stream_cond;
static pthread_mutex_t stream_mutex;
static pthread_mutex_t playlist_mutex;
static pthread_mutex_t stream_state_mutex;
static pthread_mutex_t conf_mutex;
static pthread_t stream_thread;
static pthread_t scan_thread;

static struct {
	const char *db_conn_string;
	const char *stream_ip;
	uint16_t stream_port;
	const char *stream_pass;
	const char *stream_name;
	const char *stream_genre;
	const char *stream_url;
	uint16_t lame_bitrate;
	uint16_t lame_samplerate;
	uint8_t lame_quality;
	const char *teamchan;
} radioplaylist_conf;

MODULE_DEPENDS("commands", NULL);


MODULE_INIT
{
	this = self;

	pthread_cond_init(&stream_cond, NULL);
	pthread_mutex_init(&stream_mutex, NULL);
	pthread_mutex_init(&playlist_mutex, NULL);
	pthread_mutex_init(&stream_state_mutex, NULL);
	pthread_mutex_init(&conf_mutex, NULL);

	reg_conf_reload_func(conf_reload_hook);
	conf_reload_hook(); // Loads the playlist

	debug("starting stream thread");
	pthread_create(&stream_thread, NULL, stream_thread_main, NULL);

	DEFINE_COMMAND(this, "playlist on",		playlist_on,		0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist off",		playlist_off,		0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist cd",		playlist_countdown,	0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist next",		playlist_next,		0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist play",		playlist_play,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist status",		playlist_status,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "playlist blacklist",	playlist_blacklist,	1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist reload",		playlist_reload,	0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist add",		playlist_add,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist scan",		playlist_scan,		1, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist check",		playlist_check,		0, CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "playlist truncate",	playlist_truncate,	0, CMD_LOG_HOSTMASK, "group(admins)");
}


MODULE_FINI
{
	unreg_loop_func(check_countdown);
	MyFree(playlist_cd_by);

	// Tell stream to terminate
	pthread_mutex_lock(&stream_state_mutex);
	stream_state.terminate = 1;
	pthread_mutex_unlock(&stream_state_mutex);
	// Wake up stream thread so it can terminate
	pthread_mutex_lock(&stream_mutex);
	pthread_cond_signal(&stream_cond);
	pthread_mutex_unlock(&stream_mutex);
	// Wait for it to terminate
	debug("waiting for stream thread to finish");
	pthread_join(stream_thread, NULL);
	debug("stream thread finished");

	if(stream_state.playlist)
		stream_state.playlist->free(stream_state.playlist);

	if(pg_conn)
		pgsql_fini(pg_conn);

	unreg_conf_reload_func(conf_reload_hook);

	pthread_mutex_destroy(&conf_mutex);
	pthread_mutex_destroy(&stream_state_mutex);
	pthread_mutex_destroy(&playlist_mutex);
	pthread_mutex_destroy(&stream_mutex);
	pthread_cond_destroy(&stream_cond);
}

COMMAND(playlist_on)
{
	if(stream_state.playing)
	{
		reply("Die Playlist ist bereits an");
		if(stream_state.play == 2)
		{
			pthread_mutex_lock(&stream_state_mutex);
			stream_state.play = 1;
			pthread_mutex_unlock(&stream_state_mutex);
			irc_send("PRIVMSG %s :Die Playlist bleibt jetzt AN; Countdown von $b%s$b wurde gestoppt", radioplaylist_conf.teamchan, playlist_cd_by);
			if(playlist_cd_by)
				irc_send("PRIVMSG %s :Die Playlist bleibt jetzt AN; Countdown wurde von $b%s$b gestoppt", playlist_cd_by, src->nick);
			unreg_loop_func(check_countdown);
			MyFree(playlist_cd_by);
		}

		return 0;
	}

	if(!stream_state.playlist)
	{
		reply("Es ist keine Playlist geladen");
		return 0;
	}
	else if(!stream_state.playlist->count)
	{
		reply("Die Playlist ist leer");
		return 0;
	}

	pthread_mutex_lock(&stream_state_mutex);
	stream_state.play = 1;
	pthread_mutex_unlock(&stream_state_mutex);
	// Signal playlist thread to wake up
	pthread_mutex_lock(&stream_mutex);
	pthread_cond_signal(&stream_cond);
	pthread_mutex_unlock(&stream_mutex);

	// Wait for streaming to start
	while(stream_state.playing != stream_state.play)
	{
		debug("waiting for stream to start...");
		usleep(50000);
	}

	debug("stream is now %s", stream_state.playing ? "active" : "inactive");
	if(stream_state.playing)
		irc_send("PRIVMSG %s :Die Playlist ist jetzt $c3AN$c", radioplaylist_conf.teamchan);
	else
		irc_send("PRIVMSG %s :Die Playlist konnte nicht gestartet werden", radioplaylist_conf.teamchan);

	return 1;
}

COMMAND(playlist_off)
{
	if(!stream_state.playing)
	{
		reply("Die Playlist ist nicht an");
		return 0;
	}

	unreg_loop_func(check_countdown);
	MyFree(playlist_cd_by);

	pthread_mutex_lock(&stream_state_mutex);
	stream_state.play = 0;
	pthread_mutex_unlock(&stream_state_mutex);

	// Wait for streaming to start
	while(stream_state.playing != stream_state.play)
	{
		debug("waiting for stream to stop...");
		usleep(50000);
	}

	debug("stream is now %s", stream_state.playing ? "active" : "inactive");
	if(!stream_state.playing)
		irc_send("PRIVMSG %s :Die Playlist ist jetzt $c4AUS$c", radioplaylist_conf.teamchan);
	else
		irc_send("PRIVMSG %s :Die Playlist konnte nicht gestoppt werden", radioplaylist_conf.teamchan);

	return 1;
}

COMMAND(playlist_countdown)
{
	int16_t remaining = stream_state.endtime - now;

	if(!stream_state.playing)
	{
		reply("Die Playlist ist nicht an");
		return 0;
	}

	if(playlist_cd_by || stream_state.play == 2)
	{
		reply("Es läuft bereits ein Countdown für $b%s$b", playlist_cd_by);
		return 0;
	}

	if(remaining < 5)
	{
		reply("Countdown nicht möglich; der aktuelle Song endet in weniger als 5s");
		return 0;
	}

	playlist_cd_tick = now;
	playlist_cd_by = strdup(src->nick);
	reg_loop_func(check_countdown);

	pthread_mutex_lock(&stream_state_mutex);
	stream_state.play = 2;
	pthread_mutex_unlock(&stream_state_mutex);

	if(!channel || strcasecmp(radioplaylist_conf.teamchan, channel->name))
		irc_send("PRIVMSG %s,%s :Die Playlist wird in $b%02u:%02u$b ausgeschaltet (%s)", radioplaylist_conf.teamchan, playlist_cd_by, remaining / 60, remaining % 60, src->nick);
	else
		irc_send("PRIVMSG %s,%s :Die Playlist wird in $b%02u:%02u$b ausgeschaltet", radioplaylist_conf.teamchan, playlist_cd_by, remaining / 60, remaining % 60);
	return 1;
}

COMMAND(playlist_next)
{
	struct playlist_node *cur;

	if(!stream_state.playing)
	{
		reply("Die Playlist ist nicht an");
		return 0;
	}

	pthread_mutex_lock(&playlist_mutex);
	if(!(cur = stream_state.playlist->queue_cur))
		cur = stream_state.playlist->cur;
	pthread_mutex_unlock(&playlist_mutex);

	if(cur)
		irc_send("PRIVMSG %s :Song \xc3\xbc""bersprungen: [%"PRIu32"] %s - %s - %s", radioplaylist_conf.teamchan, cur->id, cur->artist, cur->album, cur->title);
	else
		irc_send("PRIVMSG %s :Song \xc3\xbc""bersprungen", radioplaylist_conf.teamchan);

	pthread_mutex_lock(&stream_state_mutex);
	stream_state.skip = 1;
	pthread_mutex_unlock(&stream_state_mutex);

	return 1;
}

COMMAND(playlist_status)
{
	uint16_t elapsed, duration;
	struct playlist_node *cur;

	if(!stream_state.playing)
	{
		reply("Die Playlist ist nicht an");
		return 1;
	}

	// Lock stream state as we have to read multiple values which should be from the same song
	pthread_mutex_lock(&stream_state_mutex);
	elapsed = now - stream_state.starttime;
	duration = stream_state.duration;
	pthread_mutex_unlock(&stream_state_mutex);

	pthread_mutex_lock(&playlist_mutex);
	if(!(cur = stream_state.playlist->queue_cur))
		cur = stream_state.playlist->cur;
	pthread_mutex_unlock(&playlist_mutex);

	if(cur)
		reply("Playlist ist aktiv: [%"PRIu32"] %s - %s - %s [%02u:%02u/%02u:%02u]", cur->id, cur->artist, cur->album, cur->title, elapsed / 60, elapsed % 60, duration / 60, duration % 60);
	else
		reply("Playlist ist aktiv: unknown [%02u:%02u/%02u:%02u]", elapsed / 60, elapsed % 60, duration / 60, duration % 60);
	return 1;
}

COMMAND(playlist_play)
{
	struct playlist_node *node = NULL;
	char *arg, *filename, *orig;
	uint32_t id;
	uint8_t silent = 0;

	if(!stream_state.playlist)
	{
		reply("Es ist keine Playlist geladen");
		return 0;
	}

	arg = argv[1];
	if(*arg == '!')
	{
		silent = 1;
		arg++;
	}

	if(argc == 2 && aredigits(arg)) // exactly one argument which consists of digits -> enqueue by id
	{
		id = strtoul(arg, NULL, 10);
		if(!id)
		{
			reply("Ungültige Song-ID");
			return 1;
		}

		node = stream_state.playlist->get_node(stream_state.playlist, id);
		if(!node)
		{
			reply("Ein Song mit der ID %"PRIu32" existiert nicht in der Playlist", id);
			return 0;
		}
	}
	else
	{
		orig = filename = untokenize(argc - 1, argv + 1, " ");
		if(match("*.mp3", filename))
		{
			reply("Dateiname muss auf *.mp3 enden oder eine Song-ID sein");
			free(orig);
			return 0;
		}

		if(*filename == '!')
		{
			silent = 1;
			filename++;
		}

		if(!file_exists(filename))
		{
			reply("Datei nicht gefunden");
			free(orig);
			return 0;
		}

		node = stream_state.playlist->make_node(stream_state.playlist, filename);
		if(!node)
		{
			reply("Datei konnte nicht in die Playlist geladen werden");
			free(orig);
			return 0;
		}
		free(orig);
	}

	assert_return(node, 0);

	pthread_mutex_lock(&playlist_mutex);
	stream_state.playlist->enqueue(stream_state.playlist, node);
	pthread_mutex_unlock(&playlist_mutex);

	if(silent)
		reply("N\xc3\xa4""chster Song: %s - %s - %s", node->artist, node->album, node->title);
	else
		irc_send("PRIVMSG %s :N\xc3\xa4""chster Song: %s - %s - %s", radioplaylist_conf.teamchan, node->artist, node->album, node->title);

	return 1;
}

COMMAND(playlist_blacklist)
{
	uint32_t id;
	int8_t rc;

	if(!stream_state.playlist)
	{
		reply("Es ist keine Playlist geladen");
		return 0;
	}

	if(!(id = strtoul(argv[1], NULL, 10)))
	{
		reply("Ungültige Song-ID");
		return 0;
	}

	rc = stream_state.playlist->blacklist_id(stream_state.playlist, id);
	if(rc == 0)
		reply("Song %"PRIu32" wurde zur Blacklist hinzugefügt", id);
	else
		reply("Song %"PRIu32" konnte nicht zur Blacklist hinzugefügt werden", id);

	return rc ? 0 : 1;
}

COMMAND(playlist_reload)
{
	struct playlist *playlist;

	if(!pg_conn)
	{
		reply("Datenbank ist nicht verfügbar");
		return 0;
	}

	if(!(playlist = playlist_load(pg_conn, PL_L_RANDOMIZE)))
	{
		reply("Playlist konnte nicht geladen werden");
		return 0;
	}

	log_append(LOG_INFO, "playlist contains %"PRIu32" tracks", playlist->count);
	if(playlist->count == 0)
	{
		reply("Playlist ist leer; alte Playlist wird beibehalten");
		playlist->free(playlist);
	}
	else
	{
		reply("Playlist wurde geladen (%"PRIu32" tracks)", playlist->count);
		pthread_mutex_lock(&playlist_mutex);
		if(stream_state.playlist)
			stream_state.playlist->free(stream_state.playlist);
		stream_state.playlist = playlist;
		pthread_mutex_unlock(&playlist_mutex);
	}

	return 1;
}

COMMAND(playlist_add)
{
	char *filename;
	struct pgsql *conn;
	struct stat sb;
	int8_t rc;

	filename = untokenize(argc - 1, argv + 1, " ");

	if(stat(filename, &sb) == -1)
	{
		reply("Ungültige Datei: %s", strerror(errno));
		free(filename);
		return 0;
	}

	if(!S_ISREG(sb.st_mode))
	{
		reply("Es können nur einzelne Dateien hinzugefügt werden");
		free(filename);
		return 0;
	}

	if(!(conn = pgsql_init(radioplaylist_conf.db_conn_string)))
	{
		reply("Konnte keine Verbindung zur Datenbank aufbauen");
		free(filename);
		return 0;
	}

	rc = playlist_add_file(filename, conn, &sb);

	if(rc == 0)
		reply("Datei wurde zur Playlist hinzugefügt");
	else
		reply("Datei konnte nicht zur Playlist hinzugefügt werden");

	pgsql_fini(conn);
	free(filename);
	return (rc == 0);
}

COMMAND(playlist_scan)
{
	char *path;
	struct stat sb;
	pthread_attr_t attr;

	if(scan_state.state != SCAN_IDLE)
	{
		reply("Es wird bereits ein Ordner gescannt");
		return 0;
	}

	path = untokenize(argc - 1, argv + 1, " ");

	if(stat(path, &sb) == -1)
	{
		reply("Ungültiger Ordner: %s", strerror(errno));
		free(path);
		return 0;
	}

	if(!S_ISDIR(sb.st_mode))
	{
		reply("Es können nur komplette Ordner hinzugefügt werden");
		free(path);
		return 0;
	}

	memset(&scan_state, 0, sizeof(scan_state));
	scan_state.state = SCAN_ACTIVE;
	scan_state.path = path;
	scan_state.nick = strdup(src->nick);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&scan_thread, &attr, scan_thread_main, NULL);
	pthread_attr_destroy(&attr);
	reply("Scan gestartet");
	reg_loop_func(check_scan_result);
	return 1;
}

COMMAND(playlist_check)
{
	struct pgsql *conn;
	int32_t count;

	if(!(conn = pgsql_init(radioplaylist_conf.db_conn_string)))
	{
		reply("Konnte keine Verbindung zur Datenbank aufbauen");
		return 0;
	}

	pgsql_begin(conn);
	count = playlist_scan(NULL, conn, PL_S_REMOVE_MISSING);
	if(count < 0)
	{
		pgsql_rollback(conn);
		reply("Beim Überprüfen ist ein Fehler aufgetreten");
	}
	else
	{
		pgsql_commit(conn);
		reply("Überprüfung abgeschlossen; es wurde%s %"PRId32" Songs gelöscht", (count != 1 ? "n" : ""), count);
	}

	pgsql_fini(conn);
	return 1;
}

COMMAND(playlist_truncate)
{
	struct pgsql *conn;
	static time_t truncate_ts = 0;
	int32_t rc;

	if((now - truncate_ts) > 3)
	{
		truncate_ts = now;
		reply("Um wirklich die gesamte Playlist zu $c4löschen$c, rufe diesen Befehl innerhalb der nächsten 3s erneut auf");
		return 0;
	}

	truncate_ts = 0;

	if(!(conn = pgsql_init(radioplaylist_conf.db_conn_string)))
	{
		reply("Konnte keine Verbindung zur Datenbank aufbauen");
		return 0;
	}

	pgsql_begin(conn);
	rc = playlist_scan(NULL, conn, PL_S_TRUNCATE);
	if(rc >= 0)
	{
		pgsql_commit(conn);
		reply("Playlist wurde gelöscht");
	}
	else
	{
		pgsql_rollback(conn);
		reply("Playlist konnte nicht gelöscht werden");
	}

	pgsql_fini(conn);
	return 1;
}

static void check_countdown()
{
	int16_t remaining = stream_state.endtime - now;

	// If the stream is still active but no time is remaining, wait about 1 second
	if(stream_state.playing && remaining <= 0)
	{
		while(remaining >= -1 && stream_state.playing)
			usleep(25000);
	}

	// If the stream has stopped now, notify the user
	if(!stream_state.playing)
	{
		irc_send("PRIVMSG %s,%s :Die Playlist ist jetzt $c4AUS$c -> ab auf den Stream, $b%s$b", radioplaylist_conf.teamchan, playlist_cd_by, playlist_cd_by);
		unreg_loop_func(check_countdown);
		MyFree(playlist_cd_by);
		return;
	}

	// If we already had a tick this second, do nothing
	if(playlist_cd_tick >= now)
		return;

	if(remaining >= 0)
	{
		// Display remaining time in 1-second intervals during the last ten seconds and in larger intervals if there's more time remaining
		if((remaining < 10) || (remaining <= 60 && (remaining % 10) == 0) || ((remaining % 30) == 0))
		{
			irc_send("PRIVMSG %s :Die Playlist wird in $b%02u:%02u$b ausgeschaltet", playlist_cd_by, remaining / 60, remaining % 60);
		}
	}
	else if((remaining % 5) == 0) // overtime, display message every 5 seconds
	{
		irc_send("PRIVMSG %s :Die Playlist wird in Kürze augeschaltet", playlist_cd_by);
	}

	playlist_cd_tick = now;
}

static void check_scan_result()
{
	if(scan_state.state != SCAN_FINISHED)
		return;

	unreg_loop_func(check_scan_result);
	assert(scan_state.path);
	assert(scan_state.nick);

	if(scan_state.count >= 0)
		reply_nick(scan_state.nick, "Scan von $b%s$b abgeschlossen; es wurden %"PRId32" neue Songs gefunden", scan_state.path, scan_state.count);
	else
		reply_nick(scan_state.nick, "Beim Scan von $b%s$b ist ein Fehler aufgetreten", scan_state.path);

	free(scan_state.path);
	free(scan_state.nick);
	scan_state.count = 0;
	scan_state.state = SCAN_IDLE;
}

static void conf_reload_hook()
{
	const char *str;

	str = conf_get("radioplaylist/db_conn_string", DB_STRING);
	radioplaylist_conf.db_conn_string = str ? str : "";

	pthread_mutex_lock(&conf_mutex); // lock config
	str = conf_get("radioplaylist/stream_ip", DB_STRING);
	radioplaylist_conf.stream_ip = str ? str : "127.0.0.1";

	str = conf_get("radioplaylist/stream_port", DB_STRING);
	radioplaylist_conf.stream_port = str ? atoi(str) : 8000;

	str = conf_get("radioplaylist/stream_pass", DB_STRING);
	radioplaylist_conf.stream_pass = str ? str : "secret";

	str = conf_get("radioplaylist/stream_name", DB_STRING);
	radioplaylist_conf.stream_name = str;

	str = conf_get("radioplaylist/stream_genre", DB_STRING);
	radioplaylist_conf.stream_genre = str;

	str = conf_get("radioplaylist/stream_url", DB_STRING);
	radioplaylist_conf.stream_url = str;

	str = conf_get("radioplaylist/lame_bitrate", DB_STRING);
	radioplaylist_conf.lame_bitrate = str ? atoi(str) : 192;

	str = conf_get("radioplaylist/lame_samplerate", DB_STRING);
	radioplaylist_conf.lame_samplerate = str ? atoi(str) : 44100;

	str = conf_get("radioplaylist/lame_quality", DB_STRING);
	radioplaylist_conf.lame_quality = str ? atoi(str) : 3;
	pthread_mutex_unlock(&conf_mutex); // unlock config

	str = conf_get("radioplaylist/teamchan", DB_STRING);
	radioplaylist_conf.teamchan = str;

	if(!pg_conn || !(str = conf_get_old("radioplaylist/db_conn_string", DB_STRING)) || strcmp(str, radioplaylist_conf.db_conn_string))
	{
		struct pgsql *new_conn = pgsql_init(radioplaylist_conf.db_conn_string);
		if(new_conn)
		{
			// no need to lock the playlist mutex here as we never access its pgsql connection from the stream thread
			if(pg_conn)
				pgsql_fini(pg_conn);
			pg_conn = new_conn;
		}

		if(pg_conn)
		{
			if(stream_state.playlist)
				stream_state.playlist->conn = pg_conn;
			else
			{
				// If there was no playlist, load it now
				struct playlist *playlist = playlist_load(pg_conn, PL_L_RANDOMIZE);
				if(playlist)
				{
					log_append(LOG_INFO, "playlist contains %"PRIu32" tracks", playlist->count);
					pthread_mutex_lock(&playlist_mutex);
					stream_state.playlist = playlist;
					pthread_mutex_unlock(&playlist_mutex);
				}
			}
		}
	}
}

/* scanning code */
static void *scan_thread_main(void *arg)
{
	struct pgsql *conn;

	if(!(conn = pgsql_init(radioplaylist_conf.db_conn_string)))
		scan_state.count = -1;
	else
	{
		pgsql_begin(conn);
		scan_state.count = playlist_scan(scan_state.path, conn, 0);
		if(scan_state.count < 0)
		{
			debug("scan failed");
			pgsql_rollback(conn);
		}
		else
		{
			debug("found %"PRId32" new files", scan_state.count);
			pgsql_commit(conn);
		}
		pgsql_fini(conn);
	}

	scan_state.state = SCAN_FINISHED;
	debug("scan thread exiting");
	pthread_exit(NULL);
}


/* streaming code */
static void *stream_thread_main(void *arg)
{
	struct stream_ctx stream;

	shout_init();
	while(!stream_state.terminate)
	{
		debug("waiting for stream condition");
		pthread_mutex_lock(&stream_mutex);
		pthread_cond_wait(&stream_cond, &stream_mutex);
		pthread_mutex_unlock(&stream_mutex);
		if(stream_state.terminate)
			continue;

		if(stream_init(&stream) != 0)
		{
			stream_fini(&stream);
			pthread_mutex_lock(&stream_state_mutex);
			stream_state.play = 0;
			stream_state.playing = 0;
			pthread_mutex_unlock(&stream_state_mutex);
			continue;
		}

		while(stream_state.play == 1 && !stream_state.terminate)
		{
			char titlebuf[768];
			char file[PATH_MAX];
			struct playlist_node *song;

			// Unset skip flag if it is still set for some reason, also set playing flag
			if(stream_state.skip || !stream_state.playing)
			{
				pthread_mutex_lock(&stream_state_mutex);
				stream_state.playing = 1;
				stream_state.skip = 0;
				pthread_mutex_unlock(&stream_state_mutex);
			}

			pthread_mutex_lock(&playlist_mutex); // lock playlist
			song = stream_state.playlist ? stream_state.playlist->next(stream_state.playlist) : NULL;
			if(!song)
			{
				pthread_mutex_unlock(&playlist_mutex); // unlock playlist
				log_append(LOG_WARNING, "empty playlist");
				break;
			}

			strlcpy(file, song->file, sizeof(file));

			if(song->artist && song->title)
				snprintf(titlebuf, sizeof(titlebuf), "%s - %s", song->artist, song->title);
			else if(song->title)
				strlcpy(titlebuf, song->title, sizeof(titlebuf));
			else if(song->artist)
				strlcpy(titlebuf, song->artist, sizeof(titlebuf));
			else
				strcpy(titlebuf, "Unknown Song");

			pthread_mutex_lock(&stream_state_mutex);
			stream_state.duration = song->duration;
			stream_state.starttime = time(NULL);
			stream_state.endtime = stream_state.starttime + song->duration;
			pthread_mutex_unlock(&stream_state_mutex);

			log_append(LOG_INFO, "Streaming: %s - %s - %s (%02u:%02u)", song->artist, song->album, song->title, song->duration / 60, song->duration % 60);
			pthread_mutex_unlock(&playlist_mutex); // unlock playlist

			stream.meta = shout_metadata_new();
			shout_metadata_add(stream.meta, "song", titlebuf);
			shout_metadata_add(stream.meta, "url", "http://");

			stream_song(&stream, file);

			shout_metadata_free(stream.meta);
			stream.meta = NULL;

			if(shout_get_errno(stream.shout) == SHOUTERR_SOCKET)
			{
				log_append(LOG_WARNING, "shout reports socket error: %s", shout_get_error(stream.shout));
				break;
			}
		}

		stream_fini(&stream);
		pthread_mutex_lock(&stream_state_mutex);
		stream_state.play = 0;
		stream_state.playing = 0;
		pthread_mutex_unlock(&stream_state_mutex);
	}

	shout_shutdown();
	debug("stream thread exiting");
	pthread_exit(NULL);
}

static int8_t stream_init(struct stream_ctx *stream)
{
	memset(stream, 0, sizeof(struct stream_ctx));

	stream->shout = shout_new();
	pthread_mutex_lock(&conf_mutex); // lock config
	shout_set_host(stream->shout, radioplaylist_conf.stream_ip);
	shout_set_protocol(stream->shout, SHOUT_PROTOCOL_ICY);
	shout_set_port(stream->shout, radioplaylist_conf.stream_port);
	shout_set_password(stream->shout, radioplaylist_conf.stream_pass);
	if(radioplaylist_conf.stream_name)
		shout_set_name(stream->shout, radioplaylist_conf.stream_name);
	if(radioplaylist_conf.stream_genre)
		shout_set_genre(stream->shout, radioplaylist_conf.stream_genre);
	if(radioplaylist_conf.stream_url)
		shout_set_url(stream->shout, radioplaylist_conf.stream_url);
	pthread_mutex_unlock(&conf_mutex); // unlock config
	shout_set_format(stream->shout, SHOUT_FORMAT_MP3);
	if(shout_open(stream->shout) != SHOUTERR_SUCCESS)
	{
		log_append(LOG_WARNING, "shout_open failed: %s", shout_get_error(stream->shout));
		return -1;
	}

	return 0;
}

static void stream_lame_init(struct stream_ctx *stream)
{
	if(stream->lame)
		lame_close(stream->lame);
	stream->lame = lame_init();
	pthread_mutex_lock(&conf_mutex); // lock config
	lame_set_out_samplerate(stream->lame, radioplaylist_conf.lame_samplerate);
	lame_set_brate(stream->lame, radioplaylist_conf.lame_bitrate);
	lame_set_quality(stream->lame, radioplaylist_conf.lame_quality);
	pthread_mutex_unlock(&conf_mutex); // unlock config
}

static void stream_fini(struct stream_ctx *stream)
{
	if(stream->lame)
		lame_close(stream->lame);
	shout_close(stream->shout);
	shout_free(stream->shout);
}

static int8_t stream_song(struct stream_ctx *stream, const char *filename)
{
	int fd;
	struct stat sb;
	void *filedata;

	if((fd = open(filename, O_RDONLY)) == -1)
	{
		char errbuf[64];
		log_append(LOG_WARNING, "open(%s) failed: %s", filename, strerror_r(errno, errbuf, sizeof(errbuf)));
		return -1;
	}

	if(fstat(fd, &sb) == -1)
	{
		char errbuf[64];
		log_append(LOG_WARNING, "fstat() failed: %s", strerror_r(errno, errbuf, sizeof(errbuf)));
		return -1;
	}

	if(sb.st_size == 0)
	{
		log_append(LOG_WARNING, "file %s is empty", filename);
		return -1;
	}

	if((filedata = mmap(0, sb.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
	{
		char errbuf[64];
		log_append(LOG_WARNING, "mmap() failed: %s", strerror_r(errno, errbuf, sizeof(errbuf)));
		return -1;
	}

	stream->start = filedata;
	stream->length = sb.st_size;
	stream->samplerate = 0;

	decode_mp3(stream);

	munmap(filedata, sb.st_size);
	close(fd);
	return 0;
}

static int decode_mp3(struct stream_ctx *stream)
{
	struct mad_decoder decoder;
	unsigned char mp3buf[LAME_MAXMP3BUFFER];
	ssize_t olen;
	int result;

	mad_decoder_init(&decoder, stream,
			 input_cb, NULL /* header */, NULL /* filter */, output_cb,
			 error_cb, NULL /* message */);

	result = mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
	olen = lame_encode_flush_nogap(stream->lame, mp3buf, sizeof(mp3buf));
	shout_sync(stream->shout);
	if(shout_send(stream->shout, mp3buf, olen) != SHOUTERR_SUCCESS)
		log_append(LOG_WARNING, "shout_send failed: %s", shout_get_error(stream->shout));

	mad_decoder_finish(&decoder);
	return result;
}

static enum mad_flow input_cb(void *data, struct mad_stream *stream)
{
	struct stream_ctx *ctx = data;

	if(!ctx->length)
		return MAD_FLOW_STOP;

	mad_stream_buffer(stream, ctx->start, ctx->length);
	ctx->length = 0;
	return MAD_FLOW_CONTINUE;
}

static enum mad_flow output_cb(void *data, struct mad_header const *header, struct mad_pcm *pcm)
{
	struct stream_ctx *ctx = data;
	unsigned int nchannels, nsamples;
	mad_fixed_t *left_ch, *right_ch;
	unsigned char mp3buf[LAME_MAXMP3BUFFER];
	ssize_t olen;

	nchannels = pcm->channels;
	nsamples = pcm->length;
	left_ch = pcm->samples[0];
	right_ch = nchannels > 1 ? pcm->samples[1] : left_ch;

	// First call
	if(!ctx->samplerate)
	{
		debug("channels: %u, sample rate: %u kHz", nchannels, header->samplerate);
		assert_return(nchannels <= 2, MAD_FLOW_BREAK);
		ctx->samplerate = header->samplerate;
		if(!ctx->lame || (ctx->last_samplerate && ctx->last_samplerate != ctx->samplerate))
		{
			stream_lame_init(ctx);
			lame_set_in_samplerate(ctx->lame, header->samplerate);
			lame_init_params(ctx->lame);
		}
		shout_set_metadata(ctx->shout, ctx->meta);
	}

	assert_return(ctx->samplerate == header->samplerate, MAD_FLOW_BREAK);

	float pcm_data[2][1152];
	for(unsigned int i = 0; i < nsamples; i++)
	{
		pcm_data[0][i] = (float)mad_f_todouble(left_ch[i]) * 32767.0;
		pcm_data[1][i] = (float)mad_f_todouble(right_ch[i]) * 32767.0;
	}

	olen = lame_encode_buffer_float(ctx->lame, pcm_data[0], pcm_data[1], nsamples, mp3buf, sizeof(mp3buf));

	shout_sync(ctx->shout);
	if(shout_send(ctx->shout, mp3buf, olen) != SHOUTERR_SUCCESS)
	{
		log_append(LOG_WARNING, "shout_send failed: %s", shout_get_error(ctx->shout));
		return MAD_FLOW_BREAK;
	}

	if(!stream_state.play || stream_state.terminate)
		return MAD_FLOW_STOP;
	else if(stream_state.skip)
	{
		pthread_mutex_lock(&stream_state_mutex);
		stream_state.skip = 0;
		pthread_mutex_unlock(&stream_state_mutex);
		return MAD_FLOW_STOP;
	}

	return MAD_FLOW_CONTINUE;
}


static enum mad_flow error_cb(void *data, struct mad_stream *stream, struct mad_frame *frame)
{
	struct stream_ctx *ctx = data;

	// Ignore sync lost error the beginning.. it's caused by tags
	if((stream->this_frame - ctx->start) == 0 && stream->error == MAD_ERROR_LOSTSYNC)
		return MAD_FLOW_CONTINUE;

	debug("decoding error 0x%04x (%s) at byte offset %zu",
		stream->error, mad_stream_errorstr(stream),
		stream->this_frame - ctx->start);

	return MAD_FLOW_CONTINUE;
}
