#ifndef PLAYLIST_H
#define PLAYLIST_H

struct pgsql;

enum playlist_scan_flags
{
	PL_S_TRUNCATE		= 0x01, // truncate playlist (but not blacklist) before scanning
	PL_S_TRUNCATE_ALL	= 0x02, // also truncate blacklist
	PL_S_PARSE_ALL		= 0x04, // also parse unmodified files
	PL_S_REMOVE_MISSING	= 0x08  // remove missing files, cannot be combined with the previous flags
};

enum playlist_load_flags
{
	PL_L_RANDOMIZE	= 0x01, // load playlist in random order
	PL_L_ALL	= 0x02, // load everything, including blacklisted files
	PL_L_RANDOMGENRE= 0x04  // load random genre
};

struct playlist_node
{
	uint32_t id;
	char *file;

	char *artist;
	char *album;
	char *title;

	uint16_t duration;
	uint8_t blacklist;
	uint8_t genre_id;

	uint32_t inode;
	int32_t size;
	int32_t mtime;

	struct playlist_node *prev;
	struct playlist_node *next;
};

struct playlist
{
	struct pgsql *conn;

	struct playlist_node *queue;
	struct playlist_node *queue_cur;
	struct playlist_node *head;
	struct playlist_node *tail;
	struct playlist_node *cur;
	uint32_t count;

	uint8_t load_flags;
	uint8_t genre_id;

	void (*free)(struct playlist *playlist);
	struct playlist_node* (*next)(struct playlist *playlist);
	int8_t (*blacklist_node)(struct playlist *playlist, struct playlist_node *node);
	int8_t (*blacklist_id)(struct playlist *playlist, uint32_t id);
	// Note: make_node and get_node are ONLY for enqueue. They both allocate
	// memory which cannot be free'd except by enqueuing the nodes.
	struct playlist_node* (*make_node)(struct playlist *playlist, const char *file);
	struct playlist_node* (*get_node)(struct playlist *playlist, uint32_t id);
	void (*enqueue)(struct playlist *playlist, struct playlist_node *node);
};

int8_t playlist_scan(const char *path, struct pgsql *conn, uint8_t mode, uint32_t *new_count, uint32_t *updated_count);
int8_t playlist_add_file(const char *file, struct pgsql *conn, struct stat *sb);
struct playlist *playlist_load(struct pgsql *conn, uint8_t genre_id, uint8_t flags);

#endif
