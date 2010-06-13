#ifndef ID3_H
#define ID3_H

struct mp3_file;
void ices_id3v1_parse(struct mp3_file *source);
void ices_id3v2_parse(struct mp3_file *source);

#endif
