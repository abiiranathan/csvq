// ===============================================================================
// Command-line tool to pretty-print a CSV table.
// Uses a fast algorithm to compute longest column and longest field per column
// to print ascii table.
// Can export data as Markdown, CSV, Table or JSON
//
// Date: 07 December 2025
// Author: Dr. Abiira Nathan
// =================================================================================

#include <ctype.h>             // for isspace
#include <solidc/csvparser.h>  // CSV parsing functions
#include <solidc/flags.h>      // Command-line parser
#include <stdbool.h>           // for bool
#include <stdio.h>             // for fprintf, printf, stderr
#include <stdlib.h>            // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free, calloc
#include <string.h>            // for strlen, strcasestr, strcmp, strdup
#include <strings.h>           // for strcasecmp
#include "str.h"
#include "where-parser.h"

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

/** ANSI background colors for alternating rows (stripped table). */
static const char* BG_COLOR_EVEN = "\033[48;5;236m";  // Dark teal background for even rows
static const char* BG_COLOR_ODD  = "";                // No background for odd rows

/** Output format types. */
typedef enum {
    OUTPUT_TABLE,     // ASCII table (default)
    OUTPUT_CSV,       // CSV format
    OUTPUT_TSV,       // Tab-separated values
    OUTPUT_JSON,      // JSON array of objects
    OUTPUT_MARKDOWN,  // Markdown table
} OutputFormat;

/** Column selection/reordering. */
typedef struct {
    size_t indices[MAX_SELECTED_COLUMNS];  // Column indices in desired order
    size_t count;                          // Number of selected columns
} ColumnSelection;

/**
 * Simple bitset for tracking hidden columns.
 * Much more efficient than dynamic arrays for up to 64 columns.
 */
static unsigned long hidden_columns_mask = 0;

/** Context for the qsort comparison function. */
static struct {
    size_t col_idx;  // Index of column to sort by
    bool desc;       // Sort descending?
    bool active;     // Is sorting active?
} sort_ctx = {0, false, false};

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
 * Finds a column index by name in the header row.
 * @param header The header row.
 * @param name Column name to search for (case-sensitive).
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

    while (token != NULL && selection->count < MAX_SELECTED_COLUMNS) {
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
 * Computes the maximum width needed for each column across all rows.
 * @param rows Array of Row pointers.
 * @param row_count Number of rows.
 * @param col_count Number of columns (fields per row).
 * @param widths Output array for column widths (must be allocated by caller).
 * @param selection Optional column selection (NULL for all columns).
 */
static void compute_column_widths(Row** rows, size_t row_count, size_t col_count, size_t* widths,
                                  const ColumnSelection* selection) {
    size_t num_cols = selection != NULL ? selection->count : col_count;

    // Initialize widths to minimum
    for (size_t i = 0; i < num_cols; i++) {
        widths[i] = MIN_COLUMN_WIDTH;
    }

    // Scan all rows to find maximum width per column
    for (size_t row = 0; row < row_count; row++) {
        const Row* r = rows[row];
        if (r == NULL) {
            continue;
        }

        for (size_t i = 0; i < num_cols; i++) {
            size_t col = selection != NULL ? selection->indices[i] : i;

            // If a column is specifically selected, we ignore the 'hide' flag.
            // Only skip calculation if no selection is active and column is hidden.
            if ((selection == NULL && is_column_hidden(col)) || col >= r->count) {
                continue;
            }

            const char* field = r->fields[col];
            if (field != NULL) {
                size_t field_len = strlen(field);
                if (field_len > widths[i]) {
                    widths[i] = field_len;
                }
            }
        }
    }
}

/**
 * Gets the background color code for a given row (alternating pattern).
 * @param row_index Row index (0-based, for alternating pattern).
 * @param use_colors Whether colors are enabled.
 * @return ANSI background color code string, or empty string if colors disabled.
 */
static inline const char* get_row_bg_color(size_t row_index, bool use_bgcolor) {
    if (!use_bgcolor) {
        return "";
    }
    return (row_index % 2 == 0) ? BG_COLOR_EVEN : BG_COLOR_ODD;
}

/**
 * Gets the color code for a given column index.
 * @param col Column index.
 * @param use_colors Whether colors are enabled.
 * @return ANSI color code string, or empty string if colors disabled.
 */
static inline const char* get_column_color(size_t col, bool use_colors) {
    return use_colors ? COLUMN_COLORS[col % NUM_COLORS] : "";
}

/**
 * Gets the color reset code.
 * @param use_colors Whether colors are enabled.
 * @return ANSI reset code string, or empty string if colors disabled.
 */
static inline const char* get_color_reset(bool use_colors) {
    return use_colors ? COLOR_RESET : "";
}

/**
 * Prints a horizontal separator line for the table.
 * @param widths Array of column widths.
 * @param col_count Number of columns.
 * @param use_colors Whether to use colors.
 * @param selection Optional column selection (NULL for all columns).
 */
static void print_separator(const size_t* widths, size_t col_count, bool use_colors, const ColumnSelection* selection) {
    putchar('+');
    for (size_t i = 0; i < col_count; i++) {
        size_t col = selection != NULL ? selection->indices[i] : i;

        // Skip hidden columns if no explicit selection
        if (selection == NULL && is_column_hidden(col)) {
            continue;
        }

        const char* color = get_column_color(i, use_colors);
        const char* reset = get_color_reset(use_colors);

        fputs(color, stdout);
        for (size_t k = 0; k < widths[i] + 2; k++) {
            putchar('-');
        }
        fputs(reset, stdout);
        putchar('+');
    }
    putchar('\n');
}

/**
 * Trims whitespace and escapes a string for JSON output.
 * @param str The string to process.
 * @return Allocated string with whitespace trimmed and special chars escaped. Caller must free.
 */
static char* trim_and_escape_json(const char* s) {
    if (s == NULL) {
        return strdup("");
    }

    char* str = (char*)s;
    str       = trim_string(str);

    // Get length of trimmed string.
    size_t len = strlen(str);

    // Allocate memory (Worst case: every char needs escaping \x -> \\x)
    char* escaped = malloc(len * 2 + 1);
    if (escaped == NULL) {
        return NULL;
    }

    // Escape characters within the trimmed range
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
 * Prints a field for ASCII table output, replacing control characters
 * (tabs, newlines, returns) with spaces to maintain alignment.
 */
static void print_sanitized_field(const char* str) {
    if (str == NULL) return;

    for (const char* p = str; *p; p++) {
        // Replace Tab, Newline, or Carriage Return with a generic space
        if (*p == '\t' || *p == '\n' || *p == '\r') {
            putchar(' ');
        } else {
            putchar(*p);
        }
    }
}

/**
 * Prints a single row in the specified output format.
 * @param row The row to print.
 * @param widths Array of column widths (for table format).
 * @param col_count Number of columns to print.
 * @param format Output format.
 * @param use_colors Whether to use colors (table format only).
 * @param selection Optional column selection (NULL for all columns).
 * @param header Optional header row for JSON field names (NULL if not JSON).
 * @param is_last_row Whether this is the last row (for JSON formatting).
 * @param row_index Row index for background coloring (0-based).
 */
static void print_row_format(const Row* row, const size_t* widths, size_t col_count, OutputFormat format,
                             bool use_colors, bool use_bgcolor, const ColumnSelection* selection, const Row* header,
                             bool is_last_row, size_t row_index) {
    if (row == NULL) {
        return;
    }

    switch (format) {
        case OUTPUT_TABLE: {
            const char* bg_color = get_row_bg_color(row_index, use_bgcolor);
            putchar('|');
            for (size_t i = 0; i < col_count; i++) {
                size_t col = selection != NULL ? selection->indices[i] : i;

                // Skip hidden columns if no explicit selection
                if (selection == NULL && is_column_hidden(col)) {
                    continue;
                }

                const char* field = "";
                if (col < row->count && row->fields[col] != NULL) {
                    field = row->fields[col];
                }

                size_t field_len = strlen(field);

                size_t padding = 0;
                if (widths[i] > field_len) {
                    padding = widths[i] - field_len;
                }

                const char* color = get_column_color(i, use_colors);
                const char* reset = get_color_reset(use_colors);

                // Print background color, then column color
                fputs(bg_color, stdout);
                fputs(color, stdout);

                // Print content space-by-space.
                // This ensures a '\t' inside data is printed as ' ' (1 char),
                // matching the logic of strlen used to calculate width.
                putchar(' ');
                print_sanitized_field(field);

                // Print padding
                for (size_t j = 0; j < padding; j++) {
                    putchar(' ');
                }

                // Print closing space and reset color
                putchar(' ');
                fputs(reset, stdout);
                putchar('|');
            }
            putchar('\n');
            break;
        }

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

                // Clean and escape both the Key (header) and the Value (field)
                char* escaped_key = trim_and_escape_json(field_name);
                char* escaped_val = trim_and_escape_json(field);

                printf("\"%s\": \"%s\"", escaped_key != NULL ? escaped_key : "",
                       escaped_val != NULL ? escaped_val : "");

                free(escaped_key);
                free(escaped_val);

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
    }
}

/**
 * Prints markdown table separator after header.
 * @param col_count Number of columns.
 * @param selection Optional column selection (NULL for all columns).
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

/**
 * Pretty-prints the CSV data in the specified format.
 * @param rows Array of Row pointers.
 * @param row_count Number of rows.
 * @param has_header Whether the first row is a header.
 * @param format Output format.
 * @param use_colors Whether to use colors for columns (table format only).
 * @param filter_pattern Pattern to filter rows (can be NULL).
 * @param where Where clause filter (can be NULL).
 * @param selection Optional column selection (NULL for all columns).
 */
static void print_table(Row** rows, size_t row_count, bool has_header, OutputFormat format, bool use_colors,
                        bool use_bgcolor, const char* filter_pattern, WhereFilter* where,
                        const ColumnSelection* selection) {
    if (row_count == 0 || rows[0]->count == 0) {
        fprintf(stderr, "Error: No data to print\n");
        return;
    }

    size_t original_col_count = rows[0]->count;
    size_t col_count          = selection != NULL ? selection->count : original_col_count;

    // Resolve where clause column indices if present
    if (where != NULL && has_header) {
        resolve_ast_indices(where->root, rows[0]);
    }

    size_t* widths = NULL;
    if (format == OUTPUT_TABLE) {
        widths = calloc(col_count, sizeof(*widths));
        if (widths == NULL) {
            fprintf(stderr, "Error: Failed to allocate memory for column widths\n");
            return;
        }
        compute_column_widths(rows, row_count, original_col_count, widths, selection);
    }

    // Print format-specific header
    if (format == OUTPUT_TABLE) {
        print_separator(widths, col_count, use_colors, selection);
    } else if (format == OUTPUT_JSON) {
        printf("[\n");
    }

    size_t printed_rows = 0;
    size_t start_row    = 0;
    const Row* header   = NULL;

    // Print header if present
    if (has_header && row_count > 0) {
        header = rows[0];

        // Skip printing the header row itself for JSON output.
        // For JSON, the header is only used to generate keys for data rows.
        if (format != OUTPUT_JSON) {
            print_row_format(rows[0], widths, col_count, format, use_colors, use_bgcolor, selection, NULL, false, 0);
            if (format == OUTPUT_TABLE) {
                print_separator(widths, col_count, use_colors, selection);
            } else if (format == OUTPUT_MARKDOWN) {
                print_markdown_separator(col_count, selection);
            }
            printed_rows++;
        }

        start_row = 1;
    }

    // Print data rows (with filtering)
    size_t data_row_index = 0;  // Track actual data row index for background coloring
    for (size_t i = start_row; i < row_count; i++) {
        bool matches = row_matches_filter(rows[i], filter_pattern);
        if (matches && where != NULL) {
            matches = evaluate_where_filter(rows[i], where);
        }

        if (matches) {
            bool is_last = (i == row_count - 1);

            // For JSON, need to check if there are more matching rows
            if (format == OUTPUT_JSON && !is_last) {
                bool future_match_found = false;

                for (size_t j = i + 1; j < row_count; j++) {
                    bool next_matches = row_matches_filter(rows[j], filter_pattern);
                    if (next_matches && where != NULL) {
                        next_matches = evaluate_where_filter(rows[j], where);
                    }

                    if (next_matches) {
                        future_match_found = true;
                        break;
                    }
                }

                // If no future match was found, this is the last row to be printed.
                if (!future_match_found) {
                    is_last = true;
                }
            }

            print_row_format(rows[i], widths, col_count, format, use_colors, use_bgcolor, selection, header, is_last,
                             data_row_index);
            printed_rows++;
            data_row_index++;
        }
    }

    // Print format-specific footer
    if (format == OUTPUT_TABLE) {
        print_separator(widths, col_count, use_colors, selection);
    } else if (format == OUTPUT_JSON) {
        printf("]\n");
    }

    // Report filtered row count if filtering was applied
    if ((filter_pattern != NULL && filter_pattern[0] != '\0') || where != NULL) {
        size_t data_rows       = printed_rows - (has_header ? 1 : 0);
        size_t total_data_rows = row_count - (has_header ? 1 : 0);

        if (format == OUTPUT_TABLE || format == OUTPUT_MARKDOWN) {
            printf("\nFiltered: %zu/%zu rows matched\n", data_rows, total_data_rows);
        }
    }

    free(widths);
}

int main(int argc, char* argv[]) {
    FlagParser* parser = flag_parser_new("csvq", "Query and format CSV files");
    if (parser == NULL) {
        return EXIT_FAILURE;
    }

    bool has_header      = true;
    bool skip_header     = false;
    bool use_colors      = false;
    bool use_bgcolor     = false;
    char comment         = '#';
    char* delim_arg      = ",";
    char* hide_cols      = NULL;
    char* filter_pattern = NULL;
    char* where_str      = NULL;
    char* select_str     = NULL;
    char* format_str     = NULL;
    bool sort_desc       = false;
    char* sort_col       = NULL;
    size_t max_memory    = 2 << 20;  // 2 MB

    flag_size_t(parser, "memory", 'm', "Maximum memory in bytes to use", &max_memory);
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

    if (flag_parse(parser, argc, argv) != FLAG_OK) {
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    if (flag_positional_count(parser) < 1) {
        fprintf(stderr, "Required positional argument <filename> is missing\n");
        flag_print_usage(parser);
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    char delimiter = ',';  // Default is comma (csv)

    // Conver char * to char because it painful to pass \t via command-line.
    if (delim_arg != NULL) {
        if (strcmp(delim_arg, "\\t") == 0) {
            delimiter = '\t';
        } else {
            // Take the first character (handles literal tabs or custom chars like ';')
            delimiter = delim_arg[0];
        }
    }

    // If skip-header is requested, the first parsed row is actually data.
    // We shouldn't treat it as a header for selection or printing logic.
    if (skip_header) {
        has_header = false;
    }

    const char* filename = flag_positional_at(parser, 0);

    // Parse hidden columns into bitset
    if (hide_cols != NULL && parse_hidden_columns(hide_cols) < 0) {
        fprintf(stderr, "Error: Failed to parse hidden columns\n");
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    // Parse output format
    OutputFormat format = OUTPUT_TABLE;
    if (format_str != NULL) {
        if (strcasecmp(format_str, "csv") == 0) {
            format = OUTPUT_CSV;
        } else if (strcasecmp(format_str, "tsv") == 0) {
            format = OUTPUT_TSV;
        } else if (strcasecmp(format_str, "json") == 0) {
            format = OUTPUT_JSON;
        } else if (strcasecmp(format_str, "markdown") == 0 || strcasecmp(format_str, "md") == 0) {
            format = OUTPUT_MARKDOWN;
        } else if (strcasecmp(format_str, "table") != 0) {
            fprintf(stderr, "Warning: Unknown format '%s', using table\n", format_str);
        }
    }

    // Initialize the CSV reader
    CsvReader* reader = csv_reader_new(filename, max_memory);
    if (reader == NULL) {
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    // Configure and parse
    CsvReaderConfig config = {
        .has_header  = has_header,
        .skip_header = skip_header,
        .comment     = comment,
        .delim       = delimiter,
    };

    csv_reader_setconfig(reader, config);

    Row** rows = csv_reader_parse(reader);
    if (rows == NULL) {
        fprintf(stderr, "Error: Failed to parse CSV file, likely due to invalid delimiter. ");
        fprintf(stderr, "Use --delimiter='\\\\t' for Tab-seperated Value file\n");
        csv_reader_free(reader);
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    size_t count = csv_reader_numrows(reader);
    if (count == 0) {
        fprintf(stderr, "Error: No rows in CSV file\n");
        csv_reader_free(reader);
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    // Sort data is column is specified.
    if (sort_col != NULL && count > 0) {
        long idx = -1;
        char* endptr;
        long parsed_idx = strtol(sort_col, &endptr, 10);

        // Determine Index
        if (*endptr == '\0' && parsed_idx >= 0) {
            idx = parsed_idx;
        } else if (has_header) {
            // Reusing existing find_column_by_name helper
            ssize_t found = find_column_by_name(rows[0], sort_col);
            if (found >= 0) idx = found;
        }

        if (idx >= 0) {
            sort_ctx.col_idx = (size_t)idx;
            sort_ctx.desc    = sort_desc;
            sort_ctx.active  = true;

            // If we have a header, we must NOT sort row[0].
            // We shift the array pointer by 1 and decrease count by 1.
            size_t sort_start_offset = (has_header) ? 1 : 0;
            size_t sort_count        = (count > sort_start_offset) ? count - sort_start_offset : 0;

            if (sort_count > 1) {
                qsort(rows + sort_start_offset, sort_count, sizeof(Row*), compare_rows);
            }
        } else {
            fprintf(stderr, "Warning: Could not resolve sort column '%s'. Sorting skipped.\n", sort_col);
        }
    }

    // Parse column selection if provided
    ColumnSelection selection = {0};
    ColumnSelection* sel_ptr  = NULL;
    if (select_str != NULL) {
        const Row* header = has_header ? rows[0] : NULL;
        if (parse_column_selection(select_str, header, &selection)) {
            sel_ptr = &selection;
        }
    }

    // Parse where clause if provided
    WhereFilter where      = {0};
    WhereFilter* where_ptr = NULL;

    if (where_str != NULL) {
        if (parse_where_clause(where_str, &where)) {
            where_ptr = &where;
        }
    }

    print_table(rows, count, has_header, format, use_colors, use_bgcolor, filter_pattern, where_ptr, sel_ptr);
    where_filter_free(where_ptr);
    csv_reader_free(reader);
    flag_parser_free(parser);
    return EXIT_SUCCESS;
}
