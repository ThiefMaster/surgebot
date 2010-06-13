#include "global.h"
#include "mp3.h"
#include "id3.h"

#define MPG_MD_MONO 3
#define MP3_BUFFER_SIZE 4096

struct mp3_header
{
	 unsigned int version;
	 unsigned int layer;
	 unsigned int error_protection;
	 unsigned int bitrate;
	 unsigned int samplerate;
	 unsigned int padding;
	 unsigned int extension;
	 unsigned int mode;
	 unsigned int mode_ext;
	 unsigned int copyright;
	 unsigned int original;
	 unsigned int emphasis;
	 unsigned int channels;
};

static int ices_mp3_parse(struct mp3_file *source);
static ssize_t ices_mp3_read(struct mp3_file *self, void* buf, size_t len);
static int ices_mp3_close(struct mp3_file *self);
static int mp3_fill_buffer(struct mp3_file *self, size_t len);
static void mp3_trim_file(struct mp3_file *self, struct mp3_header *header);
static int mp3_parse_frame(const unsigned char* buf, struct mp3_header *header);
static int mp3_check_vbr(struct mp3_file *source, struct mp3_header *header);
static size_t mp3_frame_length(struct mp3_header *header);

static unsigned int bitrates[2][3][15] = {
	/* MPEG-1 */
	{
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
		{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}
	},
	/* MPEG-2 LSF, MPEG-2.5 */
	{
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}
	},
};

static unsigned int samplerates[3][4] = {
	{44100, 48000, 32000, 0},
	{22050, 24000, 16000, 0},
	{11025, 8000, 8000, 0}
};

static char *mode_names[5] = {"stereo", "j-stereo", "dual-ch", "mono", "multi-ch"};
static char *layer_names[3] = {"I", "II", "III"};
static char *version_names[3] = {"MPEG-1", "MPEG-2 LSF", "MPEG-2.5"};

/* Parse mp3 file for header information; bitrate, channels, mode, sample_rate.
 */
static int ices_mp3_parse(struct mp3_file *source)
{
	struct mp3_header mh;
	size_t len, framelen;
	int rc = 0;
	int off = 0;

	if (source->data.len < 4)
		return 1;

	/* Ogg/Vorbis often contains bits that almost look like MP3 headers */
	if (!strncmp("OggS", (char *)source->data.buf, 4))
		return 1;

	/* first check for ID3v2 */
	if (!strncmp("ID3", (char *)source->data.buf, 3))
		ices_id3v2_parse(source);

	/* ensure we have at least 4 bytes in the read buffer */
	if (!source->data.buf || source->data.len - source->data.pos < 4)
		mp3_fill_buffer(source, MP3_BUFFER_SIZE);
	if (source->data.len - source->data.pos < 4)
	{
		log_append(LOG_WARNING, "Short file: %s", source->path);
		return	-1;
	}

	/* seek past garbage if necessary */
	do
	{
		len = source->data.len - source->data.pos;

		/* copy remaining bytes to front, refill buffer without malloc/free */
		if (len < 4)
		{
			unsigned char *buffer = source->data.buf;

			memcpy(buffer, buffer + source->data.pos, len);
			/* make read fetch from source instead of buffer */
			source->data.buf = NULL;
			len += source->read(source, buffer + len, source->data.len - len);
			source->data.buf = buffer;
			source->data.pos = 0;
			if (len < 4)
				break;
		}

		/* we must be able to read at least 4 bytes of header */
		while (source->data.len - source->data.pos >= 4)
		{
			/* don't bother with free bit rate MP3s - they are so rare that a parse error is more likely */
			if ((rc = mp3_parse_frame(source->data.buf + source->data.pos, &mh)) && (framelen = mp3_frame_length(&mh)))
			{
				struct mp3_header next_header;

				source->samplerate = mh.samplerate;
				source->bitrate = mh.bitrate;
				source->channels = mh.channels;

				if (mp3_check_vbr (source, &mh))
				{
					source->bitrate = 0;
					break;
				}

				/* check next frame if possible */
				if (mp3_fill_buffer(source, framelen + 4) <= 0)
					break;

				/* if we can't find the second frame, we assume the first frame was junk */
				if ((rc = mp3_parse_frame(source->data.buf + source->data.pos + framelen, &next_header)))
				{
					if (mh.version != next_header.version || mh.layer != next_header.layer || mh.samplerate != next_header.samplerate)
					{
						rc = 0;
					}
					else /* fallback VBR check if VBR tag is missing */
					{
						if (mh.bitrate != next_header.bitrate)
						{
							debug("Bitrate of first frame (%d) doesn't match second frame (%d), assuming VBR", mh.bitrate, next_header.bitrate);
							source->bitrate = 0;
						}
						break;
					}
				}
				if (!rc)
					debug("Bad frame at offset %d", (int)(source->bytes_read + source->data.pos));
			}
			source->data.pos++;
			off++;
		}
	} while (!rc);

	if (!rc)
	{
		log_append(LOG_WARNING, "Couldn't find synch");
		return -1;
	}

	if (off)
		debug("Skipped %d bytes of garbage before MP3", off);

	/* adjust file size for short frames */
	mp3_trim_file(source, &mh);

	if (source->bitrate)
		debug("%s layer %s, %d kbps, %d Hz, %s", version_names[mh.version], layer_names[mh.layer - 1], mh.bitrate, mh.samplerate, mode_names[mh.mode]);
	else
		debug("%s layer %s, VBR, %d Hz, %s", version_names[mh.version], layer_names[mh.layer - 1], mh.samplerate, mode_names[mh.mode]);
	debug("Ext: %d\tMode_Ext: %d\tCopyright: %d\tOriginal: %d", mh.extension, mh.mode_ext, mh.copyright, mh.original);
	debug("Error Protection: %d\tEmphasis: %d\tPadding: %d", mh.error_protection, mh.emphasis, mh.padding);

	return 0;
}

int ices_mp3_open(struct mp3_file *self, const char *buf, size_t len)
{
	int rc;

	if (!len)
		return -1;

	if (!(self->data.buf = (unsigned char *)malloc(len)))
	{
		log_append(LOG_ERROR, "Malloc failed in ices_mp3_open");
		return -1;
	}

	memcpy(self->data.buf, buf, len);
	self->data.len = len;
	self->data.pos = 0;

	self->read = ices_mp3_read;
	self->close = ices_mp3_close;

	ices_id3v1_parse(self);

	if((rc = ices_mp3_parse(self)))
	{
		free(self->data.buf);
		return rc;
	}

	return 0;
}

int stream_open_source(struct mp3_file *source)
{
	char buf[INPUT_BUFSIZE];
	size_t len;
	int fd;
	int rc;

	source->filesize = 0;
	source->bytes_read = 0;
	source->channels = 2;

	if((fd = open(source->path, O_RDONLY)) < 0)
	{
		log_append(LOG_WARNING, "Error opening %s: %s", source->path, strerror(errno));
		return -1;
	}

	source->fd = fd;

	if ((rc = lseek(fd, 0, SEEK_END)) >= 0)
	{
		source->filesize = rc;
		lseek(fd, 0, SEEK_SET);
	}

	if ((len = read(fd, buf, sizeof(buf))) <= 0)
	{
		log_append(LOG_WARNING, "Error reading header: %s", strerror(errno));
		close(fd);
		return -1;
	}

	if (!(rc = ices_mp3_open(source, buf, len)))
		return 0;
	if (rc < 0)
	{
		close(fd);
		return -1;
	}
	close(fd);
	return -1;
}

/* input_stream_t wrapper for fread */
static ssize_t ices_mp3_read(struct mp3_file *self, void *buf, size_t len)
{
	int remaining;
	int rlen = 0;

	if (self->data.buf)
	{
		remaining = self->data.len - self->data.pos;
		if(remaining > (int)len)
		{
			rlen = len;
			memcpy(buf, self->data.buf + self->data.pos, len);
			self->data.pos += len;
		}
		else
		{
			rlen = remaining;
			memcpy(buf, self->data.buf + self->data.pos, remaining);
			free(self->data.buf);
			self->data.buf = NULL;
		}
	}
	else
	{
		/* we don't just use EOF because we'd like to avoid the ID3 tag */
		if (self->filesize && self->filesize - self->bytes_read < len)
			len = self->filesize - self->bytes_read;
		if (len)
			rlen = read (self->fd, buf, len);
	}

	self->bytes_read += rlen;

	return rlen;
}

static int ices_mp3_close(struct mp3_file *self)
{
	MyFree(self->data.buf);
	return close(self->fd);
}

/* trim short frame from end of file if necessary */
static void mp3_trim_file(struct mp3_file *self, struct mp3_header *header)
{
	unsigned char buf[MP3_BUFFER_SIZE];
	struct mp3_header match;
	off_t cur, start, end;
	int framelen;
	int rlen, len;

	if (!self->filesize)
		return;

	cur = lseek(self->fd, 0, SEEK_CUR);
	end = self->filesize;
	while (end > cur)
	{
		start = end - sizeof(buf);
		if (start < cur)
			start = cur;

		/* load buffer */
		lseek(self->fd, start, SEEK_SET);
		for (len = 0; start + len < end; len += rlen)
		{
			if ((rlen = read(self->fd, buf + len, end - (start + len))) <= 0)
			{
				log_append(LOG_WARNING, "Error reading MP3 while trimming end");
				lseek(self->fd, cur, SEEK_SET);
				return;
			}
		}
		end = start;

		/* search buffer backwards looking for sync */
		for (len -= 4; len >= 0; len--)
		{
			if(mp3_parse_frame(buf + len, &match) && (framelen = mp3_frame_length(&match))
				&& header->version == match.version && header->layer == match.layer
				&& header->samplerate == match.samplerate
				&& (!self->bitrate || self->bitrate == match.bitrate))
			{
				if ((unsigned)(start + len + framelen) < self->filesize)
				{
					self->filesize = start + len + framelen;
					debug("Trimmed file to %d bytes", (int)self->filesize);
				}
				else if ((unsigned)(start + len + framelen) > self->filesize)
				{
					debug("Trimmed short frame (%d bytes missing) at offset %d", (int)((start + len + framelen) - self->filesize), (int)(start + len));
					self->filesize = start + len;
				}

				lseek(self->fd, cur, SEEK_SET);
				return;
			}
		}
	}
	lseek(self->fd, cur, SEEK_SET);
}

/* make sure source buffer has at least len bytes.
 * returns: 1: success, 0: EOF before len, -1: malloc error, -2: read error */
static int mp3_fill_buffer(struct mp3_file *self, size_t len)
{
	unsigned char *buffer;
	size_t buflen;
	ssize_t rlen;
	size_t needed;

	buflen = self->data.len - self->data.pos;

	if (self->data.buf && len < buflen)
		return 1;

	if (self->filesize && len > buflen + self->filesize - self->bytes_read)
		return 0;

	/* put off adjusting self->data.len until read succeeds. len indicates how much valid
	 * data is in the buffer, not how much has been allocated. We don't need to track
	 * that for free or even realloc, although in the odd case that we can't fill the
	 * extra memory we may end up reallocing when we don't strictly have to. */
	if (!self->data.buf)
	{
		needed = len;
		if (!(self->data.buf = malloc(needed)))
			return -1;
		self->data.pos = 0;
		self->data.len = 0;
	}
	else
	{
		needed = len - buflen;
		if (!(buffer = realloc(self->data.buf, self->data.len + needed)))
			return -1;
		self->data.buf = buffer;
	}

	while (needed && (rlen = read(self->fd, self->data.buf + self->data.len, needed)) > 0)
	{
		self->data.len += rlen;
		self->bytes_read += rlen;
		needed -= rlen;
	}

	if (!needed)
		return 1;

	if (!rlen)
		return 0;

	log_append(LOG_WARNING, "Error filling read buffer: %s", strerror(errno));
	return -1;
}

static int mp3_parse_frame(const unsigned char *buf, struct mp3_header *header)
{
	int bitrate_idx, samplerate_idx;

	if (((buf[0] << 4) | ((buf[1] >> 4) & 0xE)) != 0xFFE)
		return 0;

	switch ((buf[1] >> 3 & 0x3))
	{
		case 3:
			header->version = 0;
			break;
		case 2:
			header->version = 1;
			break;
		case 0:
			header->version = 2;
			break;
		default:
			return 0;
	}

	bitrate_idx = (buf[2] >> 4) & 0xF;
	samplerate_idx = (buf[2] >> 2) & 0x3;
	header->mode = (buf[3] >> 6) & 0x3;
	header->layer = 4 - ((buf[1] >> 1) & 0x3);
	header->emphasis = (buf[3]) & 0x3;

	if (bitrate_idx == 0xF || samplerate_idx == 0x3 || header->layer == 4 || header->emphasis == 2)
		return 0;

	header->error_protection = !(buf[1] & 0x1);
	if (header->version == 0)
		header->bitrate = bitrates[0][header->layer-1][bitrate_idx];
	else
		header->bitrate = bitrates[1][header->layer-1][bitrate_idx];
	header->samplerate = samplerates[header->version][samplerate_idx];
	header->padding = (buf[2] >> 1) & 0x01;
	header->extension = buf[2] & 0x01;
	header->mode_ext = (buf[3] >> 4) & 0x03;
	header->copyright = (buf[3] >> 3) & 0x01;
	header->original = (buf[3] >> 2) & 0x1;
	header->channels = (header->mode == MPG_MD_MONO) ? 1 : 2;

	return 1;
}

static int mp3_check_vbr(struct mp3_file *source, struct mp3_header *header)
{
	int offset;

	/* check for VBR tag */
	/* Tag offset varies (but FhG VBRI is always MPEG1 Layer III 160 kbps stereo) */
	if (header->version == 0)
	{
		if (header->channels == 1)
			offset = 21;
		else
			offset = 36;
	}
	else
	{
		if (header->channels == 1)
			offset = 13;
		else
			offset = 21;
	}
	/* only needed if frame length can't be calculated (free bitrate) */
	if (mp3_fill_buffer(source, offset + 4) <= 0)
	{
		log_append(LOG_WARNING, "Error trying to read VBR tag");
		return -1;
	}

	offset += source->data.pos;
	if(!strncmp("VBRI", (char *)(source->data.buf + offset), 4) || !strncmp("Xing", (char *)(source->data.buf + offset), 4))
	{
		debug("VBR tag found");
		return 1;
	}

	return 0;
}

/* Calculate the expected length of the next frame, or return 0 if we don't know how */
static size_t mp3_frame_length(struct mp3_header *header)
{
	if (!header->bitrate)
		return 0;

	if (header->layer == 1)
		return (12000 * header->bitrate / header->samplerate + header->padding) * 4;
	else if (header->layer ==3 && header->version > 0)
		return 72000 * header->bitrate / header->samplerate + header->padding;

	return 144000 * header->bitrate / header->samplerate + header->padding;
}

