#ifndef GALE_CODEGEN_H
#define GALE_CODEGEN_H

#include "asm.h"
#include "sema.h"

typedef enum {
    TARGET_LINUX,   /* x86-64 Linux, raw system calls, ELF */
    TARGET_WINDOWS  /* x86-64 Windows, kernel32 imports, PE */
} Target;

/* The target of the system the compiler itself was built for. */
Target host_target(void);

typedef struct {
    char file[512];
    char msg[300];
    int line, col;
} CodegenError;

/* Generates assembly for a type-checked program: a `_start` that calls `main` and
 * exits with its result (modulo 256, on every target), followed by `main` and every function reachable from it.
 * Functions that are never called are not emitted.
 *
 * The calling convention: arguments are pushed right to left (each takes a multiple
 * of 8 bytes; structs are copied), the caller pops them, the result comes back in rax.
 * A function returning a struct takes a hidden first argument, the address to write
 * the result to.
 *
 * Returns 1 on success and stores the label of `_start` in *entry_label. On failure
 * returns 0 and fills *err. */
int codegen(Sema *sema, Asm *out, Target target, int *entry_label, CodegenError *err);

#endif /* GALE_CODEGEN_H */
