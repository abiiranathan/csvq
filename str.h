#ifndef STR_H
#define STR_H

#include <ctype.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Trims whitespace from the beginning and end of a string in-place.
 * Optimization: Uses a single pass to find the end and track trailing
 * whitespace simultaneously, avoiding the double-scan of strlen().
 */
static inline char* trim_string(char* str) {
    if (!str) return NULL;

    // 1. Trim leading whitespace
    while (isspace((unsigned char)*str)) {
        str++;
    }

    // Optimization: If string is empty or all spaces, return early
    if (*str == '\0') {
        return str;
    }

    // 2. Single-pass forward scan for trailing trim
    // 'cursor' finds the null terminator.
    // 'end' tracks the last seen non-space character.
    char* end    = str;
    char* cursor = str;

    while (*cursor) {
        if (!isspace((unsigned char)*cursor)) {
            end = cursor;
        }
        cursor++;
    }

    // 3. Terminate after the last non-space character
    *(end + 1) = '\0';

    return str;
}

#ifdef _WIN32
static inline char* strcasestr(const char* haystack, const char* needle) {
    if (needle == NULL || *needle == '\0') {
        return (char*)haystack;
    }

    const size_t needle_len   = strlen(needle);
    const size_t haystack_len = strlen(haystack);

    if (needle_len > haystack_len) {
        return NULL;
    }

    const size_t search_len = haystack_len - needle_len + 1;

    for (size_t i = 0; i < search_len; i++) {
        if (tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[0])) {
            bool match = true;
            for (size_t j = 1; j < needle_len; j++) {
                if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return (char*)(haystack + i);
            }
        }
    }

    return NULL;
}
#endif

#ifdef __cplusplus
}
#endif

#endif  // STR_H
