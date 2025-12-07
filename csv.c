// ===============================================================================
// Command-line tool to pretty-print a CSV table.
// Uses a fast algorithm to compute longest column and longest field per column
// to print ascii table.
//
// ========== Author: Dr. Abiira Nathan ==========================================

#include <ctype.h>             // for isspace
#include <solidc/csvparser.h>  // CSV parsing functions
#include <solidc/flags.h>      // Command-line parser
#include <stdbool.h>           // for bool
#include <stdio.h>             // for fprintf, printf, stderr
#include <stdlib.h>            // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free
#include <string.h>            // for strlen, strcasestr, strcmp
#include <strings.h>           // for strcasecmp

/** Minimum column width for aesthetics. */
#define MIN_COLUMN_WIDTH 3

/** Maximum number of columns we support hiding. */
#define MAX_HIDDEN_COLUMNS 64

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

/**
 * Simple bitset for tracking hidden columns.
 * Much more efficient than dynamic arrays for up to 64 columns.
 */
static unsigned long hidden_columns_mask = 0;

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
 */
static void compute_column_widths(Row** rows, size_t row_count, size_t col_count, size_t* widths) {
    // Initialize widths to minimum
    for (size_t col = 0; col < col_count; col++) {
        widths[col] = MIN_COLUMN_WIDTH;
    }

    // Scan all rows to find maximum width per column
    for (size_t row = 0; row < row_count; row++) {
        const Row* r = rows[row];

        // Should not happen, but better safe than sorry!
        if (r == NULL) {
            continue;
        }

        for (size_t col = 0; col < col_count && col < r->count; col++) {
            if (is_column_hidden(col)) {
                continue;
            }

            const char* field = r->fields[col];

            // Also field should not be NULL, but we are being paranoid!
            if (field != NULL) {
                size_t field_len = strlen(field);
                if (field_len > widths[col]) {
                    widths[col] = field_len;
                }
            }
        }
    }
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
 */
static void print_separator(const size_t* widths, size_t col_count, bool use_colors) {
    putchar('+');
    for (size_t col = 0; col < col_count; col++) {
        if (is_column_hidden(col)) {
            continue;
        }

        const char* color = get_column_color(col, use_colors);
        const char* reset = get_color_reset(use_colors);

        fputs(color, stdout);
        for (size_t i = 0; i < widths[col] + 2; i++) {
            putchar('-');
        }
        fputs(reset, stdout);
        putchar('+');
    }
    putchar('\n');
}

/**
 * Prints a single row of the table with proper padding.
 * @param row The row to print.
 * @param widths Array of column widths.
 * @param col_count Number of columns.
 * @param use_colors Whether to use colors.
 */
static void print_row(const Row* row, const size_t* widths, size_t col_count, bool use_colors) {
    putchar('|');
    for (size_t col = 0; col < col_count; col++) {
        if (is_column_hidden(col)) {
            continue;
        }

        const char* field = "";
        if (col < row->count && row->fields[col] != NULL) {
            field = row->fields[col];
        }

        size_t field_len = strlen(field);
        size_t padding   = widths[col] - field_len;

        const char* color = get_column_color(col, use_colors);
        const char* reset = get_color_reset(use_colors);

        // Print with left-aligned padding: " field   "
        printf("%s %s", color, field);
        for (size_t i = 0; i < padding; i++) {
            putchar(' ');
        }
        printf(" %s|", reset);
    }
    putchar('\n');
}

/**
 * Pretty-prints the CSV data as an ASCII table.
 * @param rows Array of Row pointers.
 * @param row_count Number of rows.
 * @param has_header Whether the first row is a header.
 * @param use_colors Whether to use colors for columns.
 * @param filter_pattern Pattern to filter rows (can be NULL).
 */
static void print_table(Row** rows, size_t row_count, bool has_header, bool use_colors,
                        const char* filter_pattern) {
    if (row_count == 0 || rows[0]->count == 0) {
        return;
    }

    size_t col_count = rows[0]->count;
    size_t* widths   = calloc(col_count, sizeof(size_t));
    if (!widths) {
        perror("calloc");
        return;
    }

    compute_column_widths(rows, row_count, col_count, widths);

    // Print top border
    print_separator(widths, col_count, use_colors);

    size_t printed_rows = 0;
    size_t start_row    = 0;

    // Print header if present
    if (has_header && row_count > 0) {
        print_row(rows[0], widths, col_count, use_colors);
        print_separator(widths, col_count, use_colors);
        printed_rows++;
        start_row = 1;
    }

    // Print data rows (with filtering)
    for (size_t i = start_row; i < row_count; i++) {
        if (row_matches_filter(rows[i], filter_pattern)) {
            print_row(rows[i], widths, col_count, use_colors);
            printed_rows++;
        }
    }

    // Print bottom border
    print_separator(widths, col_count, use_colors);

    // Report filtered row count if filtering was applied
    if (filter_pattern != NULL && filter_pattern[0] != '\0') {
        size_t data_rows       = printed_rows - (has_header ? 1 : 0);
        size_t total_data_rows = row_count - (has_header ? 1 : 0);
        printf("\nFiltered: %zu/%zu rows matched pattern '%s'\n", data_rows, total_data_rows,
               filter_pattern);
    }

    // Free dynamically allocated array of widths.
    free(widths);
}

int main(int argc, char* argv[]) {
    FlagParser* parser = flag_parser_new("csv", "Pretty print CSV to stdout");
    if (parser == NULL) {
        return EXIT_FAILURE;
    }

    bool has_header      = true;
    bool skip_header     = false;
    bool use_colors      = false;
    char comment         = '#';
    char delimiter       = ',';
    char* hide_cols      = NULL;
    char* filter_pattern = NULL;

    flag_bool(parser, "header", 'h', "The CSV file has a header", &has_header);
    flag_bool(parser, "skip-header", 's', "Skip the header", &skip_header);
    flag_bool(parser, "color", 'C', "Use colors for each column", &use_colors);
    flag_char(parser, "comment", 'c', "Comment Character", &comment);
    flag_char(parser, "delimiter", 'd', "The CSV delimiter", &delimiter);
    flag_string(parser, "hide", 'H', "Comma-separated column indices to hide (e.g., 0,2,5)",
                &hide_cols);
    flag_string(parser, "filter", 'f', "Show only rows containing this pattern", &filter_pattern);

    if (flag_parse(parser, argc, argv) != FLAG_OK) {
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    if (flag_positional_count(parser) < 1) {
        fprintf(stderr, "Error: Missing required CSV filename\n");
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    const char* filename = flag_positional_at(parser, 0);

    // Parse hidden columns into bitset
    if (hide_cols != NULL && parse_hidden_columns(hide_cols) < 0) {
        fprintf(stderr, "Error: Failed to parse hidden columns\n");
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    // Initialize the CSV reader
    CsvReader* reader = csv_reader_new(filename);
    if (reader == NULL) {
        fprintf(stderr, "Error: Failed to open CSV file: %s\n", filename);
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
        fprintf(stderr, "Error: Failed to parse CSV file\n");
        csv_reader_free(reader);
        flag_parser_free(parser);
        return EXIT_FAILURE;
    }

    size_t count = csv_reader_numrows(reader);
    if (count > 0) {
        printf("Number of rows: %zu\n\n", count);
        print_table(rows, count, has_header, use_colors, filter_pattern);
    }

    csv_reader_free(reader);
    flag_parser_free(parser);
    return EXIT_SUCCESS;
}
