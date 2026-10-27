/* Minimal SDL 1.2 config for cross-compiling against the OnionOS (glibc, ARM) build of SDL. */
#ifndef _SDL_config_h
#define _SDL_config_h
#include "SDL_platform.h"
#define SDL_HAS_64BIT_TYPE 1
#define SDL_BYTEORDER 1234
#define HAVE_LIBC 1
#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDARG_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#endif
