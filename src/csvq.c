// ===============================================================================
// Command-line tool to pretty-print a CSV table.
// Uses a fast algorithm to compute longest column and longest field per column
// to print ascii table.
// Can export data as Markdown, CSV, Table or JSON
//
// Date: 07 December 2025
// Author: Dr. Abiira Nathan
// Refactored: February 2026 - Improved modularity and robustness
// =================================================================================

#include <ctype.h>               // for isspace
#include <errno.h>               // for errno
#include <solidc/arena.h>        // Arena allocator
#include <solidc/csvparser.h>    // CSV parsing functions
#include <solidc/flags.h>        // Command-line parser
#include <solidc/map.h>          // Hash map for unique/group-by
#include <solidc/prettytable.h>  // Table printer
#include <solidc/str.h>          // string utilities
#include <stdbool.h>             // for bool
#include <stdint.h>              // for SIZE_MAX
#include <stdio.h>               // for fprintf, printf, stderr
#include <stdlib.h>              // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free, calloc
#include <string.h>              // for strlen, strcasestr, strcmp, strdup
#include <strings.h>             // for strcasecmp
#ifndef _WIN32
#include <unistd.h>  // for close, mkstemp
#endif
#include "../include/where-parser.h"

// =============================================================================
// CONSTANTS AND CONFIGURATION
// =============================================================================

/** Minimum column width for aesthetics. */
#define MIN_COLUMN_WIDTH 3

/** Maximum number of sort keys accepted by --sort. */
#define MAX_SORT_KEYS 8

/** Maximum number of aggregate expressions accepted by --aggregate. */
#define MAX_AGGREGATES 16

/** Maximum number of columns accepted by --group-by. */
#define MAX_GROUP_COLUMNS 8

/** Maximum number of columns we support hiding. */
#define MAX_HIDDEN_COLUMNS 64

/** Maximum number of columns we support selecting/reordering. */
#define MAX_SELECTED_COLUMNS 64

/** Version reported by --version. */
#define CSVQ_VERSION "0.2.0"

/** Filename used to request input from standard input. */
#define STDIN_FILENAME "-"

#define ARENA_ALLOC_ARRAY(arena, type, count) \
    ((type*)arena_alloc_array_zero(arena, sizeof(type), ARENA_DEFAULT_ALIGN, (count)))

/** ANSI color codes for column coloring. */
static const char* COLUMN_COLORS[] = {
    "\033[36m",  // Cyan
    "\033[33m",  // Yellow
    "\033[35m",  // Magenta
    "\033[32m",  // Green
    "\033[34m",  // Blue
    "\033[91m",  // Bright Red
    "\033[92m",  // Bright Green
    "\033[93m",  // Bright Yellow
    "\033[94m",  // Bright Blue
    "\033[95m",  // Bright Magenta
    "\033[96m",  // Bright Cyan
    "\033[31m",  // Red
};

/** Number of available colors. */
#define NUM_COLORS (sizeof(COLUMN_COLORS) / sizeof(COLUMN_COLORS[0]))

/** ANSI reset code. */
#define COLOR_RESET "\033[0m"

// =============================================================================
// TYPE DEFINITIONS
// =============================================================================

/** Output format types. */
typedef enum {
    OUTPUT_TABLE,     // ASCII table (default)
    OUTPUT_CSV,       // CSV format
    OUTPUT_TSV,       // Tab-separated values
    OUTPUT_JSON,      // JSON array of objects
    OUTPUT_MARKDOWN,  // Markdown table
    OUTPUT_HTML,      // HTML table
    OUTPUT_EXCEL,     // XML Spreadsheet 2003
} OutputFormat;

/** Column selection/reordering. */
typedef struct {
    size_t indices[MAX_SELECTED_COLUMNS];  // Column indices in desired order
    size_t count;                          // Number of selected columns
} ColumnSelection;

/** One sort key: a column plus its direction. */
typedef struct {
    size_t col_idx;
    bool desc;
} SortKey;

/** Context for the qsort comparison function. */
typedef struct {
    SortKey keys[MAX_SORT_KEYS];
    size_t count;
    bool active;
} SortContext;

/** Aggregate functions supported by --aggregate. */
typedef enum {
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX,
} AggregateFunc;

/** One parsed aggregate expression, e.g. sum(amount). */
typedef struct {
    AggregateFunc func;
    char* label;        // Display label, e.g. "sum(amount)"
    char* column_name;  // Column reference (NULL for count)
    size_t column_idx;  // Resolved column index
} AggregateSpec;

/** Parsed aggregate expressions. */
typedef struct {
    AggregateSpec specs[MAX_AGGREGATES];
    size_t count;
} AggregateConfig;

/** Running state for one aggregate expression in one group. */
typedef struct {
    size_t numeric_count;
    double sum;
    double min;
    double max;
    bool has_numeric;
} AggregateState;

/** One group produced by --group-by. */
typedef struct {
    const char** key_values;  // Values of the group columns (arena strings)
    size_t row_count;
    AggregateState* states;  // One state per aggregate expression
} AggregateGroup;

/** Context for table rendering callbacks. */
typedef struct {
    Row** rows;
    size_t* col_mapping;
    size_t* widths;  // Pre-computed widths for manual padding when using colors
    bool use_colors;
    Arena* arena;       // For allocating colored strings
    const Row* header;  // Original header row for get_header
} TableContext;

/** Configuration for printing operations. */
typedef struct {
    bool has_header;
    OutputFormat format;
    bool use_colors;
    bool use_bgcolor;
    const char* filter_pattern;
    WhereFilter* where;
    const ColumnSelection* selection;
    bool unique;
    const char* unique_by;
    size_t limit;
    size_t offset;
} PrintConfig;

/** Numeric descriptive statistics for one column. */
typedef struct {
    size_t numeric_count;
    size_t missing_count;
    size_t non_numeric_count;
    double sum;
    double min;
    double max;
    bool has_numeric;
} ColumnStats;

/** Context for describe-mode table rendering callbacks. */
typedef struct {
    const char** headers;
    const char** cells;  // Flattened row-major: row * num_cols + col
    size_t* widths;
    size_t num_rows;
    size_t num_cols;
    bool use_colors;
    Arena* arena;
} DescribeTableContext;

// =============================================================================
// GLOBAL STATE
// =============================================================================

/**
 * Simple bitset for tracking hidden columns.
 * Much more efficient than dynamic arrays for up to 64 columns.
 */
static unsigned long hidden_columns_mask = 0;

/** Context for the qsort comparison function. */
static SortContext sort_ctx = {.count = 0, .active = false};

// Forward declarations for helpers defined later in this file.
static int build_column_mapping(Arena* arena, size_t original_col_count,
                                const ColumnSelection* selection, size_t** col_mapping);
static size_t filter_rows(Arena* arena, Row** rows, size_t row_count, bool has_header,
                          const char* filter_pattern, WhereFilter* where, Row*** filtered_rows);
static bool row_passes_filters(const Row* row, const char* filter_pattern, WhereFilter* where);
static void print_describe_pretty_table(const char** headers, const char** cells, size_t num_rows,
                                        size_t num_cols, bool use_colors, Arena* arena);
static size_t apply_unique(Arena* arena, Row** rows, size_t row_count, const Row* header,
                           bool unique_all, const char* unique_by, Row*** out_rows);
static bool parse_numeric_field(const char* field, double* value);
static void trimmed_bounds(const char* s, const char** start, size_t* len);
static char* arena_trimmed_strdup(Arena* arena, const char* s);
static void print_table(Row** rows, size_t row_count, const PrintConfig* config);

// =============================================================================
// COLUMN VISIBILITY MANAGEMENT
// =============================================================================

/**
 * Marks a column as hidden.
 * @param index Column index to hide (0-based).
 */
static inline void hide_column(size_t index) {
    if (index < MAX_HIDDEN_COLUMNS) {
        hidden_columns_mask |= (1UL << index);
    }
}

/**
 * Checks if a column is hidden.
 * @param index Column index to check.
 * @return true if the column is hidden, false otherwise.
 */
static inline bool is_column_hidden(size_t index) {
    return index < MAX_HIDDEN_COLUMNS && (hidden_columns_mask & (1UL << index));
}

/**
 * Resets the hidden columns mask.
 */
static inline void reset_hidden_columns(void) { hidden_columns_mask = 0; }

/**
 * Parses a comma-separated list of column indices and marks them as hidden.
 * @param columns_str String like "0,2,5" or "1,3".
 * @return Number of columns successfully parsed, or -1 on error.
 */
static int parse_hidden_columns(const char* columns_str) {
    if (columns_str == NULL || columns_str[0] == '\0') {
        return 0;
    }

    char* str_copy = strdup(columns_str);
    if (str_copy == NULL) {
        return -1;
    }

    int count = 0;
    char* saveptr;
    char* token = strtok_r(str_copy, ",", &saveptr);

    while (token != NULL) {
        // Trim leading whitespace
        while (isspace((unsigned char)*token)) {
            token++;
        }

        // Parse as integer
        char* endptr;
        long index = strtol(token, &endptr, 10);

        // Skip trailing whitespace before checking for validity
        while (isspace((unsigned char)*endptr)) {
            endptr++;
        }

        if (*endptr != '\0' || index < 0 || index >= MAX_HIDDEN_COLUMNS) {
            fprintf(stderr, "Warning: Invalid column index '%s', skipping\n", token);
        } else {
            hide_column((size_t)index);
            count++;
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(str_copy);
    return count;
}

// =============================================================================
// COLUMN LOOKUP AND SELECTION
// =============================================================================

/**
 * Finds a column index by name in the header row.
 * @param header The header row.
 * @param name Column name to search for (case-insensitive).
 * @return Column index, or -1 if not found.
 */
ssize_t find_column_by_name(const Row* header, const char* name) {
    if (header == NULL || name == NULL) {
        return -1;
    }

    size_t name_len = strlen(name);

    for (size_t i = 0; i < header->count; i++) {
        const char* h_start = header->fields[i];
        if (h_start == NULL) {
            continue;
        }

        // Trim leading whitespace
        while (isspace((unsigned char)*h_start)) {
            h_start++;
        }

        // Calculate length excluding trailing whitespace
        size_t h_len = strlen(h_start);
        while (h_len > 0 && isspace((unsigned char)h_start[h_len - 1])) {
            h_len--;
        }

        // Use case-insensitive comparison on the trimmed segment
        if (h_len == name_len && strncasecmp(h_start, name, h_len) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

/**
 * Resolves a column reference (name or zero-based index) to an index.
 *
 * @param arena Arena used for the temporary trimmed name.
 * @param header Header row, or NULL when only numeric indices are available.
 * @param ref Column name or numeric index.
 * @param out Resolved column index.
 * @return true on success, false if the reference cannot be resolved.
 */
static bool resolve_column_ref(Arena* arena, const Row* header, const char* ref, size_t* out) {
    if (arena == NULL || ref == NULL || out == NULL) {
        return false;
    }

    const char* start = NULL;
    size_t len = 0;
    trimmed_bounds(ref, &start, &len);
    if (len == 0) {
        return false;
    }

    char* name = arena_strdupn(arena, start, len);
    if (name == NULL) {
        return false;
    }

    char* endptr = NULL;
    long index = strtol(name, &endptr, 10);
    if (*name != '\0' && endptr != NULL && *endptr == '\0' && index >= 0) {
        *out = (size_t)index;
        return true;
    }

    if (header != NULL) {
        ssize_t found = find_column_by_name(header, name);
        if (found >= 0) {
            *out = (size_t)found;
            return true;
        }
    }

    return false;
}

/**
 * Parses a column selection string (e.g., "name,age,email" or "0,2,1").
 * @param select_str The selection string.
 * @param header The header row (needed to resolve names to indices).
 * @param selection Output ColumnSelection structure.
 * @return true on success, false on error.
 */
static bool parse_column_selection(const char* select_str, const Row* header,
                                   ColumnSelection* selection) {
    if (select_str == NULL || select_str[0] == '\0' || selection == NULL) {
        return false;
    }

    selection->count = 0;

    char* str_copy = strdup(select_str);
    if (str_copy == NULL) {
        return false;
    }

    char* saveptr;
    char* token = strtok_r(str_copy, ",", &saveptr);

    while (token != NULL && selection->count < (size_t)MAX_SELECTED_COLUMNS) {
        // Trim whitespace
        while (isspace((unsigned char)*token)) {
            token++;
        }
        char* end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end)) {
            *end-- = '\0';
        }

        // Try parsing as integer first
        char* endptr;
        long index = strtol(token, &endptr, 10);

        if (*endptr == '\0' && index >= 0) {
            // It's a numeric index
            selection->indices[selection->count++] = (size_t)index;
        } else if (header != NULL) {
            // Try resolving as column name
            ssize_t col_idx = find_column_by_name(header, token);
            if (col_idx >= 0) {
                selection->indices[selection->count++] = (size_t)col_idx;
            } else {
                fprintf(stderr, "Warning: Column '%s' not found, skipping\n", token);
            }
        } else {
            fprintf(stderr, "Warning: Cannot resolve column name '%s' without header\n", token);
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(str_copy);
    return selection->count > 0;
}

// =============================================================================
// ROW FILTERING
// =============================================================================

/**
 * Checks if a row matches the given filter pattern (case-insensitive substring).
 * @param row The row to check.
 * @param pattern The pattern to search for.
 * @return true if the row contains the pattern in any field, false otherwise.
 */
static bool row_matches_filter(const Row* row, const char* pattern) {
    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }

    for (size_t i = 0; i < row->count; i++) {
        if (row->fields[i] != NULL && strcasestr(row->fields[i], pattern) != NULL) {
            return true;
        }
    }
    return false;
}

/**
 * Checks whether a string is blank (NULL, empty, or only whitespace).
 */
static bool is_blank_field(const char* s) {
    if (s == NULL) {
        return true;
    }

    while (*s != '\0') {
        if (!isspace((unsigned char)*s)) {
            return false;
        }
        s++;
    }

    return true;
}

/**
 * Parses a field as a complete floating-point number.
 * Leading/trailing whitespace is allowed, but trailing non-whitespace is not.
 *
 * @param field Field value (may be NULL).
 * @param value Output value on success.
 * @return true when the whole field is numeric.
 */
static bool parse_numeric_field(const char* field, double* value) {
    if (field == NULL || value == NULL) {
        return false;
    }

    errno = 0;
    char* endptr = NULL;
    double parsed = strtod(field, &endptr);

    // Reject blank/whitespace-only fields: strtod performs no conversion and
    // would otherwise report them as the number zero.
    if (endptr == field) {
        return false;
    }

    while (endptr != NULL && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    if (endptr == NULL || *endptr != '\0' || errno != 0) {
        return false;
    }

    *value = parsed;
    return true;
}

/**
 * Computes the trimmed bounds of a string without modifying it.
 *
 * @param s Input string (may be NULL).
 * @param start Output pointer to the first non-space character.
 * @param len Output length after trimming trailing whitespace.
 */
static void trimmed_bounds(const char* s, const char** start, size_t* len) {
    if (s == NULL) {
        *start = "";
        *len = 0;
        return;
    }

    const char* begin = s;
    while (isspace((unsigned char)*begin)) {
        begin++;
    }

    size_t end = strlen(begin);
    while (end > 0 && isspace((unsigned char)begin[end - 1])) {
        end--;
    }

    *start = begin;
    *len = end;
}

/**
 * Returns a trimmed copy of a field allocated in the arena.
 */
static char* arena_trimmed_strdup(Arena* arena, const char* s) {
    const char* start = NULL;
    size_t len = 0;
    trimmed_bounds(s, &start, &len);
    return arena_strdupn(arena, start, len);
}

/**
 * Parses a non-negative integer command-line option.
 */
static bool parse_non_negative_size(const char* value, const char* option_name, size_t* out) {
    if (value == NULL || out == NULL) {
        return false;
    }

    const char* p = value;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '-') {
        fprintf(stderr, "Error: --%s must be non-negative (got '%s')\n", option_name, value);
        return false;
    }

    errno = 0;
    char* endptr = NULL;
    unsigned long long parsed = strtoull(value, &endptr, 10);

    while (endptr != NULL && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    if (value[0] == '\0' || endptr == NULL || *endptr != '\0' || errno != 0 || parsed > SIZE_MAX) {
        fprintf(stderr, "Error: Invalid value for --%s: '%s'\n", option_name, value);
        return false;
    }

    *out = (size_t)parsed;
    return true;
}

/**
 * Returns a paginated window over a filtered row set.
 */
static void compute_row_window(size_t filtered_count, size_t offset, size_t limit, size_t* start,
                               size_t* count) {
    size_t window_start = (offset < filtered_count) ? offset : filtered_count;
    size_t remaining = filtered_count - window_start;
    size_t window_count = remaining;

    if (limit != SIZE_MAX && limit < window_count) {
        window_count = limit;
    }

    *start = window_start;
    *count = window_count;
}

/**
 * Prints numeric descriptive statistics for visible columns.
 */
static void print_describe_stats(Row** rows, size_t row_count, bool has_header,
                                 const char* filter_pattern, WhereFilter* where,
                                 const ColumnSelection* selection, bool unique,
                                 const char* unique_by, bool use_colors) {
    if (row_count == 0 || rows[0]->count == 0) {
        fprintf(stderr, "Error: No data to describe\n");
        return;
    }

    Arena* arena = arena_create(0);
    if (!arena) {
        fprintf(stderr, "Error: Failed to initialize analysis arena\n");
        return;
    }

    if (where != NULL && has_header) {
        resolve_ast_indices(where->root, rows[0]);
    }

    size_t* col_mapping = NULL;
    int visible_cols = build_column_mapping(arena, rows[0]->count, selection, &col_mapping);
    if (visible_cols < 0 || col_mapping == NULL) {
        fprintf(stderr, "Error: Failed to build column mapping for describe\n");
        arena_destroy(arena);
        return;
    }

    Row** filtered_rows = NULL;
    size_t filtered_count =
        filter_rows(arena, rows, row_count, has_header, filter_pattern, where, &filtered_rows);

    if (unique || unique_by != NULL) {
        Row** unique_rows = NULL;
        const Row* unique_header = has_header ? rows[0] : NULL;
        filtered_count = apply_unique(arena, filtered_rows, filtered_count, unique_header, unique,
                                      unique_by, &unique_rows);
        filtered_rows = unique_rows;
    }

    const size_t describe_cols = 7;
    const char* describe_headers[] = {"Column", "Numeric", "Missing", "NonNumeric",
                                      "Min",    "Max",     "Mean"};
    const size_t describe_rows = (size_t)visible_cols;

    const char** describe_cells =
        ARENA_ALLOC_ARRAY(arena, const char*, describe_rows* describe_cols);
    if (describe_cells == NULL) {
        fprintf(stderr, "Error: Failed to allocate describe table\n");
        arena_destroy(arena);
        return;
    }

    for (int i = 0; i < visible_cols; i++) {
        size_t col = col_mapping[i];

        ColumnStats stats = {0};

        for (size_t r = 0; r < filtered_count; r++) {
            Row* row = filtered_rows[r];
            const char* field =
                (col < row->count && row->fields[col] != NULL) ? row->fields[col] : NULL;

            if (is_blank_field(field)) {
                stats.missing_count++;
                continue;
            }

            double value = 0.0;
            if (parse_numeric_field(field, &value)) {
                if (!stats.has_numeric) {
                    stats.min = value;
                    stats.max = value;
                    stats.has_numeric = true;
                } else {
                    if (value < stats.min) stats.min = value;
                    if (value > stats.max) stats.max = value;
                }

                stats.sum += value;
                stats.numeric_count++;
            } else {
                stats.non_numeric_count++;
            }
        }

        char col_name_buf[32] = {0};
        const char* col_name = col_name_buf;

        if (has_header && col < rows[0]->count && rows[0]->fields[col] != NULL) {
            col_name = rows[0]->fields[col];
        } else {
            snprintf(col_name_buf, sizeof(col_name_buf), "col_%zu", col);
            // Store the generated name in the arena: col_name_buf is reused by
            // the next iteration and cannot be referenced from describe_cells.
            col_name = arena_strdup(arena, col_name_buf);
            if (col_name == NULL) {
                col_name = "-";
            }
        }

        size_t base = ((size_t)i) * describe_cols;
        describe_cells[base + 0] = col_name;

        char numeric_buf[32];
        char missing_buf[32];
        char non_numeric_buf[32];
        snprintf(numeric_buf, sizeof(numeric_buf), "%zu", stats.numeric_count);
        snprintf(missing_buf, sizeof(missing_buf), "%zu", stats.missing_count);
        snprintf(non_numeric_buf, sizeof(non_numeric_buf), "%zu", stats.non_numeric_count);

        describe_cells[base + 1] = arena_strdup(arena, numeric_buf);
        describe_cells[base + 2] = arena_strdup(arena, missing_buf);
        describe_cells[base + 3] = arena_strdup(arena, non_numeric_buf);

        if (stats.numeric_count > 0) {
            double mean = stats.sum / (double)stats.numeric_count;
            char min_buf[64];
            char max_buf[64];
            char mean_buf[64];

            snprintf(min_buf, sizeof(min_buf), "%.6g", stats.min);
            snprintf(max_buf, sizeof(max_buf), "%.6g", stats.max);
            snprintf(mean_buf, sizeof(mean_buf), "%.6g", mean);

            describe_cells[base + 4] = arena_strdup(arena, min_buf);
            describe_cells[base + 5] = arena_strdup(arena, max_buf);
            describe_cells[base + 6] = arena_strdup(arena, mean_buf);
        } else {
            describe_cells[base + 4] = "-";
            describe_cells[base + 5] = "-";
            describe_cells[base + 6] = "-";
        }
    }

    print_describe_pretty_table(describe_headers, describe_cells, describe_rows, describe_cols,
                                use_colors, arena);

    fprintf(stderr, "Describe summary computed on %zu filtered rows\n", filtered_count);
    arena_destroy(arena);
}

/**
 * Applies all filters to a row.
 * @param row The row to check.
 * @param filter_pattern Optional substring filter.
 * @param where Optional WHERE clause filter.
 * @return true if the row passes all filters.
 */
static bool row_passes_filters(const Row* row, const char* filter_pattern, WhereFilter* where) {
    if (!row_matches_filter(row, filter_pattern)) {
        return false;
    }

    if (where != NULL && !evaluate_where_filter(row, where)) {
        return false;
    }

    return true;
}

// =============================================================================
// SORTING
// =============================================================================

/**
 * Comparator function for qsort.
 * Compares each sort key in order, trying numeric comparison first and
 * falling back to case-insensitive string comparison.
 */
static int compare_rows(const void* a, const void* b) {
    const Row* r1 = *(const Row**)a;
    const Row* r2 = *(const Row**)b;

    for (size_t k = 0; k < sort_ctx.count; k++) {
        size_t col = sort_ctx.keys[k].col_idx;

        const char* val1 =
            (col < r1->count && r1->fields[col] != NULL) ? r1->fields[col] : "";
        const char* val2 =
            (col < r2->count && r2->fields[col] != NULL) ? r2->fields[col] : "";

        int result = 0;

        char *end1, *end2;
        double d1 = strtod(val1, &end1);
        double d2 = strtod(val2, &end2);

        if (*val1 != '\0' && *val2 != '\0' && *end1 == '\0' && *end2 == '\0') {
            if (d1 < d2)
                result = -1;
            else if (d1 > d2)
                result = 1;
        } else {
            result = strcasecmp(val1, val2);
        }

        if (result != 0) {
            return sort_ctx.keys[k].desc ? -result : result;
        }
    }

    return 0;
}

/**
 * Sorts rows by one or more comma-separated columns.
 *
 * Each key may be prefixed with '-' for descending or '+' for ascending.
 * Without a prefix, the global --desc setting applies.
 *
 * @param arena Arena for temporary column names.
 * @param rows Array of row pointers.
 * @param count Total number of rows.
 * @param has_header Whether the first row is a header.
 * @param sort_col Comma-separated column names or indices.
 * @param sort_desc Default direction for keys without an explicit prefix.
 * @return true on success, false if no key could be resolved.
 */
static bool sort_rows(Arena* arena, Row** rows, size_t count, bool has_header, const char* sort_col,
                      bool sort_desc) {
    if (sort_col == NULL || count == 0) {
        return false;
    }

    char* copy = strdup(sort_col);
    if (copy == NULL) {
        return false;
    }

    sort_ctx.count = 0;

    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);
    while (token != NULL && sort_ctx.count < MAX_SORT_KEYS) {
        const char* start = NULL;
        size_t len = 0;
        trimmed_bounds(token, &start, &len);

        if (len == 0) {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        bool desc = sort_desc;
        if (start[0] == '-') {
            desc = true;
            start++;
            len--;
        } else if (start[0] == '+') {
            desc = false;
            start++;
            len--;
        }

        char* ref = arena_strdupn(arena, start, len);
        size_t col_idx = 0;
        if (ref != NULL && resolve_column_ref(arena, has_header ? rows[0] : NULL, ref, &col_idx)) {
            sort_ctx.keys[sort_ctx.count].col_idx = col_idx;
            sort_ctx.keys[sort_ctx.count].desc = desc;
            sort_ctx.count++;
        } else {
            fprintf(stderr, "Warning: Could not resolve sort column '%s'. Skipping.\n", token);
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);

    if (sort_ctx.count == 0) {
        fprintf(stderr, "Warning: No valid sort columns. Sorting skipped.\n");
        return false;
    }

    sort_ctx.active = true;

    size_t sort_start_offset = has_header ? 1 : 0;
    size_t sort_count = (count > sort_start_offset) ? count - sort_start_offset : 0;

    if (sort_count > 1) {
        qsort(rows + sort_start_offset, sort_count, sizeof(Row*), compare_rows);
    }

    return true;
}

// =============================================================================
// STRING ESCAPING AND SANITIZATION
// =============================================================================

/**
 * Trims whitespace and escapes a string for JSON output.
 * @param arena Scratch arena for allocation.
 * @param str The string to process.
 * @return Allocated string with whitespace trimmed and special chars escaped.
 */
static char* trim_and_escape_json(Arena* arena, const char* s) {
    if (s == NULL) {
        return arena_strdup(arena, "");
    }

    // Copy before trimming: callers may pass string literals or the same
    // row field multiple times, and str_trim() writes to the buffer.
    char* str = arena_strdup(arena, s);
    if (str == NULL) {
        return NULL;
    }
    str_trim(str);

    size_t len = strlen(str);
    char* escaped = arena_alloc(arena, len * 2 + 1);
    if (escaped == NULL) {
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':
                escaped[j++] = '\\';
                escaped[j++] = '"';
                break;
            case '\\':
                escaped[j++] = '\\';
                escaped[j++] = '\\';
                break;
            case '\n':
                escaped[j++] = '\\';
                escaped[j++] = 'n';
                break;
            case '\r':
                escaped[j++] = '\\';
                escaped[j++] = 'r';
                break;
            case '\t':
                escaped[j++] = '\\';
                escaped[j++] = 't';
                break;
            default:
                escaped[j++] = c;
                break;
        }
    }
    escaped[j] = '\0';
    return escaped;
}

/**
 * Escapes XML special characters for Excel/HTML output.
 * @param arena Scratch arena.
 * @param str Input string.
 * @return Escaped string.
 */
static char* escape_xml(Arena* arena, const char* str) {
    if (!str) return "";

    size_t len = 0;
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '<':
                len += 4;
                break;  // &lt;
            case '>':
                len += 4;
                break;  // &gt;
            case '&':
                len += 5;
                break;  // &amp;
            case '"':
                len += 6;
                break;  // &quot;
            case '\'':
                len += 6;
                break;  // &apos;
            default:
                len++;
                break;
        }
    }

    char* res = arena_alloc(arena, len + 1);
    if (!res) return "";

    char* dst = res;
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '<':
                strcpy(dst, "&lt;");
                dst += 4;
                break;
            case '>':
                strcpy(dst, "&gt;");
                dst += 4;
                break;
            case '&':
                strcpy(dst, "&amp;");
                dst += 5;
                break;
            case '"':
                strcpy(dst, "&quot;");
                dst += 6;
                break;
            case '\'':
                strcpy(dst, "&apos;");
                dst += 6;
                break;
            default:
                *dst++ = *p;
                break;
        }
    }
    *dst = '\0';
    return res;
}

/**
 * Sanitizes control characters in a string for display.
 * @param arena Arena for allocation.
 * @param val Input string.
 * @return Sanitized string.
 */
static const char* sanitize_for_display(Arena* arena, const char* val) {
    if (val == NULL) {
        return "";
    }

    // Check if sanitization is needed
    bool dirty = false;
    for (const char* v = val; *v; v++) {
        if (*v == '\t' || *v == '\n' || *v == '\r') {
            dirty = true;
            break;
        }
    }

    if (!dirty) {
        return val;
    }

    // Sanitize
    size_t len = strlen(val);
    char* buf = arena_alloc(arena, len + 1);
    if (!buf) return "";

    char* p = buf;
    for (const char* v = val; *v; v++) {
        if (*v == '\t' || *v == '\n' || *v == '\r') {
            *p++ = ' ';
        } else {
            *p++ = *v;
        }
    }
    *p = '\0';
    return buf;
}

// =============================================================================
// TABLE RENDERING CALLBACKS (for PrettyTable)
// =============================================================================

/**
 * Callback to get header text for a column.
 */
static const char* get_header_cb(void* user_data, int col) {
    TableContext* ctx = (TableContext*)user_data;
    if (ctx->header == NULL) return "";

    size_t actual_col = ctx->col_mapping[col];
    if (actual_col >= ctx->header->count || ctx->header->fields[actual_col] == NULL) {
        return "";
    }

    // Trim string
    char* s = ctx->header->fields[actual_col];
    while (isspace(*s)) s++;

    return s;
}

/**
 * Callback to get cell value with optional coloring.
 * When colors are used, we manually pad because prettytable needs consistent cell lengths.
 */
static const char* get_cell_cb(void* user_data, int row, int col) {
    TableContext* ctx = (TableContext*)user_data;
    Row* r = ctx->rows[row];
    size_t actual_col = ctx->col_mapping[col];

    const char* val = "";
    if (actual_col < r->count && r->fields[actual_col] != NULL) {
        val = r->fields[actual_col];
    }

    // Sanitize and color if needed
    if (ctx->use_colors) {
        const char* color = COLUMN_COLORS[(size_t)col % NUM_COLORS];
        const char* reset = COLOR_RESET;

        size_t len = strlen(val);
        size_t color_len = strlen(color);
        size_t reset_len = strlen(reset);

        // Calculate padding required
        size_t target_width = (ctx->widths) ? ctx->widths[col] : len;
        size_t padding = (target_width > len) ? target_width - len : 0;

        // Allocate: color + val + padding + reset + null
        char* buf = arena_alloc(ctx->arena, color_len + len + padding + reset_len + 1);
        if (!buf) return "";

        char* p = buf;

        // 1. Color code
        memcpy(p, color, color_len);
        p += color_len;

        // 2. Value (sanitized)
        for (const char* v = val; *v; v++) {
            if (*v == '\t' || *v == '\n' || *v == '\r') {
                *p++ = ' ';
            } else {
                *p++ = *v;
            }
        }

        // 3. Padding spaces
        for (size_t i = 0; i < padding; i++) {
            *p++ = ' ';
        }

        // 4. Reset code
        memcpy(p, reset, reset_len);
        p += reset_len;
        *p = '\0';

        return buf;
    }

    // Non-colored path
    return sanitize_for_display(ctx->arena, val);
}

/**
 * Callback to get visible length (ignoring ANSI codes).
 */
static int get_length_cb(void* user_data, const char* text) {
    (void)user_data;
    if (!text) return 0;

    int len = 0;
    const char* p = text;
    while (*p) {
        if (*p == '\033') {
            // Skip ANSI sequence
            p++;
            if (*p == '[') {
                p++;
                while (*p && *p != 'm') {
                    p++;
                }
                if (*p == 'm') p++;
            }
        } else {
            len++;
            p++;
        }
    }
    return len;
}

/**
 * Callback to get describe-table header text.
 */
static const char* get_describe_header_cb(void* user_data, int col) {
    DescribeTableContext* ctx = (DescribeTableContext*)user_data;
    if (ctx == NULL || col < 0 || (size_t)col >= ctx->num_cols) {
        return "";
    }

    const char* header = ctx->headers[col];
    return header != NULL ? header : "";
}

/**
 * Callback to get describe-table cell text with optional colors.
 */
static const char* get_describe_cell_cb(void* user_data, int row, int col) {
    DescribeTableContext* ctx = (DescribeTableContext*)user_data;
    if (ctx == NULL || row < 0 || col < 0 || (size_t)row >= ctx->num_rows ||
        (size_t)col >= ctx->num_cols) {
        return "";
    }

    const char* val = ctx->cells[((size_t)row * ctx->num_cols) + (size_t)col];
    if (val == NULL) {
        val = "";
    }

    if (ctx->use_colors) {
        const char* color = COLUMN_COLORS[(size_t)col % NUM_COLORS];
        const char* reset = COLOR_RESET;

        size_t len = strlen(val);
        size_t color_len = strlen(color);
        size_t reset_len = strlen(reset);
        size_t target_width = (ctx->widths) ? ctx->widths[col] : len;
        size_t padding = (target_width > len) ? target_width - len : 0;

        char* buf = arena_alloc(ctx->arena, color_len + len + padding + reset_len + 1);
        if (buf == NULL) {
            return "";
        }

        char* p = buf;
        memcpy(p, color, color_len);
        p += color_len;

        for (const char* v = val; *v; v++) {
            if (*v == '\t' || *v == '\n' || *v == '\r') {
                *p++ = ' ';
            } else {
                *p++ = *v;
            }
        }

        for (size_t i = 0; i < padding; i++) {
            *p++ = ' ';
        }

        memcpy(p, reset, reset_len);
        p += reset_len;
        *p = '\0';

        return buf;
    }

    return sanitize_for_display(ctx->arena, val);
}

/**
 * Computes column widths for describe-table colored output.
 */
static size_t* compute_describe_widths(Arena* arena, const char** headers, const char** cells,
                                       size_t num_rows, size_t num_cols) {
    size_t* widths = ARENA_ALLOC_ARRAY(arena, size_t, num_cols);
    if (widths == NULL) {
        return NULL;
    }

    for (size_t c = 0; c < num_cols; c++) {
        const char* header = headers[c] != NULL ? headers[c] : "";
        size_t width = strlen(header);
        if (width < MIN_COLUMN_WIDTH) {
            width = MIN_COLUMN_WIDTH;
        }

        for (size_t r = 0; r < num_rows; r++) {
            const char* val = cells[r * num_cols + c];
            if (val == NULL) {
                val = "";
            }

            size_t len = strlen(val);
            if (len > width) {
                width = len;
            }
        }

        widths[c] = width;
    }

    return widths;
}

/**
 * Renders describe statistics using prettytable.
 */
static void print_describe_pretty_table(const char** headers, const char** cells, size_t num_rows,
                                        size_t num_cols, bool use_colors, Arena* arena) {
    DescribeTableContext ctx = {
        .headers = headers,
        .cells = cells,
        .widths = NULL,
        .num_rows = num_rows,
        .num_cols = num_cols,
        .use_colors = use_colors,
        .arena = arena,
    };

    if (use_colors) {
        ctx.widths = compute_describe_widths(arena, headers, cells, num_rows, num_cols);
    }

    prettytable_config cfg;
    prettytable_config_init(&cfg);
    cfg.num_rows = num_rows;
    cfg.num_cols = num_cols;
    cfg.get_header = get_describe_header_cb;
    cfg.get_cell = get_describe_cell_cb;
    cfg.get_length = get_length_cb;
    cfg.user_data = &ctx;
    cfg.show_header = true;
    cfg.style = &PRETTYTABLE_STYLE_BOX;
    cfg.show_row_count = false;

    prettytable_print(&cfg);
}

// =============================================================================
// COLUMN MAPPING
// =============================================================================

/**
 * Builds a mapping of visible column indices.
 * @param arena Arena for allocation.
 * @param original_col_count Total number of columns in the data.
 * @param selection Optional column selection.
 * @param col_mapping Output array of visible column indices.
 * @return Number of visible columns.
 */
static int build_column_mapping(Arena* arena, size_t original_col_count,
                                const ColumnSelection* selection, size_t** col_mapping) {
    int visible_cols = 0;

    if (selection != NULL) {
        visible_cols = selection->count;
        *col_mapping = ARENA_ALLOC_ARRAY(arena, size_t, (size_t)visible_cols);
        if (*col_mapping == NULL) {
            return -1;
        }
        for (int i = 0; i < visible_cols; i++) {
            (*col_mapping)[i] = selection->indices[i];
        }
    } else {
        // Count visible columns
        for (size_t i = 0; i < original_col_count; i++) {
            if (!is_column_hidden(i)) visible_cols++;
        }

        *col_mapping = ARENA_ALLOC_ARRAY(arena, size_t, (size_t)visible_cols);
        if (*col_mapping == NULL) {
            return -1;
        }

        int idx = 0;
        for (size_t i = 0; i < original_col_count; i++) {
            if (!is_column_hidden(i)) {
                (*col_mapping)[idx++] = i;
            }
        }
    }

    return visible_cols;
}

// =============================================================================
// ROW FILTERING
// =============================================================================

/**
 * Filters rows based on pattern and WHERE clause.
 * @param arena Arena for allocation.
 * @param rows All rows.
 * @param row_count Total number of rows.
 * @param has_header Whether first row is header.
 * @param filter_pattern Optional substring filter.
 * @param where Optional WHERE clause.
 * @param filtered_rows Output array of filtered row pointers.
 * @return Number of filtered rows.
 */
static size_t filter_rows(Arena* arena, Row** rows, size_t row_count, bool has_header,
                          const char* filter_pattern, WhereFilter* where, Row*** filtered_rows) {
    size_t start_row = (has_header && row_count > 0) ? 1 : 0;
    size_t data_row_capacity = row_count;

    *filtered_rows = ARENA_ALLOC_ARRAY(arena, Row*, data_row_capacity);
    if (*filtered_rows == NULL) {
        return 0;
    }

    size_t filtered_count = 0;
    for (size_t i = start_row; i < row_count; i++) {
        if (row_passes_filters(rows[i], filter_pattern, where)) {
            (*filtered_rows)[filtered_count++] = rows[i];
        }
    }

    return filtered_count;
}

/**
 * Builds a hash-map key for one row from the selected columns.
 * Column values are trimmed and joined with the ASCII unit separator.
 */
static char* build_unique_key(Arena* arena, const Row* row, const size_t* columns,
                              size_t column_count) {
    size_t total = 0;
    for (size_t i = 0; i < column_count; i++) {
        size_t col = columns[i];
        const char* field =
            (col < row->count && row->fields[col] != NULL) ? row->fields[col] : "";
        const char* start = NULL;
        size_t len = 0;
        trimmed_bounds(field, &start, &len);
        total += len + 1;
    }

    char* key = arena_alloc_unaligned(arena, total + 1);
    if (key == NULL) {
        return NULL;
    }

    char* p = key;
    for (size_t i = 0; i < column_count; i++) {
        size_t col = columns[i];
        const char* field =
            (col < row->count && row->fields[col] != NULL) ? row->fields[col] : "";
        const char* start = NULL;
        size_t len = 0;
        trimmed_bounds(field, &start, &len);
        if (len > 0) {
            memcpy(p, start, len);
            p += len;
        }
        *p++ = '\x1f';
    }
    *p = '\0';
    return key;
}

/**
 * Removes duplicate rows, keeping the first occurrence.
 *
 * When @p unique_by is set, rows are compared using only those columns.
 * Otherwise @p unique_all compares every column. The input order is preserved.
 *
 * @param arena Arena for keys and the result array.
 * @param rows Data rows (no header).
 * @param row_count Number of data rows.
 * @param header Header row used to resolve names in @p unique_by (may be NULL).
 * @param unique_all Deduplicate using every column.
 * @param unique_by Optional comma-separated column list.
 * @param out_rows Output array of unique row pointers.
 * @return Number of rows in the output array.
 */
static size_t apply_unique(Arena* arena, Row** rows, size_t row_count, const Row* header,
                           bool unique_all, const char* unique_by, Row*** out_rows) {
    if ((!unique_all && unique_by == NULL) || row_count == 0) {
        *out_rows = rows;
        return row_count;
    }

    const size_t* columns = NULL;
    size_t column_count = 0;
    ColumnSelection selection = {0};

    if (unique_by != NULL) {
        if (!parse_column_selection(unique_by, header, &selection)) {
            fprintf(stderr, "Warning: --unique-by did not resolve any columns; deduplication skipped.\n");
            *out_rows = rows;
            return row_count;
        }
        columns = selection.indices;
        column_count = selection.count;
    } else {
        size_t total_columns = (header != NULL) ? header->count : rows[0]->count;
        size_t* all_columns = ARENA_ALLOC_ARRAY(arena, size_t, total_columns);
        if (all_columns == NULL) {
            *out_rows = rows;
            return row_count;
        }
        for (size_t i = 0; i < total_columns; i++) {
            all_columns[i] = i;
        }
        columns = all_columns;
        column_count = total_columns;
    }

    Row** result = ARENA_ALLOC_ARRAY(arena, Row*, row_count);
    if (result == NULL) {
        *out_rows = rows;
        return row_count;
    }

    HashMap* seen = map_create(MapConfigStr);
    if (seen == NULL) {
        fprintf(stderr, "Warning: Could not allocate deduplication map; deduplication skipped.\n");
        *out_rows = rows;
        return row_count;
    }

    size_t unique_count = 0;
    for (size_t i = 0; i < row_count; i++) {
        char* key = build_unique_key(arena, rows[i], columns, column_count);
        if (key == NULL) {
            result[unique_count++] = rows[i];
            continue;
        }

        size_t key_len = strlen(key) + 1;
        if (map_get(seen, key, key_len) == NULL) {
            map_set(seen, key, key_len, rows[i]);
            result[unique_count++] = rows[i];
        }
    }

    map_destroy(seen);
    *out_rows = result;
    return unique_count;
}

// =============================================================================
// AGGREGATION AND GROUPING
// =============================================================================

/**
 * Parses one aggregate expression such as "count", "count(*)", "sum(amount)",
 * "avg(score)", "min(age)" or "max(age)".
 *
 * @param arena Arena for labels and column names.
 * @param token Expression text.
 * @param spec Output specification.
 * @return true on success, false on invalid syntax.
 */
static bool parse_aggregate_spec(Arena* arena, const char* token, AggregateSpec* spec) {
    const char* start = NULL;
    size_t len = 0;
    trimmed_bounds(token, &start, &len);
    if (len == 0 || spec == NULL) {
        return false;
    }

    char* label = arena_strdupn(arena, start, len);
    if (label == NULL) {
        return false;
    }

    char* expr = arena_strdup(arena, label);
    if (expr == NULL) {
        return false;
    }

    char* open = strchr(expr, '(');
    if (open == NULL) {
        if (strcasecmp(expr, "count") != 0) {
            return false;
        }
        spec->func = AGG_COUNT;
        spec->label = label;
        spec->column_name = NULL;
        spec->column_idx = (size_t)-1;
        return true;
    }

    char* close = strrchr(expr, ')');
    if (close == NULL || close[1] != '\0') {
        return false;
    }

    *open = '\0';
    *close = '\0';

    char* func_name = expr;
    char* column = open + 1;
    str_trim(func_name);
    str_trim(column);

    AggregateFunc func;
    if (strcasecmp(func_name, "count") == 0) {
        if (strcmp(column, "*") != 0) {
            return false;
        }
        func = AGG_COUNT;
        column = NULL;
    } else if (strcasecmp(func_name, "sum") == 0) {
        func = AGG_SUM;
    } else if (strcasecmp(func_name, "avg") == 0) {
        func = AGG_AVG;
    } else if (strcasecmp(func_name, "min") == 0) {
        func = AGG_MIN;
    } else if (strcasecmp(func_name, "max") == 0) {
        func = AGG_MAX;
    } else {
        return false;
    }

    if (func != AGG_COUNT && (column == NULL || column[0] == '\0')) {
        return false;
    }

    spec->func = func;
    spec->label = label;
    spec->column_name = (column != NULL) ? arena_strdup(arena, column) : NULL;
    spec->column_idx = (size_t)-1;
    return spec->column_name != NULL || func == AGG_COUNT;
}

/**
 * Parses a comma-separated list of aggregate expressions and resolves their
 * column references against @p header.
 */
static bool parse_aggregate_config(Arena* arena, const char* text, const Row* header,
                                   AggregateConfig* config) {
    if (text == NULL || config == NULL) {
        return false;
    }

    config->count = 0;

    char* copy = strdup(text);
    if (copy == NULL) {
        return false;
    }

    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);
    while (token != NULL) {
        if (config->count >= MAX_AGGREGATES) {
            fprintf(stderr, "Error: Too many aggregate expressions (maximum %d)\n",
                    MAX_AGGREGATES);
            free(copy);
            return false;
        }

        AggregateSpec* spec = &config->specs[config->count];
        if (!parse_aggregate_spec(arena, token, spec)) {
            fprintf(stderr,
                    "Error: Invalid aggregate expression '%s'. Use count, count(*), sum(col), "
                    "avg(col), min(col), or max(col).\n",
                    token);
            free(copy);
            return false;
        }

        if (spec->column_name != NULL) {
            if (!resolve_column_ref(arena, header, spec->column_name, &spec->column_idx)) {
                fprintf(stderr, "Error: Unknown aggregate column '%s'\n", spec->column_name);
                free(copy);
                return false;
            }
        }

        config->count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);

    if (config->count == 0) {
        fprintf(stderr, "Error: --aggregate needs at least one expression\n");
        return false;
    }

    return true;
}

/**
 * Parses a comma-separated list of group-by columns and resolves them.
 */
static bool parse_group_columns(Arena* arena, const char* text, const Row* header, size_t* columns,
                                size_t* count) {
    if (text == NULL || columns == NULL || count == NULL) {
        return false;
    }

    *count = 0;

    char* copy = strdup(text);
    if (copy == NULL) {
        return false;
    }

    char* saveptr = NULL;
    char* token = strtok_r(copy, ",", &saveptr);
    while (token != NULL) {
        if (*count >= MAX_GROUP_COLUMNS) {
            fprintf(stderr, "Error: Too many group-by columns (maximum %d)\n",
                    MAX_GROUP_COLUMNS);
            free(copy);
            return false;
        }

        size_t col = 0;
        if (!resolve_column_ref(arena, header, token, &col)) {
            fprintf(stderr, "Error: Unknown group-by column '%s'\n", token);
            free(copy);
            return false;
        }

        columns[(*count)++] = col;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);

    if (*count == 0) {
        fprintf(stderr, "Error: --group-by needs at least one column\n");
        return false;
    }

    return true;
}

/**
 * Updates one aggregate state with a row.
 */
static void update_aggregate_state(AggregateState* state, const Row* row,
                                   const AggregateSpec* spec) {
    if (state == NULL || row == NULL || spec == NULL || spec->func == AGG_COUNT) {
        return;
    }

    const char* field =
        (spec->column_idx < row->count && row->fields[spec->column_idx] != NULL)
            ? row->fields[spec->column_idx]
            : NULL;

    double value = 0.0;
    if (!parse_numeric_field(field, &value)) {
        return;
    }

    if (!state->has_numeric) {
        state->min = value;
        state->max = value;
        state->has_numeric = true;
    } else {
        if (value < state->min) state->min = value;
        if (value > state->max) state->max = value;
    }

    state->sum += value;
    state->numeric_count++;
}

/**
 * Formats one aggregate result as an arena string.
 */
static char* format_aggregate_value(Arena* arena, const AggregateSpec* spec,
                                    const AggregateState* state, size_t row_count) {
    char buf[64];

    if (spec->func == AGG_COUNT) {
        snprintf(buf, sizeof(buf), "%zu", row_count);
        return arena_strdup(arena, buf);
    }

    if (state == NULL || !state->has_numeric || state->numeric_count == 0) {
        return arena_strdup(arena, "-");
    }

    double value = 0.0;
    switch (spec->func) {
        case AGG_SUM:
            value = state->sum;
            break;
        case AGG_AVG:
            value = state->sum / (double)state->numeric_count;
            break;
        case AGG_MIN:
            value = state->min;
            break;
        case AGG_MAX:
            value = state->max;
            break;
        case AGG_COUNT:
            value = 0.0;
            break;
    }

    snprintf(buf, sizeof(buf), "%.6g", value);
    return arena_strdup(arena, buf);
}

/**
 * Builds a synthetic Row from a list of string fields.
 */
static Row* make_synthetic_row(Arena* arena, const char** fields, size_t count) {
    Row* row = arena_alloc_zero(arena, sizeof(Row));
    if (row == NULL) {
        return NULL;
    }

    row->count = count;
    row->fields = ARENA_ALLOC_ARRAY(arena, char*, count);
    if (row->fields == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        row->fields[i] = (char*)fields[i];
    }
    return row;
}

/**
 * Aggregates filtered rows (optionally grouped) and renders the result using
 * the normal output pipeline, so every output format and color setting works.
 */
static bool print_aggregate_stats(Row** rows, size_t row_count, bool has_header,
                                  const char* filter_pattern, WhereFilter* where, bool unique,
                                  const char* unique_by, const char* group_by_str,
                                  const char* aggregate_str, OutputFormat format, bool use_colors,
                                  size_t limit, size_t offset) {
    if (row_count == 0 || rows[0]->count == 0) {
        fprintf(stderr, "Error: No data to aggregate\n");
        return false;
    }

    Arena* arena = arena_create(0);
    if (arena == NULL) {
        fprintf(stderr, "Error: Failed to create aggregation arena\n");
        return false;
    }

    const Row* header = has_header ? rows[0] : NULL;

    if (where != NULL && has_header) {
        resolve_ast_indices(where->root, rows[0]);
    }

    size_t group_columns[MAX_GROUP_COLUMNS] = {0};
    size_t group_column_count = 0;
    if (group_by_str != NULL &&
        !parse_group_columns(arena, group_by_str, header, group_columns, &group_column_count)) {
        arena_destroy(arena);
        return false;
    }

    AggregateConfig aggregates = {0};
    if (aggregate_str != NULL) {
        if (!parse_aggregate_config(arena, aggregate_str, header, &aggregates)) {
            arena_destroy(arena);
            return false;
        }
    } else if (group_column_count > 0) {
        // Grouping without an explicit aggregate defaults to counting rows.
        aggregates.count = 1;
        aggregates.specs[0].func = AGG_COUNT;
        aggregates.specs[0].label = "count";
        aggregates.specs[0].column_name = NULL;
        aggregates.specs[0].column_idx = (size_t)-1;
    } else {
        fprintf(stderr, "Error: --aggregate or --group-by is required\n");
        arena_destroy(arena);
        return false;
    }

    Row** filtered_rows = NULL;
    size_t filtered_count =
        filter_rows(arena, rows, row_count, has_header, filter_pattern, where, &filtered_rows);

    if (unique || unique_by != NULL) {
        Row** unique_rows = NULL;
        filtered_count = apply_unique(arena, filtered_rows, filtered_count, header, unique, unique_by,
                                      &unique_rows);
        filtered_rows = unique_rows;
    }

    size_t max_groups = (group_column_count > 0 && filtered_count > 0) ? filtered_count : 1;
    AggregateGroup* groups = ARENA_ALLOC_ARRAY(arena, AggregateGroup, max_groups);
    if (groups == NULL) {
        fprintf(stderr, "Error: Failed to allocate aggregation groups\n");
        arena_destroy(arena);
        return false;
    }

    HashMap* group_map = NULL;
    if (group_column_count > 0) {
        group_map = map_create(MapConfigStr);
        if (group_map == NULL) {
            fprintf(stderr, "Error: Failed to allocate group-by map\n");
            arena_destroy(arena);
            return false;
        }
    }

    size_t group_total = 0;
    for (size_t r = 0; r < filtered_count; r++) {
        Row* row = filtered_rows[r];
        AggregateGroup* group = NULL;

        if (group_column_count == 0) {
            if (group_total == 0) {
                group = &groups[group_total++];
            } else {
                group = &groups[0];
            }
        } else {
            char* key = build_unique_key(arena, row, group_columns, group_column_count);
            if (key == NULL) {
                continue;
            }

            size_t key_len = strlen(key) + 1;
            group = map_get(group_map, key, key_len);
            if (group == NULL) {
                group = &groups[group_total++];
                group->key_values = ARENA_ALLOC_ARRAY(arena, const char*, group_column_count);
                if (group->key_values == NULL) {
                    continue;
                }
                for (size_t c = 0; c < group_column_count; c++) {
                    size_t col = group_columns[c];
                    const char* field =
                        (col < row->count && row->fields[col] != NULL) ? row->fields[col] : "";
                    group->key_values[c] = arena_trimmed_strdup(arena, field);
                }
                map_set(group_map, key, key_len, group);
            }
        }

        if (group == NULL) {
            continue;
        }

        if (group->states == NULL) {
            group->states = ARENA_ALLOC_ARRAY(arena, AggregateState, aggregates.count);
            if (group->states == NULL) {
                continue;
            }
        }

        group->row_count++;
        for (size_t a = 0; a < aggregates.count; a++) {
            update_aggregate_state(&group->states[a], row, &aggregates.specs[a]);
        }
    }

    if (group_column_count == 0 && group_total == 0) {
        group_total = 1;
        groups[0].states = ARENA_ALLOC_ARRAY(arena, AggregateState, aggregates.count);
    }

    size_t output_columns = group_column_count + aggregates.count;
    Row** output_rows = ARENA_ALLOC_ARRAY(arena, Row*, group_total + 1);
    if (output_rows == NULL) {
        map_destroy(group_map);
        arena_destroy(arena);
        return false;
    }

    const char** header_fields = ARENA_ALLOC_ARRAY(arena, const char*, output_columns);
    if (header_fields == NULL) {
        map_destroy(group_map);
        arena_destroy(arena);
        return false;
    }

    for (size_t c = 0; c < group_column_count; c++) {
        size_t col = group_columns[c];
        if (header != NULL && col < header->count && header->fields[col] != NULL) {
            header_fields[c] = arena_trimmed_strdup(arena, header->fields[col]);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "col_%zu", col);
            header_fields[c] = arena_strdup(arena, buf);
        }
    }

    for (size_t a = 0; a < aggregates.count; a++) {
        header_fields[group_column_count + a] = aggregates.specs[a].label;
    }

    output_rows[0] = make_synthetic_row(arena, header_fields, output_columns);

    for (size_t g = 0; g < group_total; g++) {
        AggregateGroup* group = &groups[g];
        const char** fields = ARENA_ALLOC_ARRAY(arena, const char*, output_columns);
        if (fields == NULL) {
            continue;
        }

        for (size_t c = 0; c < group_column_count; c++) {
            fields[c] = (group->key_values != NULL && group->key_values[c] != NULL)
                            ? group->key_values[c]
                            : "";
        }

        for (size_t a = 0; a < aggregates.count; a++) {
            fields[group_column_count + a] =
                format_aggregate_value(arena, &aggregates.specs[a], &group->states[a],
                                       group->row_count);
        }

        output_rows[g + 1] = make_synthetic_row(arena, fields, output_columns);
    }

    map_destroy(group_map);

    PrintConfig config = {.has_header = true,
                          .format = format,
                          .use_colors = use_colors,
                          .use_bgcolor = false,
                          .filter_pattern = NULL,
                          .where = NULL,
                          .selection = NULL,
                          .unique = false,
                          .unique_by = NULL,
                          .limit = limit,
                          .offset = offset};

    print_table(output_rows, group_total + 1, &config);

    arena_destroy(arena);
    return true;
}

// =============================================================================
// FORMAT-SPECIFIC OUTPUT
// =============================================================================

/**
 * Prints a single row in the specified format.
 */
static void print_row_format(const Row* row, size_t col_count, OutputFormat format,
                             const ColumnSelection* selection, const Row* header, bool is_last_row,
                             Arena* scratch) {
    if (row == NULL) {
        return;
    }

    switch (format) {
        case OUTPUT_CSV: {
            bool first_field = true;
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                if (!first_field) {
                    putchar(',');
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                // Quote field if it contains comma, quote, or newline
                bool needs_quotes = strchr(field, ',') || strchr(field, '"') || strchr(field, '\n');
                if (needs_quotes) {
                    putchar('"');
                    // Escape quotes by doubling them
                    for (const char* p = field; *p; p++) {
                        if (*p == '"') {
                            putchar('"');
                        }
                        putchar(*p);
                    }
                    putchar('"');
                } else {
                    fputs(field, stdout);
                }
                first_field = false;
            }
            putchar('\n');
            break;
        }

        case OUTPUT_TSV: {
            bool first_field = true;
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                if (!first_field) {
                    putchar('\t');
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                fputs(field, stdout);
                first_field = false;
            }
            putchar('\n');
            break;
        }

        case OUTPUT_JSON: {
            printf("  {");
            bool first_field = true;
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                if (!first_field) {
                    printf(", ");
                }

                const char* field_name = "field";
                if (header != NULL && col < header->count && header->fields[col] != NULL) {
                    field_name = header->fields[col];
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                char* escaped_key = trim_and_escape_json(scratch, field_name);
                char* escaped_val = trim_and_escape_json(scratch, field);

                printf("\"%s\": \"%s\"", escaped_key != NULL ? escaped_key : "",
                       escaped_val != NULL ? escaped_val : "");

                first_field = false;
            }
            printf("}%s\n", is_last_row ? "" : ",");
            break;
        }

        case OUTPUT_MARKDOWN: {
            putchar('|');
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                printf(" %s |", field);
            }
            putchar('\n');
            break;
        }

        case OUTPUT_HTML: {
            printf("<tr>");
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                char* escaped = escape_xml(scratch, field);
                printf("<td>%s</td>", escaped);
            }
            printf("</tr>\n");
            break;
        }

        case OUTPUT_EXCEL: {
            printf("   <Row>\n");
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    // Trim a scratch copy so synthetic rows and shared header
                    // values are never modified.
                    char* copy = arena_strdup(scratch, row->fields[col]);
                    if (copy != NULL) {
                        str_trim(copy);
                        field = copy;
                    }
                }

                // Check if number
                char* endptr;
                strtod(field, &endptr);
                bool is_num = (*field != '\0' && *endptr == '\0');

                char* escaped = escape_xml(scratch, field);

                if (is_num) {
                    printf("    <Cell><Data ss:Type=\"Number\">%s</Data></Cell>\n", escaped);
                } else {
                    printf("    <Cell><Data ss:Type=\"String\">%s</Data></Cell>\n", escaped);
                }
            }
            printf("   </Row>\n");
            break;
        }

        default:
            break;
    }
}

/**
 * Prints markdown table separator after header.
 */
static void print_markdown_separator(size_t col_count, const ColumnSelection* selection) {
    putchar('|');
    for (size_t i = 0; i < col_count; i++) {
        size_t col = selection != NULL ? selection->indices[i] : i;

        if (selection == NULL && is_column_hidden(col)) {
            continue;
        }
        printf(" --- |");
    }
    putchar('\n');
}

// =============================================================================
// TABLE OUTPUT (using PrettyTable)
// =============================================================================

/**
 * Computes column widths for colored output.
 * This is necessary because when using colors, we manually pad cells.
 */
static size_t* compute_column_widths(Arena* arena, Row** rows, size_t row_count, const Row* header,
                                     size_t* col_mapping, int visible_cols) {
    if (visible_cols <= 0) return NULL;
    size_t* widths = ARENA_ALLOC_ARRAY(arena, size_t, (size_t)visible_cols);
    if (!widths) return NULL;

    for (int j = 0; j < visible_cols; j++) {
        widths[j] = MIN_COLUMN_WIDTH;
        size_t original_col = col_mapping[j];

        // Check header
        if (header != NULL && original_col < header->count && header->fields[original_col]) {
            size_t h_len = strlen(header->fields[original_col]);
            if (h_len > widths[j]) widths[j] = h_len;
        }

        // Check data rows
        for (size_t i = 0; i < row_count; i++) {
            Row* r = rows[i];
            if (original_col < r->count && r->fields[original_col]) {
                size_t len = strlen(r->fields[original_col]);
                if (len > widths[j]) widths[j] = len;
            }
        }
    }

    return widths;
}

/**
 * Prints data as a pretty table using the prettytable library.
 */
static void print_pretty_table(Row** filtered_rows, size_t filtered_count, const Row* header,
                               size_t* col_mapping, int visible_cols, bool use_colors,
                               Arena* arena) {
    // Compute widths if using colors (needed for manual padding)
    size_t* widths = NULL;
    if (use_colors) {
        widths = compute_column_widths(arena, filtered_rows, filtered_count, header, col_mapping,
                                       visible_cols);
    }

    TableContext ctx = {.rows = filtered_rows,
                        .col_mapping = col_mapping,
                        .widths = widths,
                        .use_colors = use_colors,
                        .arena = arena,
                        .header = header};

    prettytable_config cfg;
    prettytable_config_init(&cfg);
    cfg.num_rows = filtered_count;
    cfg.num_cols = visible_cols;
    cfg.get_header = get_header_cb;
    cfg.get_cell = get_cell_cb;
    cfg.get_length = get_length_cb;
    cfg.user_data = &ctx;
    cfg.show_header = (header != NULL);
    cfg.style = &PRETTYTABLE_STYLE_BOX;
    cfg.show_row_count = true;

    prettytable_print(&cfg);
}

// =============================================================================
// FORMAT-SPECIFIC HEADERS AND FOOTERS
// =============================================================================

/**
 * Prints format-specific headers.
 */
static void print_format_header(OutputFormat format, bool has_header, Row** rows, size_t col_count,
                                const ColumnSelection* selection, Arena* scratch) {
    switch (format) {
        case OUTPUT_JSON:
            printf("[\n");
            break;

        case OUTPUT_HTML:
            printf("<table>\n");
            if (has_header) {
                printf("  <thead>\n    <tr>");
                for (size_t i = 0; i < col_count; i++) {
                    size_t col = selection != NULL ? selection->indices[i] : i;
                    if (selection == NULL && is_column_hidden(col)) continue;

                    const char* field = "";
                    if (col < rows[0]->count && rows[0]->fields[col] != NULL) {
                        field = rows[0]->fields[col];
                    }
                    char* escaped = escape_xml(scratch, field);
                    printf("<th>%s</th>", escaped);
                }
                printf("</tr>\n  </thead>\n  <tbody>\n");
            } else {
                printf("  <tbody>\n");
            }
            break;

        case OUTPUT_EXCEL:
            printf("<?xml version=\"1.0\"?>\n");
            printf("<?mso-application progid=\"Excel.Sheet\"?>\n");
            printf("<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"\n");
            printf(" xmlns:o=\"urn:schemas-microsoft-com:office:office\"\n");
            printf(" xmlns:x=\"urn:schemas-microsoft-com:office:excel\"\n");
            printf(" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"\n");
            printf(" xmlns:html=\"http://www.w3.org/TR/REC-html40\">\n");
            printf(" <Styles>\n");
            printf("  <Style ss:ID=\"sHeader\">\n");
            printf("   <Font ss:Bold=\"1\"/>\n");
            printf("  </Style>\n");
            printf(" </Styles>\n");
            printf(" <Worksheet ss:Name=\"Sheet1\">\n");
            printf("  <Table>\n");

            if (has_header) {
                printf("   <Row>\n");
                for (size_t i = 0; i < col_count; i++) {
                    size_t col = selection != NULL ? selection->indices[i] : i;
                    if (selection == NULL && is_column_hidden(col)) continue;

                    const char* field = "";
                    if (col < rows[0]->count && rows[0]->fields[col] != NULL) {
                        field = rows[0]->fields[col];
                    }
                    char* escaped = escape_xml(scratch, field);
                    printf(
                        "    <Cell ss:StyleID=\"sHeader\"><Data "
                        "ss:Type=\"String\">%s</Data></Cell>\n",
                        escaped);
                }
                printf("   </Row>\n");
            }
            break;

        default:
            // CSV, TSV, Markdown - header printed with data
            break;
    }
}

/**
 * Prints format-specific footers.
 */
static void print_format_footer(OutputFormat format, size_t filtered_count, size_t total_data_rows,
                                bool has_filter) {
    switch (format) {
        case OUTPUT_JSON:
            printf("]\n");
            break;

        case OUTPUT_HTML:
            printf("  </tbody>\n</table>\n");
            break;

        case OUTPUT_EXCEL:
            printf("  </Table>\n");
            printf(" </Worksheet>\n");
            printf("</Workbook>\n");
            break;

        case OUTPUT_MARKDOWN:
            if (has_filter) {
                printf("\nFiltered: %zu/%zu rows matched\n", filtered_count, total_data_rows);
            }
            break;

        default:
            break;
    }
}

// =============================================================================
// MAIN PRINT FUNCTION
// =============================================================================

/**
 * Pretty-prints the CSV data in the specified format.
 */
static void print_table(Row** rows, size_t row_count, const PrintConfig* config) {
    if (row_count == 0 || rows[0]->count == 0) {
        fprintf(stderr, "Error: No data to print\n");
        return;
    }

    size_t original_col_count = rows[0]->count;
    size_t col_count = config->selection != NULL ? config->selection->count : original_col_count;

    // Resolve where clause column indices if present
    if (config->where != NULL && config->has_header) {
        resolve_ast_indices(config->where->root, rows[0]);
    }

    // Create arena for this operation
    Arena* print_arena = arena_create(0);
    if (!print_arena) {
        fprintf(stderr, "Error: Failed to create print arena\n");
        return;
    }

    // Build column mapping
    size_t* col_mapping = NULL;
    int visible_cols =
        build_column_mapping(print_arena, original_col_count, config->selection, &col_mapping);
    if (visible_cols < 0 || col_mapping == NULL) {
        fprintf(stderr, "Error: Failed to build column mapping\n");
        arena_destroy(print_arena);
        return;
    }

    // Filter rows
    Row** filtered_rows = NULL;
    size_t filtered_count = filter_rows(print_arena, rows, row_count, config->has_header,
                                        config->filter_pattern, config->where, &filtered_rows);

    // Remove duplicate rows after filtering, preserving sort order.
    if (config->unique || config->unique_by != NULL) {
        Row** unique_rows = NULL;
        const Row* unique_header = config->has_header ? rows[0] : NULL;
        filtered_count = apply_unique(print_arena, filtered_rows, filtered_count, unique_header,
                                      config->unique, config->unique_by, &unique_rows);
        filtered_rows = unique_rows;
    }

    size_t window_start = 0;
    size_t window_count = 0;
    compute_row_window(filtered_count, config->offset, config->limit, &window_start, &window_count);
    Row** window_rows = filtered_rows + window_start;

    // Print based on format
    if (config->format == OUTPUT_TABLE) {
        const Row* header = config->has_header ? rows[0] : NULL;
        print_pretty_table(window_rows, window_count, header, col_mapping, visible_cols,
                           config->use_colors, print_arena);
    } else {
        // Other formats
        Arena* scratch = arena_create(0);
        if (!scratch) {
            fprintf(stderr, "Error: Failed to create scratch arena\n");
            arena_destroy(print_arena);
            return;
        }

        // Print header
        print_format_header(config->format, config->has_header, rows, col_count, config->selection,
                            scratch);

        // Print data header for CSV/TSV/Markdown
        if (config->has_header && config->format != OUTPUT_JSON && config->format != OUTPUT_HTML &&
            config->format != OUTPUT_EXCEL) {
            print_row_format(rows[0], col_count, config->format, config->selection, NULL, false,
                             scratch);
            if (config->format == OUTPUT_MARKDOWN) {
                print_markdown_separator(col_count, config->selection);
            }
        }

        // Print data rows
        for (size_t i = 0; i < window_count; i++) {
            bool is_last = (i == window_count - 1);
            const Row* header = config->has_header ? rows[0] : NULL;
            print_row_format(window_rows[i], col_count, config->format, config->selection, header,
                             is_last, scratch);
            arena_reset(scratch);
        }

        // Print footer
        size_t total_data_rows = row_count - (config->has_header ? 1 : 0);
        bool has_filter = (config->filter_pattern != NULL && config->filter_pattern[0] != '\0') ||
                          (config->where != NULL);
        print_format_footer(config->format, filtered_count, total_data_rows, has_filter);

        arena_destroy(scratch);
    }

    arena_destroy(print_arena);
}

// =============================================================================
// COMMAND-LINE PARSING
// =============================================================================

/**
 * Copies standard input to a temporary file so the CSV reader can seek over
 * it (the solidc parser counts rows before parsing and therefore needs a
 * seekable stream).
 *
 * @return malloc'd path to the temporary file, or NULL on failure.
 *         The caller must remove() and free() it.
 */
static char* read_stdin_to_tempfile(void) {
#ifdef _WIN32
    char* path = _tempnam(NULL, "csvq");
    if (path == NULL) {
        return NULL;
    }
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        free(path);
        return NULL;
    }
#else
    char template[] = "/tmp/csvq-stdin-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        return NULL;
    }

    FILE* file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        remove(template);
        return NULL;
    }

    char* path = strdup(template);
    if (path == NULL) {
        fclose(file);
        remove(template);
        return NULL;
    }
#endif

    char buffer[8192];
    size_t read_count = 0;
    while ((read_count = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        if (fwrite(buffer, 1, read_count, file) != read_count) {
            fclose(file);
            remove(path);
            free(path);
            return NULL;
        }
    }

    bool read_error = ferror(stdin) != 0;
    fclose(file);

    if (read_error) {
        remove(path);
        free(path);
        return NULL;
    }

    return path;
}

/**
 * Parses delimiter string (handles special case of "\t" for tab).
 */
static char parse_delimiter(const char* delim_arg) {
    if (delim_arg == NULL) {
        return ',';
    }

    if (strcmp(delim_arg, "\\t") == 0) {
        return '\t';
    }

    return delim_arg[0];
}

/**
 * Parses output format string.
 */
static OutputFormat parse_output_format(const char* format_str) {
    if (format_str == NULL) {
        return OUTPUT_TABLE;
    }

    if (strcasecmp(format_str, "csv") == 0) {
        return OUTPUT_CSV;
    } else if (strcasecmp(format_str, "tsv") == 0) {
        return OUTPUT_TSV;
    } else if (strcasecmp(format_str, "json") == 0) {
        return OUTPUT_JSON;
    } else if (strcasecmp(format_str, "markdown") == 0 || strcasecmp(format_str, "md") == 0) {
        return OUTPUT_MARKDOWN;
    } else if (strcasecmp(format_str, "html") == 0) {
        return OUTPUT_HTML;
    } else if (strcasecmp(format_str, "excel") == 0 || strcasecmp(format_str, "xls") == 0) {
        return OUTPUT_EXCEL;
    } else if (strcasecmp(format_str, "table") != 0) {
        fprintf(stderr, "Warning: Unknown format '%s', using table\n", format_str);
    }

    return OUTPUT_TABLE;
}

// =============================================================================
// MAIN ENTRY POINT
// =============================================================================

int main(int argc, char* argv[]) {
    // Create main arena
    Arena* arena = arena_create(0);
    if (!arena) {
        fprintf(stderr, "Error: Failed to initialize arena\n");
        return EXIT_FAILURE;
    }

    FlagParser* parser = flag_parser_new("csvq", "Query and format CSV files");
    if (parser == NULL) {
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Command-line arguments
    bool has_header = true;
    bool skip_header = false;
    bool use_colors = false;
    bool use_bgcolor = false;
    char comment = '#';
    char* delim_arg = ",";
    char* hide_cols = NULL;
    char* filter_pattern = NULL;
    char* where_str = NULL;
    char* select_str = NULL;
    char* format_str = NULL;
    bool sort_desc = false;
    char* sort_col = NULL;
    bool count_only = false;
    bool describe_only = false;
    bool unique = false;
    char* unique_by = NULL;
    char* group_by_str = NULL;
    char* aggregate_str = NULL;
    bool show_version = false;
    bool use_stdin = false;
    char* limit_str = NULL;
    char* offset_str = NULL;
    size_t limit = SIZE_MAX;
    size_t offset = 0;

    // Define flags
    flag_bool(parser, "header", 'h', "The CSV file has a header", &has_header);
    flag_bool(parser, "skip-header", 's', "Skip the header", &skip_header);
    flag_bool(parser, "color", 'C', "Use text colors for each column", &use_colors);
    flag_bool(parser, "bgcolor", 'G', "Use background color for rows", &use_bgcolor);
    flag_bool(parser, "desc", 'D', "Sort in descending order", &sort_desc);
    flag_bool(parser, "count", 'n', "Print number of rows after filtering", &count_only);
    flag_bool(parser, "describe", 'a',
              "Print numeric stats (count/min/max/mean) for visible columns", &describe_only);
    flag_bool(parser, "unique", 'u', "Remove duplicate rows (all columns)", &unique);
    flag_bool(parser, "version", 'V', "Print version information and exit", &show_version);
    flag_bool(parser, "stdin", 0, "Read CSV data from standard input (or use '-' as filename)",
              &use_stdin);
    flag_char(parser, "comment", 'c', "Comment Character", &comment);
    flag_string(parser, "delimiter", 'd', "The CSV delimiter (use '\\t' for tab)", &delim_arg);
    flag_string(parser, "hide", 'H', "Comma-separated column indices to hide (e.g., 0,2,5)",
                &hide_cols);
    flag_string(parser, "filter", 'f', "Show only rows containing this pattern", &filter_pattern);
    flag_string(parser, "where", 'w',
                "Filter rows with condition (e.g., 'age > 25', 'name contains John' or 'age > 25 "
                "OR status = active')",
                &where_str);
    flag_string(parser, "select", 'S', "Select and order columns (e.g., 'name,age' or '0,2,1')",
                &select_str);
    flag_string(parser, "output", 'o',
                "Output format: table (default), csv, tsv, json, markdown, html, excel",
                &format_str);
    flag_string(parser, "sort", 'B',
                "Sort by column(s); comma-separate keys, prefix '-' for descending (e.g., "
                "'city,-age')",
                &sort_col);
    flag_string(parser, "unique-by", 'U', "Remove duplicates using these columns (e.g., 'id')",
                &unique_by);
    flag_string(parser, "group-by", 'g',
                "Group rows by columns before aggregating (e.g., 'region')", &group_by_str);
    flag_string(parser, "aggregate", 'A',
                "Aggregate expressions: count, count(*), sum(col), avg(col), min(col), max(col)",
                &aggregate_str);
    flag_string(parser, "limit", 'l', "Limit output rows after filtering/sorting", &limit_str);
    flag_string(parser, "offset", 'O', "Skip N rows after filtering/sorting", &offset_str);

    // Accept a bare "-" as an alias for --stdin. The flag parser treats
    // dash-prefixed arguments as flags, so rewrite it before parsing.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], STDIN_FILENAME) == 0) {
            argv[i] = (char*)"--stdin";
        }
    }

    // Parse flags
    if (flag_parse(parser, argc, argv) != FLAG_OK) {
        const char* parse_error = flag_get_error(parser);
        if (parse_error != NULL && parse_error[0] != '\0') {
            fprintf(stderr, "Error: %s\n", parse_error);
        } else {
            fprintf(stderr, "Error: Failed to parse command-line arguments\n");
        }
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    if (show_version) {
        printf("csvq %s\n", CSVQ_VERSION);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_SUCCESS;
    }

    if (flag_positional_count(parser) < 1 && !use_stdin) {
        fprintf(stderr, "Required positional argument <filename> is missing\n");
        flag_print_usage(parser);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    const char* filename =
        flag_positional_count(parser) > 0 ? flag_positional_at(parser, 0) : NULL;
    const char* input_filename = filename;
    char* stdin_temp_path = NULL;
    if (use_stdin || (filename != NULL && strcmp(filename, STDIN_FILENAME) == 0)) {
        stdin_temp_path = read_stdin_to_tempfile();
        if (stdin_temp_path == NULL) {
            fprintf(stderr, "Error: Failed to read CSV data from standard input\n");
            flag_parser_free(parser);
            arena_destroy(arena);
            return EXIT_FAILURE;
        }
        input_filename = stdin_temp_path;
    }

    // Parse configuration
    char delimiter = parse_delimiter(delim_arg);
    OutputFormat format = parse_output_format(format_str);

    if (skip_header) {
        has_header = false;
    }

    if (limit_str != NULL && !parse_non_negative_size(limit_str, "limit", &limit)) {
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    if (offset_str != NULL && !parse_non_negative_size(offset_str, "offset", &offset)) {
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Parse hidden columns
    if (hide_cols != NULL && parse_hidden_columns(hide_cols) < 0) {
        fprintf(stderr, "Error: Failed to parse hidden columns\n");
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Initialize CSV reader
    CsvReader* reader = csv_reader_new(input_filename, 0);
    if (reader == NULL) {
        if (stdin_temp_path != NULL) {
            remove(stdin_temp_path);
            free(stdin_temp_path);
        }
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Configure and parse CSV
    CsvReaderConfig config = {
        .has_header = has_header,
        .skip_header = skip_header,
        .comment = comment,
        .delim = delimiter,
    };

    csv_reader_setconfig(reader, config);

    Row** rows = csv_reader_parse(reader);
    if (stdin_temp_path != NULL) {
        remove(stdin_temp_path);
        free(stdin_temp_path);
        stdin_temp_path = NULL;
    }
    if (rows == NULL) {
        fprintf(stderr, "Error: Failed to parse CSV file, likely due to invalid delimiter. ");
        fprintf(stderr, "Use --delimiter='\\\\t' for Tab-separated Value file\n");
        csv_reader_free(reader);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    size_t count = csv_reader_numrows(reader);
    if (count == 0) {
        fprintf(stderr, "Error: No rows in CSV file\n");
        csv_reader_free(reader);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Sort if requested
    if (sort_col != NULL) {
        sort_rows(arena, rows, count, has_header, sort_col, sort_desc);
    }

    // Parse column selection
    ColumnSelection selection = {0};
    ColumnSelection* sel_ptr = NULL;
    if (select_str != NULL) {
        const Row* header = has_header ? rows[0] : NULL;
        if (parse_column_selection(select_str, header, &selection)) {
            sel_ptr = &selection;
        }
    }

    // Parse WHERE clause
    WhereFilter where = {0};
    WhereFilter* where_ptr = NULL;
    if (where_str != NULL) {
        if (parse_where_clause(arena, where_str, &where)) {
            where_ptr = &where;
        }
    }

    if (where_ptr != NULL && has_header) {
        resolve_ast_indices(where_ptr->root, rows[0]);
    }

    // Aggregation/grouping takes precedence over --count and --describe.
    if (aggregate_str != NULL || group_by_str != NULL) {
        bool ok = print_aggregate_stats(rows, count, has_header, filter_pattern, where_ptr, unique,
                                        unique_by, group_by_str, aggregate_str, format, use_colors,
                                        limit, offset);
        csv_reader_free(reader);
        flag_parser_free(parser);
        arena_destroy(arena);
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (count_only) {
        Row** filtered_rows = NULL;
        size_t matched =
            filter_rows(arena, rows, count, has_header, filter_pattern, where_ptr, &filtered_rows);

        if (unique || unique_by != NULL) {
            Row** unique_rows = NULL;
            const Row* header = has_header ? rows[0] : NULL;
            matched = apply_unique(arena, filtered_rows, matched, header, unique, unique_by,
                                   &unique_rows);
        }

        printf("%zu\n", matched);
        csv_reader_free(reader);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_SUCCESS;
    }

    if (describe_only) {
        print_describe_stats(rows, count, has_header, filter_pattern, where_ptr, sel_ptr, unique,
                             unique_by, use_colors);
        csv_reader_free(reader);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_SUCCESS;
    }

    // Prepare print configuration
    PrintConfig print_config = {.has_header = has_header,
                                .format = format,
                                .use_colors = use_colors,
                                .use_bgcolor = use_bgcolor,
                                .filter_pattern = filter_pattern,
                                .where = where_ptr,
                                .selection = sel_ptr,
                                .unique = unique,
                                .unique_by = unique_by,
                                .limit = limit,
                                .offset = offset};

    // Print the table
    print_table(rows, count, &print_config);

    // Cleanup
    csv_reader_free(reader);
    flag_parser_free(parser);
    arena_destroy(arena);

    return EXIT_SUCCESS;
}
