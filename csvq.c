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
#include <solidc/arena.h>        // Arena allocator
#include <solidc/csvparser.h>    // CSV parsing functions
#include <solidc/flags.h>        // Command-line parser
#include <solidc/prettytable.h>  // Table printer
#include <solidc/str_utils.h>    // trim_string
#include <stdbool.h>             // for bool
#include <stdio.h>               // for fprintf, printf, stderr
#include <stdlib.h>              // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free, calloc
#include <string.h>              // for strlen, strcasestr, strcmp, strdup
#include <strings.h>             // for strcasecmp
#include "where-parser.h"

// =============================================================================
// CONSTANTS AND CONFIGURATION
// =============================================================================

/** Minimum column width for aesthetics. */
#define MIN_COLUMN_WIDTH 3

/** Maximum number of columns we support hiding. */
#define MAX_HIDDEN_COLUMNS 64

/** Maximum number of columns we support selecting/reordering. */
#define MAX_SELECTED_COLUMNS 64

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

/** Context for the qsort comparison function. */
typedef struct {
    size_t col_idx;  // Index of column to sort by
    bool desc;       // Sort descending?
    bool active;     // Is sorting active?
} SortContext;

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
} PrintConfig;

// =============================================================================
// GLOBAL STATE
// =============================================================================

/**
 * Simple bitset for tracking hidden columns.
 * Much more efficient than dynamic arrays for up to 64 columns.
 */
static unsigned long hidden_columns_mask = 0;

/** Context for the qsort comparison function. */
static SortContext sort_ctx = {0, false, false};

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
 * Parses a column selection string (e.g., "name,age,email" or "0,2,1").
 * @param select_str The selection string.
 * @param header The header row (needed to resolve names to indices).
 * @param selection Output ColumnSelection structure.
 * @return true on success, false on error.
 */
static bool parse_column_selection(const char* select_str, const Row* header, ColumnSelection* selection) {
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
 * Tries to compare numerically first; falls back to string comparison.
 */
static int compare_rows(const void* a, const void* b) {
    const Row* r1 = *(const Row**)a;
    const Row* r2 = *(const Row**)b;

    // Handle rows with missing columns safely
    const char* val1 =
        (sort_ctx.col_idx < r1->count && r1->fields[sort_ctx.col_idx]) ? r1->fields[sort_ctx.col_idx] : "";
    const char* val2 =
        (sort_ctx.col_idx < r2->count && r2->fields[sort_ctx.col_idx]) ? r2->fields[sort_ctx.col_idx] : "";

    int result = 0;

    // Try numeric comparison first
    char *end1, *end2;
    double d1 = strtod(val1, &end1);
    double d2 = strtod(val2, &end2);

    // If both values parsed completely as numbers
    if (*val1 != '\0' && *val2 != '\0' && *end1 == '\0' && *end2 == '\0') {
        if (d1 < d2)
            result = -1;
        else if (d1 > d2)
            result = 1;
        else
            result = 0;
    } else {
        // Fallback to case-insensitive string comparison
        result = strcasecmp(val1, val2);
    }

    return sort_ctx.desc ? -result : result;
}

/**
 * Sorts rows by a specified column.
 * @param rows Array of row pointers.
 * @param count Total number of rows.
 * @param has_header Whether the first row is a header.
 * @param sort_col Column name or index to sort by.
 * @param sort_desc Sort in descending order?
 * @return true on success, false if column not found.
 */
static bool sort_rows(Row** rows, size_t count, bool has_header, const char* sort_col, bool sort_desc) {
    if (sort_col == NULL || count == 0) {
        return false;
    }

    long idx = -1;
    char* endptr;
    long parsed_idx = strtol(sort_col, &endptr, 10);

    // Determine Index
    if (*endptr == '\0' && parsed_idx >= 0) {
        idx = parsed_idx;
    } else if (has_header) {
        ssize_t found = find_column_by_name(rows[0], sort_col);
        if (found >= 0) {
            idx = found;
        }
    }

    if (idx < 0) {
        fprintf(stderr, "Warning: Could not resolve sort column '%s'. Sorting skipped.\n", sort_col);
        return false;
    }

    sort_ctx.col_idx = (size_t)idx;
    sort_ctx.desc = sort_desc;
    sort_ctx.active = true;

    // If we have a header, we must NOT sort row[0].
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

    char* str = (char*)s;
    str = trim_string(str);

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
static int build_column_mapping(Arena* arena, size_t original_col_count, const ColumnSelection* selection,
                                size_t** col_mapping) {
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
static size_t filter_rows(Arena* arena, Row** rows, size_t row_count, bool has_header, const char* filter_pattern,
                          WhereFilter* where, Row*** filtered_rows) {
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

// =============================================================================
// FORMAT-SPECIFIC OUTPUT
// =============================================================================

/**
 * Prints a single row in the specified format.
 */
static void print_row_format(const Row* row, size_t col_count, OutputFormat format, const ColumnSelection* selection,
                             const Row* header, bool is_last_row, Arena* scratch) {
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
                    field = row->fields[col];
                    field = trim_string(field);
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
static size_t* compute_column_widths(Arena* arena, Row** rows, size_t row_count, const Row* header, size_t* col_mapping,
                                     int visible_cols) {
    size_t* widths = ARENA_ALLOC_ARRAY(arena, size_t, visible_cols);
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
static void print_pretty_table(Row** filtered_rows, size_t filtered_count, const Row* header, size_t* col_mapping,
                               int visible_cols, bool use_colors, Arena* arena) {
    // Compute widths if using colors (needed for manual padding)
    size_t* widths = NULL;
    if (use_colors) {
        widths = compute_column_widths(arena, filtered_rows, filtered_count, header, col_mapping, visible_cols);
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
                    printf("    <Cell ss:StyleID=\"sHeader\"><Data ss:Type=\"String\">%s</Data></Cell>\n", escaped);
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
static void print_format_footer(OutputFormat format, size_t filtered_count, size_t total_data_rows, bool has_filter) {
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
    int visible_cols = build_column_mapping(print_arena, original_col_count, config->selection, &col_mapping);
    if (visible_cols < 0 || col_mapping == NULL) {
        fprintf(stderr, "Error: Failed to build column mapping\n");
        arena_destroy(print_arena);
        return;
    }

    // Filter rows
    Row** filtered_rows = NULL;
    size_t filtered_count = filter_rows(print_arena, rows, row_count, config->has_header, config->filter_pattern,
                                        config->where, &filtered_rows);

    // Print based on format
    if (config->format == OUTPUT_TABLE) {
        const Row* header = config->has_header ? rows[0] : NULL;
        print_pretty_table(filtered_rows, filtered_count, header, col_mapping, visible_cols, config->use_colors,
                           print_arena);
    } else {
        // Other formats
        Arena* scratch = arena_create(0);
        if (!scratch) {
            fprintf(stderr, "Error: Failed to create scratch arena\n");
            arena_destroy(print_arena);
            return;
        }

        // Print header
        print_format_header(config->format, config->has_header, rows, col_count, config->selection, scratch);

        // Print data header for CSV/TSV/Markdown
        if (config->has_header && config->format != OUTPUT_JSON && config->format != OUTPUT_HTML &&
            config->format != OUTPUT_EXCEL) {
            print_row_format(rows[0], col_count, config->format, config->selection, NULL, false, scratch);
            if (config->format == OUTPUT_MARKDOWN) {
                print_markdown_separator(col_count, config->selection);
            }
        }

        // Print data rows
        for (size_t i = 0; i < filtered_count; i++) {
            bool is_last = (i == filtered_count - 1);
            const Row* header = config->has_header ? rows[0] : NULL;
            print_row_format(filtered_rows[i], col_count, config->format, config->selection, header, is_last, scratch);
            arena_reset(scratch);
        }

        // Print footer
        size_t total_data_rows = row_count - (config->has_header ? 1 : 0);
        bool has_filter =
            (config->filter_pattern != NULL && config->filter_pattern[0] != '\0') || (config->where != NULL);
        print_format_footer(config->format, filtered_count, total_data_rows, has_filter);

        arena_destroy(scratch);
    }

    arena_destroy(print_arena);
}

// =============================================================================
// COMMAND-LINE PARSING
// =============================================================================

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

    // Define flags
    flag_bool(parser, "header", 'h', "The CSV file has a header", &has_header);
    flag_bool(parser, "skip-header", 's', "Skip the header", &skip_header);
    flag_bool(parser, "color", 'C', "Use text colors for each column", &use_colors);
    flag_bool(parser, "bgcolor", 'G', "Use background color for rows", &use_bgcolor);
    flag_bool(parser, "desc", 'D', "Sort in descending order", &sort_desc);
    flag_char(parser, "comment", 'c', "Comment Character", &comment);
    flag_string(parser, "delimiter", 'd', "The CSV delimiter (use '\\t' for tab)", &delim_arg);
    flag_string(parser, "hide", 'H', "Comma-separated column indices to hide (e.g., 0,2,5)", &hide_cols);
    flag_string(parser, "filter", 'f', "Show only rows containing this pattern", &filter_pattern);
    flag_string(parser, "where", 'w',
                "Filter rows with condition (e.g., 'age > 25', 'name contains John' or 'age > 25 OR status = active')",
                &where_str);
    flag_string(parser, "select", 'S', "Select and order columns (e.g., 'name,age' or '0,2,1')", &select_str);
    flag_string(parser, "output", 'o', "Output format: table (default), csv, tsv, json, markdown", &format_str);
    flag_string(parser, "sort", 'B', "Sort by column name or index", &sort_col);

    // Parse flags
    if (flag_parse(parser, argc, argv) != FLAG_OK) {
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    if (flag_positional_count(parser) < 1) {
        fprintf(stderr, "Required positional argument <filename> is missing\n");
        flag_print_usage(parser);
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    const char* filename = flag_positional_at(parser, 0);

    // Parse configuration
    char delimiter = parse_delimiter(delim_arg);
    OutputFormat format = parse_output_format(format_str);

    if (skip_header) {
        has_header = false;
    }

    // Parse hidden columns
    if (hide_cols != NULL && parse_hidden_columns(hide_cols) < 0) {
        fprintf(stderr, "Error: Failed to parse hidden columns\n");
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Initialize CSV reader
    CsvReader* reader = csv_reader_new(filename, 0);
    if (reader == NULL) {
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
        sort_rows(rows, count, has_header, sort_col, sort_desc);
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

    // Prepare print configuration
    PrintConfig print_config = {.has_header = has_header,
                                .format = format,
                                .use_colors = use_colors,
                                .use_bgcolor = use_bgcolor,
                                .filter_pattern = filter_pattern,
                                .where = where_ptr,
                                .selection = sel_ptr};

    // Print the table
    print_table(rows, count, &print_config);

    // Cleanup
    csv_reader_free(reader);
    flag_parser_free(parser);
    arena_destroy(arena);

    return EXIT_SUCCESS;
}
