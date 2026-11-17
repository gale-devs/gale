#ifndef GALE_COMMON_H
#define GALE_COMMON_H

#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

#endif /* GALE_COMMON_H */
