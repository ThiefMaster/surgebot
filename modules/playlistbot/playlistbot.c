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
#include "mp3.h"

#include <math.h>
#include <shout/shout.h>
#include <pthread.h>
#include <lame/lame.h>

#define STREAM_BITRATE 192

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

static lame_global_flags *lame = NULL;

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

/* For each song, reset the liblame engine, otherwise it craps out if
 * the bitrate or sample rate changes */
static void ices_reencode_reset(struct mp3_file *source)
{
	static int init_decoder = 1;

	if(!init_decoder)
	{
		lame_decode_exit();
		init_decoder = 1;
	}

	if(init_decoder)
	{
		lame_decode_init();
		init_decoder = 0;
	}

	/* only reset encoder if audio format changes */
	if(lame)
	{
		if(lame_get_in_samplerate(lame) == (int)source->samplerate)
			return;

		lame_close(lame);
	}

	lame = lame_init();
	lame_set_in_samplerate(lame, source->samplerate);
	/* Lame won't reencode mono to stereo for some reason, so we have to
	 * duplicate left into right by hand. */
	if(source->channels == 1)
		lame_set_num_channels(lame, source->channels);

	lame_set_brate(lame, STREAM_BITRATE);
	lame_set_out_samplerate(lame, 44100);

	lame_init_params(lame);
}


/* decode buffer, of length buflen, into left and right. Stream-independent
 * (do this once per chunk, not per stream). Result is number of samples
 * for ices_reencode_reencode_chunk. */
static int ices_reencode_decode(unsigned char *buf, size_t blen, size_t olen, int16_t *left, int16_t *right)
{
	int outlen = lame_decode(buf, blen, left, right);
	/*
	debug("lame_decode(%p, %u, %p, %p) = %u", buf, blen, left, right, outlen);
	printf("%p = \"", buf);
	for(size_t i = 0; i < blen; i++)
		if(isprint(buf[i]))
			printf("%c", buf[i]);
		else
			printf("\\%o", buf[i]);
	printf("\"\n");
	*/
	return outlen;
}

/* reencode buff, of len buflen, put max outlen reencoded bytes in outbuf */
static int ices_reencode(int nsamples, int16_t *left, int16_t *right, unsigned char *outbuf, int outlen)
{
	return lame_encode_buffer(lame, left, right, nsamples, outbuf, outlen);
}

/* At EOF of each file, flush the liblame buffers and get some extra candy */
static int ices_reencode_flush(unsigned char *outbuf, int maxlen)
{
	return lame_encode_flush_nogap(lame, outbuf, maxlen);
}

static int stream_needs_reencoding(struct mp3_file *source)
{
	if ((source->bitrate != STREAM_BITRATE) || (source->samplerate != 44100) || (source->channels != 2))
		return 1;
	return 0;
}

static int stream_send_data(unsigned char* buf, size_t len)
{
	if (shout_get_connected(shout) != SHOUTERR_CONNECTED)
		return -1;

	shout_sync(shout);
	if (shout_send(shout, buf, len) == SHOUTERR_SUCCESS)
		return 0;

	log_append(LOG_WARNING, "Libshout reported send error, !disconnecting: %s", shout_get_error(shout));
	//shout_close(shout);

	return -1;
}

static int stream_send(struct mp3_file *source)
{
	unsigned char ibuf[INPUT_BUFSIZE];
	ssize_t len;
	ssize_t olen;
	int samples;
	int rc;
	int do_sleep;
	int decode = 0;
	struct {
		unsigned char *data;
		size_t len;
	} obuf;
	/* worst case decode: 22050 Hz at 8kbs = 44.1 samples/byte */
	static int16_t left[INPUT_BUFSIZE * 45];
	static int16_t right[INPUT_BUFSIZE * 45];
	static int16_t* rightp;

	obuf.data = NULL;
	obuf.len = 0;

	ices_reencode_reset(source);
	decode = stream_needs_reencoding(source);
	debug("Recoding needed: %s", decode ? "yes" : "no");

	if (decode)
	{
		obuf.len = OUTPUT_BUFSIZE;
		if (!(obuf.data = malloc(OUTPUT_BUFSIZE)))
		{
			log_append(LOG_ERROR, "Error allocating encode buffer");
			return -1;
		}
	}

	debug("Playing %s", source->path);

	while(stream_stop != 2)
	{
		if(stream_next)
		{
			pthread_mutex_lock(&options_mutex);
			stream_next = 0;
			pthread_mutex_unlock(&options_mutex);
			break;
		}

		len = samples = 0;
		/* fetch input buffer */
		len = source->read (source, ibuf, sizeof (ibuf));
		if (decode)
			samples = ices_reencode_decode (ibuf, len, sizeof (left), left, right);

		if (len == 0)
		{
			debug("Done sending");
			break;
		}
		else if (len < 0)
		{
			log_append(LOG_WARNING, "Read error: %s", strerror(errno));
			goto err;
		}

		do_sleep = 1;
		while (do_sleep)
		{
			rc = olen = 0;
			/* don't reencode if the source is MP3 and the same bitrate */
			if (stream_needs_reencoding(source))
			{
				if (samples > 0)
				{
					/* for some reason we have to manually duplicate right from left to get
					 * LAME to output stereo from a mono source */
					if (source->channels == 1)
						rightp = left;
					else
						rightp = right;
					if (obuf.len < (unsigned int) (7200 + samples + samples / 4))
					{
						unsigned char *tmpbuf;

						/* pessimistic estimate from lame.h */
						obuf.len = 7200 + 5 * samples / 2;
						if (!(tmpbuf = realloc(obuf.data, obuf.len)))
						{
							log_append(LOG_WARNING, "Error growing output buffer, aborting track");
							goto err;
						}
						obuf.data = tmpbuf;
						debug("Grew output buffer to %d bytes", (int)obuf.len);
					}

					if ((olen = ices_reencode(samples, left, rightp, obuf.data, obuf.len)) < -1)
					{
						log_append(LOG_WARNING, "Reencoding error, aborting track");
						goto err;
					}
					else if (olen == -1)
					{
						unsigned char *tmpbuf;
						if ((tmpbuf = realloc(obuf.data, obuf.len + OUTPUT_BUFSIZE)))
						{
							obuf.data = tmpbuf;
							obuf.len += OUTPUT_BUFSIZE;
							debug("Grew output buffer to %d bytes", (int)obuf.len);
						}
						else
						{
							log_append(LOG_ERROR, "%d byte output buffer is too small", (int)obuf.len);
						}
					}
					else if (olen > 0)
					{
						rc = stream_send_data(obuf.data, olen);
					}
				}
			}
			else
			{
				rc = stream_send_data(ibuf, len);
			}

			if (rc < 0)
				log_append(LOG_WARNING, "Error during send");
			else
				do_sleep = 0;
			/* this is so if we have errors on every stream we pause before
			 * attempting to reconnect */
			if (do_sleep)
			{
				struct timeval delay;
				delay.tv_sec = 1;
				delay.tv_usec = 0;
				select(1, NULL, NULL, NULL, &delay);
			}
		}
	}

	if (stream_needs_reencoding(source))
	{
		len = ices_reencode_flush(obuf.data, obuf.len);
		if (len > 0)
			rc = stream_send_data(obuf.data, len);
	}

	if (obuf.data)
		free(obuf.data);

	return 0;

err:
	if (obuf.data)
		free(obuf.data);
	return -1;
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
		struct playlist_node *node;
		struct mp3_file source;

		pthread_mutex_lock(&playlist_mutex);
		node = playlist_next(playlist);

		source.path = node->file;
		if(stream_open_source(&source) < 0)
		{
			log_append(LOG_WARNING, "Error opening %s", source.path);
			continue;
		}

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

		stream_send(&source);
		source.close(&source);

		/*
		unsigned char ibuf[INPUT_BUFSIZE];
		ssize_t len, olen;
		int samples, rc, decode = 0;
		struct {
			char *data;
			size_t len;
		} obuf;
		static int16_t left[INPUT_BUFSIZE * 45];
		static int16_t right[INPUT_BUFSIZE * 45];
		static int16_t *rightp;

		obuf.data = NULL;
		obuf.len = 0;

		ices_reencode_reset(samplerate, channels);
		if(bitrate != 192 || samplerate != 44100 || channels != 2)
			decode = 1;

		debug("Decode: %u", decode);

		if(decode)
		{
			obuf.len = OUTPUT_BUFSIZE;
			obuf.data = malloc(OUTPUT_BUFSIZE);
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

			len = samples = 0;
			// fetch input buffer
			len = fread(ibuf, 1, sizeof(ibuf), mp3_fp);
			if (decode)
				samples = ices_reencode_decode(ibuf, len, sizeof(left), left, right);

			if (len <= 0) // Nothing else to read or something bad happened
				break;

			while(1)
			{
				rc = olen = 0;
				// don't reencode if the source is MP3 and the same bitrate
				if(decode)
				{
					if(samples > 0)
					{
						// for some reason we have to manually duplicate right from left to get
						// LAME to output stereo from a mono source
						if (channels == 1)
							rightp = left;
						else
							rightp = right;

						if(obuf.len < (unsigned int) (7200 + samples + samples / 4))
						{
							// pessimistic estimate from lame.h
							obuf.len = 7200 + 5 * samples / 2;
							obuf.data = realloc(obuf.data, obuf.len);
						}

						if((olen = ices_reencode(samples, left, rightp, (unsigned char *)obuf.data, obuf.len)) < -1)
						{
							log_append(LOG_ERROR, "Reencoding error, aborting track");
							break;
						}
						else if (olen == -1)
						{
							obuf.len += OUTPUT_BUFSIZE;
							obuf.data = realloc(obuf.data, obuf.len);

						}
						else if (olen > 0)
						{
							rc = shout_send(shout, (unsigned char *)obuf.data, olen);
							shout_sync(shout);
						}
					}
				}
				else
				{
		 			rc = shout_send(shout, ibuf, len);
					shout_sync(shout);
				}

				if(rc == SHOUTERR_SUCCESS)
					break;

				log_append(LOG_WARNING, "Error during send");
				usleep(1000000);
			}
		}

		if(decode)
		{
			len = ices_reencode_flush((unsigned char *)obuf.data, obuf.len);
			if (len > 0)
				rc = shout_send(shout, (unsigned char *)obuf.data, len);
		}

		if (obuf.data)
			free(obuf.data);

		source.close(&source);
		*/

		pthread_cancel(duration_thread);

		if(stream_stop == 1)
			stream_stop = 2;
	}

	if(lame)
	{
		lame_close(lame);
		lame = NULL;
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

static void countdown_tmr(void *bound, void *data);

static void countdown_tick()
{
	pthread_mutex_lock(&status_mutex);
	if(last_tick < now && ((playlist_status.endtime - now) < 10 || ((((playlist_status.endtime - now) % 10) == 0 && playlist_status.endtime - now <= 60) || (((playlist_status.endtime - now) % 30) == 0))))
	{
		if((long)(playlist_status.endtime >= (unsigned long)now))
			irc_send("PRIVMSG %s :Playlist wird in $b%lu$b Sekunden ausgeschaltet.", playlist_cd_by, playlist_status.endtime - now);
		last_tick = now;
	}
	pthread_mutex_unlock(&status_mutex);

	// Check if the stream is finally disabled as the playtime of the current song was wrong
	if((playlist_status.endtime < (unsigned long)now) && stream_stop >= 3)
		countdown_tmr(NULL, NULL);
}

static void countdown_tmr(void *bound, void *data)
{
	// If we are already delayed and the playlist is still active, do not wait again
	if(now - playlist_status.endtime > 3 && stream_stop < 3)
		return;

	unreg_loop_func(countdown_tick);
	time_t waiting_since = now;
	while(stream_stop < 3) // should finish quite fast so the delay can be ignored
	{
		if((time(NULL) - waiting_since) > 3) // Seems to take longer, start checking in the loop function
		{
			pthread_mutex_lock(&status_mutex);
			timer_add(this, "countdown_finished", playlist_status.endtime, countdown_tmr, NULL, 0, 0);
			pthread_mutex_unlock(&status_mutex);
			reg_loop_func(countdown_tick);
			irc_send("PRIVMSG %s :Playlist wird in $bwenigen$b Sekunden ausgeschaltet.", playlist_cd_by);
			return;
		}
		usleep(75000);
	}

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

	pthread_mutex_lock(&options_mutex);
	if(timer_exists_boundname(this, "countdown_finished") || stream_stop == 1)
	{
		reply("Playlist-Countdown ist bereits aktiv");
		return 0;
	}
	pthread_mutex_unlock(&options_mutex);

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
