#ifndef GALE_FSUTIL_H
#define GALE_FSUTIL_H

/* Reads a whole file. Returns a malloc'ed, NUL-terminated buffer, or NULL if the
 * file cannot be read. */
char *read_file(const char *path);

/* Absolute, normalized path of an existing file (malloc'ed), or NULL. */
char *path_canonical(const char *path);

/* Directory part of path (malloc'ed); "." when path has no directory. */
char *path_dir(const char *path);

#endif /* GALE_FSUTIL_H */
