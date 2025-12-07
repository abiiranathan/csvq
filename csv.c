// ===============================================================================
// Command-line tool to pretty-print a CSV table.
// Uses a fast algorithm to compute longest column and longest field per column
// to print ascii table.
//
// ========== Author: Dr. Abiira Nathan ==========================================

#include <ctype.h>             // for isspace
#include <solidc/csvparser.h>  // CSV parsing functions
#include <solidc/flags.h>      // Command-line parser.
#include <stdbool.h>           // for bool
#include <stdio.h>             // for fprintf, printf, stderr
#include <stdlib.h>            // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free, realloc
#include <string.h>            // for strlen, strstr, strcmp

/** Minimum column width for aesthetics. */
#define MIN_COLUMN_WIDTH 3

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
 * Represents a list of column indices to hide.
 */
typedef struct {
    size_t* indices;  // Array of column indices to hide
    size_t count;     // Number of indices
    size_t capacity;  // Allocated capacity
} HiddenColumns;

/**
 * Creates a new HiddenColumns structure.
 * @return Pointer to new HiddenColumns, or NULL on allocation failure.
 */
static HiddenColumns* hidden_columns_new(void) {
    HiddenColumns* hc = malloc(sizeof(*hc));
    if (hc == NULL) {
        return NULL;
    }
    hc->indices  = NULL;
    hc->count    = 0;
    hc->capacity = 0;
    return hc;
}

/**
 * Adds a column index to the hidden columns list.
 * @param hc The HiddenColumns structure.
 * @param index Column index to hide (0-based).
 * @return true on success, false on allocation failure.
 */
static bool hidden_columns_add(HiddenColumns* hc, size_t index) {
    if (hc == NULL) {
        return false;
    }

    // Check if already present
    for (size_t i = 0; i < hc->count; i++) {
        if (hc->indices[i] == index) {
            return true;  // Already hidden, nothing to do
        }
    }

    // Grow array if needed
    if (hc->count >= hc->capacity) {
        size_t new_capacity = hc->capacity == 0 ? 4 : hc->capacity * 2;
        size_t* new_indices = realloc(hc->indices, sizeof(*new_indices) * new_capacity);
        if (new_indices == NULL) {
            return false;
        }
        hc->indices  = new_indices;
        hc->capacity = new_capacity;
    }

    hc->indices[hc->count++] = index;
    return true;
}

/**
 * Checks if a column index is hidden.
 * @param hc The HiddenColumns structure.
 * @param index Column index to check.
 * @return true if the column is hidden, false otherwise.
 */
static bool hidden_columns_contains(const HiddenColumns* hc, size_t index) {
    if (hc == NULL) {
        return false;
    }
    for (size_t i = 0; i < hc->count; i++) {
        if (hc->indices[i] == index) {
            return true;
        }
    }
    return false;
}

/**
 * Frees a HiddenColumns structure.
 * @param hc The HiddenColumns structure to free.
 */
static void hidden_columns_free(HiddenColumns* hc) {
    if (hc == NULL) {
        return;
    }
    free(hc->indices);
    free(hc);
}

/**
 * Parses a comma-separated list of column indices.
 * @param columns_str String like "0,2,5" or "1,3".
 * @return HiddenColumns structure, or NULL on failure. Caller must free.
 */
static HiddenColumns* parse_hidden_columns(const char* columns_str) {
    if (columns_str == NULL || columns_str[0] == '\0') {
        return hidden_columns_new();  // Empty list
    }

    HiddenColumns* hc = hidden_columns_new();
    if (hc == NULL) {
        return NULL;
    }

    // Allocate copy because str_tok mutates the input.
    char* str_copy = strdup(columns_str);
    if (str_copy == NULL) {
        hidden_columns_free(hc);
        return NULL;
    }

    char* token = strtok(str_copy, ",");
    while (token != NULL) {
        // Trim whitespace
        while (isspace((unsigned char)*token)) {
            token++;
        }

        char* end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end)) {
            *end-- = '\0';
        }

        // Parse as integer
        char* endptr;
        long index = strtol(token, &endptr, 10);
        if (*endptr != '\0' || index < 0) {
            fprintf(stderr, "Warning: Invalid column index '%s', skipping\n", token);
        } else {
            if (!hidden_columns_add(hc, (size_t)index)) {
                fprintf(stderr, "Error: Failed to add column index %ld\n", index);
            }
        }

        token = strtok(NULL, ",");
    }

    free(str_copy);
    return hc;
}

/**
 * Checks if a row matches the given filter pattern in a case insensitive way.
 * @param row The row to check.
 * @param pattern The pattern to search for (substring match).
 * @return true if the row contains the pattern in any field, false otherwise.
 */
static bool row_matches_filter(const Row* row, const char* pattern) {
    if (row == NULL || pattern == NULL || pattern[0] == '\0') {
        return true;  // No filter, all rows match
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
 * @param hidden The HiddenColumns structure (can be NULL).
 * @return Dynamically allocated array of column widths. Caller must free.
 *         Returns NULL on allocation failure.
 */
static size_t* compute_column_widths(Row** rows, size_t row_count, size_t col_count,
                                     const HiddenColumns* hidden) {
    if (rows == NULL || row_count == 0 || col_count == 0) {
        return NULL;
    }

    size_t* widths = malloc(sizeof(*widths) * col_count);
    if (widths == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for column widths\n");
        return NULL;
    }

    // Initialize widths to minimum
    for (size_t col = 0; col < col_count; col++) {
        widths[col] = MIN_COLUMN_WIDTH;
    }

    // Scan all rows to find maximum width per column
    for (size_t row = 0; row < row_count; row++) {
        if (rows[row] == NULL || rows[row]->fields == NULL) {
            continue;
        }

        size_t fields_in_row = rows[row]->count;

        for (size_t col = 0; col < col_count && col < fields_in_row; col++) {
            if (hidden_columns_contains(hidden, col)) {
                continue;  // Skip hidden columns
            }

            if (rows[row]->fields[col] != NULL) {
                size_t field_len = strlen(rows[row]->fields[col]);
                if (field_len > widths[col]) {
                    widths[col] = field_len;
                }
            }
        }
    }

    return widths;
}

/**
 * Gets the color code for a given column index.
 * @param col Column index.
 * @param use_colors Whether colors are enabled.
 * @return ANSI color code string, or empty string if colors disabled.
 */
static const char* get_column_color(size_t col, bool use_colors) {
    if (!use_colors) {
        return "";
    }
    // modulo ensures wrap around after NUM_COLORS.
    return COLUMN_COLORS[col % NUM_COLORS];
}

/**
 * Gets the color reset code.
 * @param use_colors Whether colors are enabled.
 * @return ANSI reset code string, or empty string if colors disabled.
 */
static const char* get_color_reset(bool use_colors) {
    if (!use_colors) {
        return "";
    }
    return COLOR_RESET;
}

/**
 * Prints a horizontal separator line for the table.
 * @param widths Array of column widths.
 * @param col_count Number of columns.
 * @param use_colors Whether to use colors.
 * @param hidden The HiddenColumns structure (can be NULL).
 */
static void print_separator(const size_t* widths, size_t col_count, bool use_colors,
                            const HiddenColumns* hidden) {
    if (widths == NULL || col_count == 0) {
        return;
    }

    printf("+");
    for (size_t col = 0; col < col_count; col++) {
        if (hidden_columns_contains(hidden, col)) {
            continue;  // Skip hidden columns
        }

        const char* color = get_column_color(col, use_colors);
        const char* reset = get_color_reset(use_colors);

        printf("%s", color);
        for (size_t i = 0; i < widths[col] + 2; i++) {  // +2 for padding spaces
            printf("-");
        }
        printf("%s+", reset);
    }
    printf("\n");
}

/**
 * Prints a single row of the table with proper padding.
 * @param row The row to print.
 * @param widths Array of column widths.
 * @param col_count Number of columns.
 * @param use_colors Whether to use colors.
 * @param hidden The HiddenColumns structure (can be NULL).
 */
static void print_row(const Row* row, const size_t* widths, size_t col_count, bool use_colors,
                      const HiddenColumns* hidden) {
    if (row == NULL || widths == NULL || col_count == 0) {
        return;
    }

    printf("|");
    for (size_t col = 0; col < col_count; col++) {
        if (hidden_columns_contains(hidden, col)) {
            continue;  // Skip hidden columns
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
            printf(" ");
        }
        printf(" %s|", reset);
    }
    printf("\n");
}

/**
 * Pretty-prints the CSV data as an ASCII table.
 * @param rows Array of Row pointers.
 * @param row_count Number of rows.
 * @param has_header Whether the first row is a header.
 * @param use_colors Whether to use colors for columns.
 * @param hidden The HiddenColumns structure (can be NULL).
 * @param filter_pattern Pattern to filter rows (can be NULL).
 */
static void print_table(Row** rows, size_t row_count, bool has_header, bool use_colors,
                        const HiddenColumns* hidden, const char* filter_pattern) {
    if (rows == NULL || row_count == 0) {
        fprintf(stderr, "Error: No data to print\n");
        return;
    }

    // Determine column count from first row
    size_t col_count = rows[0]->count;
    if (col_count == 0) {
        fprintf(stderr, "Error: No columns in CSV data\n");
        return;
    }

    // Compute column widths
    size_t* widths = compute_column_widths(rows, row_count, col_count, hidden);
    if (widths == NULL) {
        return;
    }

    // Print top border
    print_separator(widths, col_count, use_colors, hidden);

    size_t printed_rows = 0;

    // Print header if present
    if (has_header && row_count > 0) {
        print_row(rows[0], widths, col_count, use_colors, hidden);
        print_separator(widths, col_count, use_colors, hidden);
        printed_rows++;

        // Print data rows (with filtering)
        for (size_t i = 1; i < row_count; i++) {
            if (row_matches_filter(rows[i], filter_pattern)) {
                print_row(rows[i], widths, col_count, use_colors, hidden);
                printed_rows++;
            }
        }
    } else {
        // Print all rows as data (with filtering)
        for (size_t i = 0; i < row_count; i++) {
            if (row_matches_filter(rows[i], filter_pattern)) {
                print_row(rows[i], widths, col_count, use_colors, hidden);
                printed_rows++;
            }
        }
    }

    // Print bottom border
    print_separator(widths, col_count, use_colors, hidden);

    // Report filtered row count if filtering was applied
    if (filter_pattern != NULL && filter_pattern[0] != '\0') {
        size_t filtered_count = printed_rows - (has_header ? 1 : 0);
        printf("\nFiltered: %zu/%zu rows matched pattern '%s'\n", filtered_count,
               row_count - (has_header ? 1 : 0), filter_pattern);
    }

    free(widths);
}

int main(int argc, char* argv[]) {
    int exit_code         = EXIT_FAILURE;
    FlagParser* parser    = NULL;
    HiddenColumns* hidden = NULL;
    CsvReader* reader     = NULL;

    parser = flag_parser_new("csv", "Pretty print CSV to stdout");
    if (!parser) {
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
        goto cleanup_parser;
    }

    int positional_flags = flag_positional_count(parser);
    if (positional_flags < 1) {
        fprintf(stderr, "Required positional argument for the CSV filename\n");
        goto cleanup_parser;
    }

    const char* filename = flag_positional_at(parser, 0);
    if (!filename) {
        fprintf(stderr, "Error: Filename is NULL\n");
        goto cleanup_parser;
    }

    // Allocate dynamic array for hidden columns.
    hidden = parse_hidden_columns(hide_cols);
    if (hidden == NULL) {
        fprintf(stderr, "Error: Failed to parse hidden columns\n");
        goto cleanup_parser;
    }

    // Initialize the CSV reader config struct.
    CsvReaderConfig config = {
        .has_header  = has_header,
        .skip_header = skip_header,
        .comment     = comment,
        .delim       = delimiter,
    };

    // Initialize the CSVReader.
    reader = csv_reader_new(filename);
    if (reader == NULL) {
        fprintf(stderr, "Error: Failed to open CSV file: %s\n", filename);
        goto cleanup_hidden;
    }

    // Apply the configuration before calling parse.
    csv_reader_setconfig(reader, config);

    // Parse the CSV.
    Row** rows = csv_reader_parse(reader);
    if (rows == NULL) {
        fprintf(stderr, "Error: Failed to parse CSV file\n");
        goto cleanup_reader;
    }

    // Get the number of columns. Must be called after parse.
    size_t count = csv_reader_numrows(reader);
    if (count > 0) {
        printf("Number of rows: %zu\n\n", count);

        // Print ascii table.
        print_table(rows, count, has_header, use_colors, hidden, filter_pattern);
    }

    // Mark success before falling through to cleanup
    exit_code = EXIT_SUCCESS;

cleanup_reader:
    csv_reader_free(reader);
cleanup_hidden:
    hidden_columns_free(hidden);
cleanup_parser:
    flag_parser_free(parser);
    return exit_code;
}
