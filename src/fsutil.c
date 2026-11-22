#if !defined(_WIN32)
/* realpath() needs the XSI extensions on glibc; without them it is not declared and
 * the call would silently return an int (truncating the pointer). */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int write_file(const char *path, const unsigned char *data, size_t len, int executable) {
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f) {
        return 0;
    }
    ok = len == 0 || fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = 0;
    }
#if !defined(_WIN32)
    if (ok && executable) {
        ok = chmod(path, 0755) == 0;
    }
#else
    (void)executable;
#endif
    return ok;
}

char *path_canonical(const char *path) {
#if defined(_WIN32)
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

char *path_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    size_t n;
    char *out;

    if (back && (!slash || back > slash)) {
        slash = back;
    }
    if (!slash) {
        out = malloc(2);
        if (out) {
            strcpy(out, ".");
        }
        return out;
    }
    n = (size_t)(slash - path);
    if (n == 0) {
        n = 1; /* "/file" -> "/" */
    }
    out = malloc(n + 1);
    if (out) {
        memcpy(out, path, n);
        out[n] = '\0';
    }
    return out;
}
