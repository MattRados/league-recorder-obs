/* jansson 2.14's jansson_private_config.h for MSVC x64: what its CMake build finds there
 * (cmake/jansson_private_config.h.cmake), with Windows' CryptoAPI seeding the hash. */

#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDINT_H 1

#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1

#define HAVE_INT32_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT8_T 1

#define ssize_t SSIZE_T

#define USE_WINDOWS_CRYPTOAPI 1

#define INITIAL_HASHTABLE_ORDER 3
