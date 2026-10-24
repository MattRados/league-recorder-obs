/* jansson 2.14's jansson_config.h for MSVC x64: what its CMake build writes there from
 * cmake/jansson_config.h.cmake (MSVC has neither GCC's __atomic nor __sync built-ins). */

#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#ifdef __cplusplus
#define JSON_INLINE inline
#else
#define JSON_INLINE __inline
#endif

#define JSON_INTEGER_IS_LONG_LONG 1
#define JSON_HAVE_LOCALECONV 1
#define JSON_HAVE_ATOMIC_BUILTINS 0
#define JSON_HAVE_SYNC_BUILTINS 0
#define JSON_PARSER_MAX_DEPTH 2048

#endif
