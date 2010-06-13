#ifndef MP3_H
#define MP3_H

#define INPUT_BUFSIZE 4096
#define OUTPUT_BUFSIZE 32768

struct mp3_file
{
	char *path;
	int fd;
	size_t filesize;
	size_t bytes_read;
	unsigned int bitrate;
	unsigned int samplerate;
	unsigned int channels;

	struct
	{
		unsigned char *buf;
		size_t len;
		int pos;
	} data;

	ssize_t (*read)(struct mp3_file *self, void *buf, size_t len);
	int (*close)(struct mp3_file *self);
};

int stream_open_source(struct mp3_file *source);
int ices_mp3_open(struct mp3_file *self, const char *buf, size_t len);

#endif
