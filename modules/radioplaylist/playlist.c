#include "global.h"
#include "playlist.h"
#include "stringlist.h"
#include "pgsql.h"

#include <libgen.h>
#include <math.h>
#include <sys/mman.h>

#include <libpq-fe.h>
#include <mad.h>
#include <id3tag.h>

static int8_t playlist_load_db(struct playlist *playlist, uint8_t genre_id, uint8_t flags);
static void playlist_add(struct playlist *playlist, struct playlist_node *node);
static struct playlist_node *playlist_next(struct playlist *playlist);
static struct playlist_node *playlist_find_file(struct playlist *playlist, const char *file);
static int8_t playlist_blacklist(struct playlist *playlist, uint32_t id, struct playlist_node *node);
static int8_t playlist_blacklist_id(struct playlist *playlist, uint32_t id);
static int8_t playlist_blacklist_node(struct playlist *playlist, struct playlist_node *node);
static struct playlist_node *playlist_make_node(struct playlist *playlist, const char *file);
static struct playlist_node *playlist_get_node(struct playlist *playlist, uint32_t id);
static void playlist_enqueue_head(struct playlist *playlist, struct playlist_node *node);
static void playlist_enqueue_tail(struct playlist *playlist, struct playlist_node *node);
static struct playlist *playlist_create();
static void playlist_free(struct playlist *playlist);

static struct playlist_node *playlist_node_create(uint32_t id, const char *file);
static void playlist_node_free(struct playlist_node *node);

static int8_t playlist_scan_dir(const char *path, struct pgsql *conn, struct playlist *playlist, uint8_t depth, uint32_t *new_count, uint32_t *updated_count);
static int8_t playlist_scan_file(struct pgsql *conn, const char *file, struct stat *sb, struct playlist *playlist, uint32_t *new_count, uint32_t *updated_count);
static const mad_timer_t *get_mp3_duration(const char *file, struct stat *sb);
static char *get_id3_entry(const struct id3_tag *tag, const char *id);
static const char *make_absolute_path(const char *relative);


struct playlist *playlist_load(struct pgsql *conn, uint8_t genre_id, uint8_t flags)
{
	struct playlist *playlist = playlist_create();
	playlist->conn = conn;
	playlist_load_db(playlist, genre_id, flags);
	return playlist;
}

static int8_t playlist_load_db(struct playlist *playlist, uint8_t genre_id, uint8_t flags)
{
	char query[256] = "SELECT * FROM playlist";
	int has_where = 0;
	PGresult *res;
	int rows;

	if((flags & PL_L_RANDOMGENRE))
	{
		PGresult *genre_res;
		genre_res = pgsql_query(playlist->conn, "SELECT id FROM genres ORDER BY random() LIMIT 1", 1, NULL);
		if(!genre_res)
		{
			pgsql_free(genre_res);
			return -1;
		}
		else if(pgsql_num_rows(genre_res))
		{
			genre_id = atoi(pgsql_nvalue(genre_res, 0, "id"));
			debug("random genre id: %u", genre_id);
		}
		else
		{
			genre_id = 0;
			debug("no genres found");
		}
		pgsql_free(genre_res);
	}

	if(genre_id)
		strcat(query, " JOIN song_genres s ON (s.song_id = playlist.id)");

	if(!(flags & PL_L_ALL))
	{
		strcat(query, " WHERE blacklist = false");
		has_where = 1;
	}

	if(genre_id)
	{
		if(has_where)
			sprintf(query + strlen(query), " AND s.genre_id = %u", genre_id);
		else
			sprintf(query + strlen(query), " WHERE s.genre_id = %u", genre_id);
	}


	if(flags & PL_L_RANDOMIZE)
		strcat(query, " ORDER BY random()");

	debug("loading playlist (randomize: %s, blacklisted songs: %s, rnd genre: %s, genre: %"PRIu8")",
		((flags & PL_L_RANDOMIZE) ? "yes" : "no"),
		((flags & PL_L_ALL) ? "yes" : "no"),
		((flags & PL_L_RANDOMGENRE) ? "yes" : "no"),
		genre_id);

	if(!(res = pgsql_query(playlist->conn, query, 1, NULL)))
		return -1;

	playlist->load_flags = flags;
	playlist->genre_id = genre_id;

	rows = pgsql_num_rows(res);
	for(int i = 0; i < rows; i++)
	{
		const char *tmp;
		char *file = pgsql_nvalue_bytea(res, i, "file");
		uint32_t id = strtoul(pgsql_nvalue(res, i, "id"), NULL, 10);
		struct playlist_node *node = playlist_node_create(id, file);
		free(file);
		if((tmp = pgsql_nvalue(res, i, "artist")))
			node->artist = strdup(tmp);
		if((tmp = pgsql_nvalue(res, i, "album")))
			node->album = strdup(tmp);
		if((tmp = pgsql_nvalue(res, i, "title")))
			node->title = strdup(tmp);
		node->duration = atoi(pgsql_nvalue(res, i, "duration"));
		node->blacklist = !strcmp(pgsql_nvalue(res, i, "blacklist"), "t");
		node->inode = strtoul(pgsql_nvalue(res, i, "st_inode"), NULL, 10);
		node->size = strtol(pgsql_nvalue(res, i, "st_size"), NULL, 10);
		node->mtime = strtol(pgsql_nvalue(res, i, "st_mtime"), NULL, 10);
		playlist_add(playlist, node);
	}

	pgsql_free(res);
	return 0;
}

static void playlist_add(struct playlist *playlist, struct playlist_node *node)
{
	if(playlist->tail)
	{
		node->prev = playlist->tail;
		node->prev->next = node;
	}

	playlist->tail = node;

	if(!playlist->head)
		playlist->head = node;

	playlist->count++;
}

static struct playlist_node *playlist_next(struct playlist *playlist)
{
	struct playlist_node *node = NULL;

	// If we have an old queued item, free it now
	if(playlist->queue_cur)
	{
		playlist_node_free(playlist->queue_cur);
		playlist->queue_cur = NULL;
	}

	// If we have something in the queue, dequeue the first item and use it
	if(playlist->queue)
	{
		node = playlist->queue;
		playlist->queue = node->next;
		if(playlist->queue)
			playlist->queue->prev = NULL;
		node->next = NULL;
		playlist->queue_cur = node;
		return node;
	}

	node = playlist->cur;

	// If we are at the last node, start from beginning
	if(node && !node->next)
	{
		debug("reached end of playlist, starting at head");
		assert_return(node == playlist->tail, NULL);
		node = NULL;
	}

	// If we have no current node, start with HEAD
	if(!node)
	{
		node = playlist->head;
		if(!node)
			return NULL;
		// If this node is not blacklisted or blacklisted nodes are fine, return it
		if(!node->blacklist || (playlist->load_flags & PL_L_ALL))
		{
			playlist->cur = node;
			return node;
		}
	}

	// Iterate over playlist until we find the next valid node
	do
	{
		node = node->next;
	} while(node->blacklist && !(playlist->load_flags & PL_L_ALL));

	playlist->cur = node;
	return node;
}

static struct playlist_node *playlist_find_file(struct playlist *playlist, const char *file)
{
	for(struct playlist_node *node = playlist->head; node; node = node->next)
	{
		if(!strcmp(node->file, file))
			return node;
	}

	return NULL;
}

static int8_t playlist_blacklist(struct playlist *playlist, uint32_t id, struct playlist_node *node)
{
	char idbuf[16];
	PGresult *res;
	int affected;

	assert_return(!node || node->id == id, -1);

	if(node && node->blacklist)
	{
		debug("%s - %s [#%"PRIu32"] is already blacklisted", node->artist, node->title, id);
		return 0;
	}

	snprintf(idbuf, sizeof(idbuf), "%"PRIu32, id);
	res = pgsql_query(playlist->conn, "UPDATE playlist SET blacklist = true WHERE id = $1", 1, stringlist_build_n(1, idbuf));
	if(!res)
		return -1;

	affected = pgsql_num_affected(res);
	pgsql_free(res);
	if(!affected)
		return 1;

	if(node)
	{
		debug("blacklisted %s - %s (#%"PRIu32") [node]", node->artist, node->title, id);
		node->blacklist = 1;
		return 0;
	}

	if(playlist->cur)
	{
		// Be efficient, most blacklist requests will hit previosly played songs
		node = playlist->cur;
		while(node)
		{
			if(node->id == id)
			{
				debug("blacklisted %s - %s (#%"PRIu32") [<=cur]", node->artist, node->title, id);
				node->blacklist = 1;
				return 0;
			}

			node = node->prev;
		}

		// Node wasn't found in the previous songs, check remaining playlist
		node = playlist->cur->next;
		while(node)
		{
			if(node->id == id)
			{
				debug("blacklisted %s - %s (#%"PRIu32") [>cur]", node->artist, node->title, id);
				node->blacklist = 1;
				return 0;
			}

			node = node->next;
		}
	}
	else
	{
		// No current node -> we have to scan the whole playlist
		node = playlist->head;
		while(node)
		{
			if(node->id == id)
			{
				debug("blacklisted %s - %s (#%"PRIu32")", node->artist, node->title, id);
				node->blacklist = 1;
				return 0;
			}

			node = node->next;
		}
	}

	// Very unlikely to happen
	debug("blacklisted #%"PRIu32, id);
	return 0;
}

static int8_t playlist_blacklist_id(struct playlist *playlist, uint32_t id)
{
	return playlist_blacklist(playlist, id, NULL);
}

static int8_t playlist_blacklist_node(struct playlist *playlist, struct playlist_node *node)
{
	assert_return(node, -1);
	if(!node->id)
		return -1;

	return playlist_blacklist(playlist, node->id, node);
}

static struct playlist_node *playlist_make_node(struct playlist *playlist, const char *file)
{
	const char *real_file;
	struct id3_file *i3f;
	struct playlist_node *node, *existing;
	const mad_timer_t *duration;
	uint16_t duration_secs;

	if(!(existing = playlist_find_file(playlist, file)) && *file != '/')
	{
		// Try using the absolute path of the file
		real_file = make_absolute_path(file);
		if(strcmp(file, real_file))
			existing = playlist_find_file(playlist, real_file);
	}

	if(existing)
	{
		debug("creating node for %s; using metadata from existing entry", file);
		node = playlist_node_create(existing->id, existing->file);
		node->duration = existing->duration;
		node->artist = existing->artist ? strdup(existing->artist) : NULL;
		node->album = existing->album ? strdup(existing->album) : NULL;
		node->title = existing->title ? strdup(existing->title) : NULL;
	}
	else
	{
		debug("creating node for %s", file);
		duration = get_mp3_duration(file, NULL);
		if(!duration)
		{
			log_append(LOG_WARNING, "file %s has no duration, skipping", file);
			return NULL;
		}

		node = playlist_node_create(0, file);
		node->duration = (uint16_t)round(mad_timer_count(*duration, MAD_UNITS_MILLISECONDS) / 1000.0);

		if((i3f = id3_file_open(file, ID3_FILE_MODE_READONLY)))
		{
			const struct id3_tag *tag = id3_file_tag(i3f);
			node->artist = get_id3_entry(tag, ID3_FRAME_ARTIST);
			node->title = get_id3_entry(tag, ID3_FRAME_TITLE);
			node->album = get_id3_entry(tag, ID3_FRAME_ALBUM);
			id3_file_close(i3f);
		}
	}

	return node;
}

static struct playlist_node *playlist_get_node(struct playlist *playlist, uint32_t id)
{
	struct playlist_node *node, *existing = NULL;

	for(struct playlist_node *tmp = playlist->head; tmp; tmp = tmp->next)
	{
		if(tmp->id == id)
		{
			existing = tmp;
			break;
		}
	}

	if(!existing)
		return NULL;

	debug("cloning node with id %"PRIu32" (%s)", existing->id, existing->file);
	node = playlist_node_create(existing->id, existing->file);
	node->duration = existing->duration;
	node->artist = existing->artist ? strdup(existing->artist) : NULL;
	node->album = existing->album ? strdup(existing->album) : NULL;
	node->title = existing->title ? strdup(existing->title) : NULL;
	return node;
}

static void playlist_enqueue_tail(struct playlist *playlist, struct playlist_node *node)
{
	if(!playlist->queue)
		playlist->queue = node;
	else
	{
		struct playlist_node *queue = playlist->queue;
		while(queue->next)
			queue = queue->next;

		queue->next = node;
		node->prev = queue;
	}
}

static void playlist_enqueue_head(struct playlist *playlist, struct playlist_node *node)
{
	node->next = playlist->queue;
	if(node->next)
		node->next->prev = node;
	playlist->queue = node;
}

static struct playlist *playlist_create()
{
	struct playlist *playlist = malloc(sizeof(struct playlist));
	memset(playlist, 0, sizeof(struct playlist));

	playlist->free = playlist_free;
	playlist->next = playlist_next;
	playlist->blacklist_node = playlist_blacklist_node;
	playlist->blacklist_id = playlist_blacklist_id;
	playlist->make_node = playlist_make_node;
	playlist->get_node = playlist_get_node;
	playlist->free_node = playlist_node_free;
	playlist->enqueue = playlist_enqueue_tail;
	playlist->enqueue_first = playlist_enqueue_head;

	return playlist;
}

static void playlist_free(struct playlist *playlist)
{
	if(playlist->queue_cur)
		playlist_node_free(playlist->queue_cur);

	for(struct playlist_node *node = playlist->head; node; )
	{
		struct playlist_node *next = node->next;
		playlist_node_free(node);
		node = next;
	}

	free(playlist);
}

/* playlist node management */
static struct playlist_node *playlist_node_create(uint32_t id, const char *file)
{
	struct playlist_node *node = malloc(sizeof(struct playlist_node));
	memset(node, 0, sizeof(struct playlist_node));
	node->id = id;
	node->file = strdup(file);
	return node;
}

static void playlist_node_free(struct playlist_node *node)
{
	free(node->file);
	MyFree(node->artist);
	MyFree(node->album);
	MyFree(node->title);
	free(node);
}


/* playlist scanning */
int8_t playlist_scan(const char *path, struct pgsql *conn, uint8_t mode, uint32_t *new_count, uint32_t *updated_count)
{
	uint32_t count = 0;
	struct playlist *playlist = NULL;
	int8_t rc;

	if(new_count)
		*new_count = 0;
	if(updated_count)
		*updated_count = 0;

	if((mode & PL_S_REMOVE_MISSING) && mode != PL_S_REMOVE_MISSING)
	{
		log_append(LOG_ERROR, "PL_S_REMOVE_MISSING cannot be combined with other flags");
		return -1;
	}
	else if(mode & PL_S_REMOVE_MISSING)
	{
		playlist = playlist_load(conn, 0, PL_L_ALL);
		assert_return(playlist, -1);
		for(struct playlist_node *node = playlist->head; node; node = node->next)
		{
			if(access(node->file, R_OK) != 0)
			{
				char errbuf[64], idbuf[16];
				log_append(LOG_INFO, "file %s is not readable: %s", node->file, strerror_r(errno, errbuf, sizeof(errbuf)));
				snprintf(idbuf, sizeof(idbuf), "%"PRIu32, node->id);
				pgsql_query(conn, "DELETE FROM playlist WHERE id = $1", 0, stringlist_build_n(1, idbuf));
				count++;
			}
		}
		playlist_free(playlist);
		if(updated_count)
			*updated_count = count;
		return 0;
	}

	// Truncate playlist if requested
	if(mode & PL_S_TRUNCATE)
	{
		log_append(LOG_INFO, "truncating playlist");
		pgsql_query(conn, "DELETE FROM playlist", 0, NULL);
	}

	// If we have no path, exit early. If we didn't truncate the playlist return a failure code.
	if(!path || !*path)
		return (mode & PL_S_TRUNCATE) ? 0 : -1;

	// If we didn't truncate and unmodified files shouldn't be parsed again, load the current playlist
	if(!(mode & (PL_S_TRUNCATE | PL_S_PARSE_ALL)))
		playlist = playlist_load(conn, 0, PL_L_ALL);

	rc = playlist_scan_dir(make_absolute_path(path), conn, playlist, 0, new_count, updated_count);

	if(playlist)
		playlist_free(playlist);
	return rc;
}

int8_t playlist_add_file(const char *file, struct pgsql *conn, struct stat *sb)
{
	int8_t rc;
	rc = playlist_scan_file(conn, make_absolute_path(file), sb, NULL, NULL, NULL);
	return (rc <= 0) ? -1 : 0;
}

static int8_t playlist_scan_dir(const char *path, struct pgsql *conn, struct playlist *playlist, uint8_t depth, uint32_t *new_count, uint32_t *updated_count)
{
	DIR *dir;
	struct dirent *dirent;
	char new_path[PATH_MAX];
	struct stat sb;
	size_t len;
	int32_t rc;

	if(!*path)
		return -1;

	if(!(dir = opendir(path)))
	{
		char errbuf[64];
		log_append(LOG_WARNING, "could not opendir(%s): %s", path, strerror_r(errno, errbuf, sizeof(errbuf)));
		return errno == EACCES ? 0 : -1;
	}

	len = strlcpy(new_path, path, sizeof(new_path));
	if(new_path[len - 1] == '/')
		new_path[--len] = '\0';

	while((dirent = readdir(dir)))
	{
		if(!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))
			continue;

		snprintf(new_path + len, sizeof(new_path) - len, "/%s", dirent->d_name);
		if(stat(new_path, &sb) == -1)
			continue;
		// Recurse into directory
		if(S_ISDIR(sb.st_mode))
		{
			rc = playlist_scan_dir(new_path, conn, playlist, depth + 1, new_count, updated_count);
			if(rc < 0)
			{
				closedir(dir);
				return -1;
			}
		}
		else if(S_ISREG(sb.st_mode))
		{
			const char *ext = strrchr(dirent->d_name, '.');
			if(!ext || strcasecmp(ext, ".mp3"))
				continue;
			rc = playlist_scan_file(conn, new_path, &sb, playlist, new_count, updated_count);
			if(rc < 0)
			{
				closedir(dir);
				return -1;
			}
		}
		else
			log_append(LOG_WARNING, "unexpected dirent type: %s", new_path);
	}

	closedir(dir);
	return 0;
}

static int8_t playlist_scan_file(struct pgsql *conn, const char *file, struct stat *sb, struct playlist *playlist, uint32_t *new_count, uint32_t *updated_count)
{
	struct id3_file *i3f;
	char *artist, *title, *album;
	const mad_timer_t *duration;
	uint16_t duration_secs;
	char durationbuf[8], inodebuf[16], sizebuf[16], mtimebuf[16];
	PGresult *res;
	struct playlist_node *node = NULL;
	int8_t rc;

	if(playlist && (node = playlist_find_file(playlist, file)))
	{
		uint8_t modified = 0;
		char *tmp = strdup(file);
		debug("found old record for %s", basename(tmp));
		free(tmp);

		/*
		if(node->inode != sb->st_ino)
		{
			modified = 1;
			debug("inode differs: %"PRIu32" -> %lu", node->inode, sb->st_ino);
		}
		*/
		if(node->size != sb->st_size)
		{
			modified = 1;
			debug("size differs: %"PRIu32" -> %ld", node->size, sb->st_size);
		}
		if(node->mtime != sb->st_mtime)
		{
			modified = 1;
			debug("mtime differs: %"PRIu32" -> %ld", node->mtime, sb->st_mtime);
		}

		if(!modified)
			return 0;
	}

	artist = title = album = NULL;
	if((i3f = id3_file_open(file, ID3_FILE_MODE_READONLY)))
	{
		const struct id3_tag *tag = id3_file_tag(i3f);
		artist = get_id3_entry(tag, ID3_FRAME_ARTIST);
		title = get_id3_entry(tag, ID3_FRAME_TITLE);
		album = get_id3_entry(tag, ID3_FRAME_ALBUM);
		id3_file_close(i3f);
	}

	duration = get_mp3_duration(file, sb);
	if(!duration)
	{
		log_append(LOG_WARNING, "file %s has no duration, skipping", file);
		MyFree(artist);
		MyFree(title);
		MyFree(album);
		return 0;
	}

	duration_secs = (uint16_t)round(mad_timer_count(*duration, MAD_UNITS_MILLISECONDS) / 1000.0);
	if(duration_secs < 5)
	{
		log_append(LOG_WARNING, "file %s has an extremely short duration (%"PRIu16" secs), skipping", file, duration_secs);
		MyFree(artist);
		MyFree(title);
		MyFree(album);
		return 0;
	}

	debug("%s - %s - %s [%02u:%02u]", artist, album, title, duration_secs / 60, duration_secs % 60);

	snprintf(durationbuf, sizeof(durationbuf), "%u", duration_secs);
	snprintf(inodebuf, sizeof(inodebuf), "%lu", sb->st_ino);
	snprintf(sizebuf, sizeof(sizebuf), "%ld", sb->st_size);
	snprintf(mtimebuf, sizeof(mtimebuf), "%ld", sb->st_mtime);

	if(node)
	{
		char idbuf[16];
		snprintf(idbuf, sizeof(idbuf), "%"PRIu32, node->id);
		res = pgsql_query(conn, "UPDATE \
						playlist \
					 SET \
						artist = $2, \
						album = $3, \
						title = $4, \
						duration = $5, \
						st_inode = $6, \
						st_size = $7, \
						st_mtime = $8 \
					 WHERE \
						id = $1",
				  1, stringlist_build_n(8, idbuf, artist, album, title, durationbuf, inodebuf, sizebuf, mtimebuf));
		if(updated_count)
			(*updated_count)++;
	}
	else
	{
		res = pgsql_query_bin(conn, "INSERT INTO playlist \
						(file, artist, album, title, duration, st_inode, st_size, st_mtime) \
					     VALUES \
						($1::bytea, $2, $3, $4, $5, $6, $7, $8)",
				  1, stringlist_build_n(8, file, artist, album, title, durationbuf, inodebuf, sizebuf, mtimebuf), 1);
		if(new_count)
			(*new_count)++;
	}
	rc = res ? 0 : -1;
	if(res)
		pgsql_free(res);
	MyFree(artist);
	MyFree(title);
	MyFree(album);
	return rc;
}

static const mad_timer_t *get_mp3_duration(const char *file, struct stat *sb)
{
	static mad_timer_t duration;
	int fd;
	struct stat sb_;
	void *filedata;
	struct mad_stream stream;
	struct mad_header header;

	duration = mad_timer_zero;

	if((fd = open(file, O_RDONLY)) == -1)
	{
		log_append(LOG_WARNING, "open(%s) failed: %s", file, strerror(errno));
		return NULL;
	}

	if(!sb)
	{
		sb = &sb_;
		if(fstat(fd, sb) == -1)
		{
			log_append(LOG_ERROR, "fstat() failed: %s", strerror(errno));
			return NULL;
		}
	}

	if(sb->st_size == 0)
	{
		log_append(LOG_WARNING, "file is empty");
		return NULL;
	}

	if((filedata = mmap(0, sb->st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
	{
		log_append(LOG_ERROR, "mmap() failed: %s", strerror(errno));
		return NULL;
	}

	mad_stream_init(&stream);
	mad_header_init(&header);
	mad_stream_buffer(&stream, filedata, sb->st_size);

	while(1)
	{
		if(mad_header_decode(&header, &stream) == -1)
		{
			if(MAD_RECOVERABLE(stream.error))
				continue;
			else
				break;
		}

		mad_timer_add(&duration, header.duration);
	}

	mad_header_finish(&header);
	mad_stream_finish(&stream);

	munmap(filedata, sb->st_size);
	close(fd);

	return &duration;
}

static char *get_id3_entry(const struct id3_tag *tag, const char *id)
{
	const struct id3_frame *frame;
	const id3_ucs4_t *ucs4;
	const union id3_field *field;

	if(!(frame = id3_tag_findframe(tag, id, 0)))
		return NULL;

	field = id3_frame_field(frame, 1);
	if(id3_field_getnstrings(field) < 1)
		return NULL;

	if(!(ucs4 = id3_field_getstrings(field, 0)))
		return NULL;

	if(!strcmp(id, ID3_FRAME_GENRE))
		ucs4 = id3_genre_name(ucs4);

	return (char *)id3_ucs4_utf8duplicate(ucs4);
}

static const char *make_absolute_path(const char *relative)
{
	static char absolute[PATH_MAX];
	static char old_cwd[PATH_MAX], new_cwd[PATH_MAX];
	char *relative_dup = NULL, *relative_dup2 = NULL;
	const char *filename, *ret, *relative_path;
	size_t pathlen;
	struct stat sb, sb2;
	int stat_rc;

	assert_return(strlen(relative) < PATH_MAX, relative);

	// Save current working dir
	getcwd(old_cwd, sizeof(old_cwd));

	if((stat_rc = stat(relative, &sb)) == -1)
		log_append(LOG_WARNING, "could not stat(%s): %s", relative, strerror(errno));

	if(S_ISDIR(sb.st_mode) && stat_rc == 0)
	{
		filename = NULL;
		relative_path = relative;
	}
	else
	{
		relative_dup = strdup(relative);
		relative_dup2 = strdup(relative);
		filename = basename(relative_dup);
		relative_path = dirname(relative_dup2);
	}

	if(chdir(relative_path) == -1)
	{
		log_append(LOG_WARNING, "could not chdir(%s): %s", relative_path, strerror(errno));
		MyFree(relative_dup);
		MyFree(relative_dup2);
		return relative;
	}

	getcwd(new_cwd, sizeof(new_cwd));

	// Go back to old working dir
	if(chdir(old_cwd) == -1)
		log_append(LOG_WARNING, "could not chdir(%s): %s", old_cwd, strerror(errno));

	if(filename)
	{
		snprintf(absolute, sizeof(absolute), "%s/%s", new_cwd, filename);
		ret = absolute;
	}
	else
	{
		// No filename -> we can just return the new working dir
		ret = new_cwd;
	}

	if(stat(ret, &sb2) == -1)
		log_append(LOG_WARNING, "could not stat(%s): %s", ret, strerror(errno));
	else if(stat_rc == 0 && sb.st_ino != sb2.st_ino) // check if the new path points at the same destination
	{
		log_append(LOG_ERROR, "absolute path points to different target than relative path");
		ret = relative;
	}

	MyFree(relative_dup);
	MyFree(relative_dup2);
	return ret;
}
