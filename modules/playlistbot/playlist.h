#ifndef PLAYLIST_H
#define PLAYLIST_H

struct playlist_node
{
	unsigned int id;
	char *file;
	char *artist;
	char *title;
	unsigned int temp : 1; // delete after playing
};

struct playlist
{
	unsigned int count;
	unsigned int size;
	unsigned int orig_count;
	unsigned int queue_count;
	unsigned int queue_size;
	unsigned int next_id;

	struct playlist_node **data;
	struct playlist_node **orig_data;
	struct playlist_node **queue_data;

	unsigned char frozen;
};

struct playlist *playlist_load(const char *path, struct playlist *playlist);
void playlist_shuffle(struct playlist *playlist);
void playlist_free(struct playlist *list);
void playlist_free_node(struct playlist_node *node);

struct playlist_node **playlist_search(struct playlist *list, const char *artist, const char *title, unsigned int *count);
struct playlist_node *playlist_next(struct playlist *list);
void playlist_freeze(struct playlist *list);
void playlist_unfreeze(struct playlist *list);
void playlist_enqueue(struct playlist *list, unsigned int id);
void playlist_enqueue_file(struct playlist *list, const char *file);

#endif
