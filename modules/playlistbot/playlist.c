#include "global.h"
#include "playlist.h"

#include <id3tag.h>

static void playlist_add(struct playlist *list, char *string);

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

	return (char *)id3_ucs4_latin1duplicate(ucs4);
}

static struct playlist *playlist_create()
{
	struct playlist *list = malloc(sizeof(struct playlist));
	memset(list, 0, sizeof(struct playlist));
	list->size = 16;
	list->queue_size = 4;
	list->data = calloc(list->size, sizeof(struct playlist_node *));
	list->queue_data = calloc(list->queue_size, sizeof(struct playlist_node *));
	list->next_id = 1;
	return list;
}

void playlist_free_node(struct playlist_node *node)
{
	free(node->file);
	if(node->artist)
		free(node->artist);
	if(node->title)
		free(node->title);
	free(node);
}

void playlist_free(struct playlist *list)
{
	unsigned int i;
	for(i = 0; i < list->orig_count; i++)
		playlist_free_node(list->orig_data[i]);
	free(list->data);
	if(list->orig_data)
		free(list->orig_data);
	free(list->queue_data);
	free(list);
}

struct playlist *playlist_load(const char *path, struct playlist *playlist)
{
	DIR *dir;
	struct dirent *direntry;
	char new_path[PATH_MAX];
	struct stat attr;
	size_t len;
	unsigned int recursive = 1;

	if(!playlist)
	{
		playlist = playlist_create();
		recursive = 0;
		debug("Loading playlist");
	}

	if(!(dir = opendir(path)))
		return playlist;

	strncpy(new_path, path, sizeof(new_path));
	len = strlen(new_path);
	if(new_path[len - 1] == '/')
		new_path[--len] = '\0';
	while((direntry = readdir(dir)))
	{
		if(!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
			continue;

		snprintf(new_path + len, sizeof(new_path) - len, "/%s", direntry->d_name);
		stat(new_path, &attr);
		if(attr.st_mode & S_IFDIR)
			playlist = playlist_load(new_path, playlist);
		else if(attr.st_mode & (S_IFREG | S_IFLNK))
		{
			char *ext = strrchr(direntry->d_name, '.');
			if(!ext || strcasecmp(ext, ".mp3"))
				continue;
			playlist_add(playlist, new_path);
		}
	}

	closedir(dir);
	if(!recursive)
		debug("Playlist loaded");
	return playlist;
}

void playlist_shuffle(struct playlist *playlist)
{
	static time_t seed = 0;
	struct playlist_node *tmp;
	int rnd, last;

	if(playlist->frozen)
		return;

	if(!seed)
	{
		seed = time(NULL);
		srand(seed);
	}

	for(last = playlist->count; last > 1; last--)
	{
		rnd = rand() % last;
		tmp = playlist->data[rnd];
		playlist->data[rnd] = playlist->data[last - 1];
		playlist->data[last - 1] = tmp;
	}
}

static void playlist_add(struct playlist *list, char *file)
{
	struct id3_file *i3f;
	struct playlist_node *node;

	if(list->frozen)
		return;

	if(list->count == list->size) // list is full, we need to allocate more memory
	{
		list->size <<= 1; // double size
		list->data = realloc(list->data, list->size * sizeof(struct playlist_node *));
	}

	node = malloc(sizeof(struct playlist_node));
	memset(node, 0, sizeof(struct playlist_node));
	node->id = list->next_id++;
	node->file = strdup(file);

	i3f = id3_file_open(file, ID3_FILE_MODE_READONLY);
	if(i3f)
	{
		const struct id3_tag *tag = id3_file_tag(i3f);
		node->artist = get_id3_entry(tag, ID3_FRAME_ARTIST);
		node->title = get_id3_entry(tag, ID3_FRAME_TITLE);
		id3_file_close(i3f);
	}

	list->data[list->count++] = node;
}

struct playlist_node **playlist_search(struct playlist *list, const char *artist, const char *title, unsigned int *count)
{
	struct playlist_node **results = calloc(1, sizeof(struct playlist_node *));
	unsigned int num = 0, size = 1;
	for(unsigned int i = 0; i < list->count; i++)
	{
		struct playlist_node *node = list->data[i];
		if(artist && match(artist, node->artist))
			continue;
		if(title && match(title, node->title))
			continue;

		size++;
		results = realloc(results, size * sizeof(struct playlist_node *));
		results[num++] = node;
	}

	if(count)
		*count = num;
	results[num] = NULL;
	return results;
}

void playlist_enqueue(struct playlist *list, unsigned int id)
{
	struct playlist_node *node = NULL;
	unsigned int pos;
	for(unsigned int i = 0; i < list->count; i++)
	{
		if(list->data[i]->id == id)
		{
			pos = i;
			node = list->data[i];
			break;
		}
	}

	if(!node)
		return;

	if(list->queue_count == list->queue_size) // list is full, we need to allocate more memory
	{
		list->queue_size++;
		list->queue_data = realloc(list->queue_data, list->queue_size * sizeof(struct playlist_node *));
	}

	list->queue_data[list->queue_count++] = node;
	memmove(list->data + pos, list->data + pos + 1, (list->count - pos - 1) * sizeof(struct playlist_node *));
	list->count--;
}

void playlist_enqueue_file(struct playlist *list, const char *file)
{
	struct id3_file *i3f;
	struct playlist_node *node;

	node = malloc(sizeof(struct playlist_node));
	memset(node, 0, sizeof(struct playlist_node));
	node->temp = 1;
	node->id = 0;
	node->file = strdup(file);

	i3f = id3_file_open(file, ID3_FILE_MODE_READONLY);
	if(i3f)
	{
		const struct id3_tag *tag = id3_file_tag(i3f);
		node->artist = get_id3_entry(tag, ID3_FRAME_ARTIST);
		node->title = get_id3_entry(tag, ID3_FRAME_TITLE);
		id3_file_close(i3f);
	}

	if(list->queue_count == list->queue_size) // list is full, we need to allocate more memory
	{
		list->queue_size++;
		list->queue_data = realloc(list->queue_data, list->queue_size * sizeof(struct playlist_node *));
	}

	list->queue_data[list->queue_count++] = node;
}

void playlist_freeze(struct playlist *list)
{
	if(list->frozen)
		return;

	list->frozen = 1;
	list->orig_count = list->count;
	list->orig_data  = calloc(list->count, sizeof(struct playlist_node *));
	memcpy(list->orig_data, list->data, list->count * sizeof(struct playlist_node *));
}

void playlist_unfreeze(struct playlist *list)
{
	if(!list->frozen)
		return;

	list->frozen = 0;
	list->count = list->orig_count;
	list->orig_count = 0;
	free(list->data);
	list->data = list->orig_data;
	list->orig_data = NULL;
}

struct playlist_node *playlist_next(struct playlist *list)
{
	struct playlist_node *node = NULL;

	if(!list->frozen)
		return NULL;

	if(list->queue_count)
	{
		node = list->queue_data[0];
		list->queue_count--;
		memmove(list->queue_data, list->queue_data + 1, list->queue_count * sizeof(struct playlist_node *));
	}
	else if(list->count)
	{
		node = list->data[0];
		list->count--;
		memmove(list->data, list->data + 1, list->count * sizeof(struct playlist_node *));
	}

	if(!list->count)
	{
		list->count = list->orig_count;
		memcpy(list->data, list->orig_data, list->count * sizeof(struct playlist_node *));
	}

	return node;
}
