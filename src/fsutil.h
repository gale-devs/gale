#ifndef GALE_FSUTIL_H
#define GALE_FSUTIL_H

#include <stddef.h>

/* Reads a whole file. Returns a malloc'ed, NUL-terminated buffer, or NULL if the
 * file cannot be read. */
char *read_file(const char *path);

/* Writes a file (binary). With `executable` set, also marks it executable where the
 * system has such a notion. Returns 1 on success. */
int write_file(const char *path, const unsigned char *data, size_t len, int executable);

/* Absolute, normalized path of an existing file (malloc'ed), or NULL. */
char *path_canonical(const char *path);

/* Directory part of path (malloc'ed); "." when path has no directory. */
char *path_dir(const char *path);

#endif /* GALE_FSUTIL_H */
