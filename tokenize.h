#ifndef TOKENIZE_H
#define TOKENIZE_H

int tokenize(char *str, char **vec, int vec_size, char token, unsigned char allow_empty);
int itokenize(char *str, char **vec, int vec_size, char token, char ltoken);

char *untokenize(int num_items, char **vec, const char *sep);

#endif
