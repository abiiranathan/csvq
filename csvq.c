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
#include <solidc/arena.h>      // Arena allocator
#include <solidc/prettytable.h> // Table printer
#include <stdbool.h>           // for bool
#include <stdio.h>             // for fprintf, printf, stderr
#include <stdlib.h>            // for EXIT_FAILURE, EXIT_SUCCESS, malloc, free, calloc
#include <string.h>            // for strlen, strcasestr, strcmp, strdup
#include <strings.h>           // for strcasecmp
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

    // Get length of trimmed string.
    size_t len = strlen(str);

    // Allocate memory (Worst case: every char needs escaping \x -> \\x)
    char* escaped = arena_alloc(arena, len * 2 + 1);
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

// -----------------------------------------------------------------------------
// PrettyTable Integration
// -----------------------------------------------------------------------------

typedef struct {
    Row** rows;
    size_t* col_mapping;
    bool use_colors;
    Arena* arena; // For allocating colored strings
    const Row* header; // Original header row for get_header
} TableContext;

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
    while(isspace(*s)) s++;
    
    return s;
}

/**
 * Callback to get cell value.
 */
static const char* get_cell_cb(void* user_data, int row, int col) {
    TableContext* ctx = (TableContext*)user_data;
    Row* r = ctx->rows[row];
    size_t actual_col = ctx->col_mapping[col];
    
    const char* val = "";
    if (actual_col < r->count && r->fields[actual_col] != NULL) {
        val = r->fields[actual_col];
    }
    
    // Sanitize control characters (tabs, newlines) for display
    // Prettytable might handle it, but better safe.
    // Also handle coloring here.
    
    if (ctx->use_colors) {
        const char* color = COLUMN_COLORS[(size_t)col % NUM_COLORS];
        const char* reset = COLOR_RESET;
        
        // Sanitize first into a temp buffer (or assume clean for now)
        // We need to calculate length.
        size_t len = strlen(val);
        size_t color_len = strlen(color);
        size_t reset_len = strlen(reset);
        
        char* buf = arena_alloc(ctx->arena, len + color_len + reset_len + 1);
        if (!buf) return "";
        
        strcpy(buf, color);
        // Copy val but replace control chars
        char* p = buf + color_len;
        for (const char* v = val; *v; v++) {
            if (*v == '\t' || *v == '\n' || *v == '\r') {
                *p++ = ' ';
            } else {
                *p++ = *v;
            }
        }
        strcpy(p, reset);
        return buf;
    }
    
    // Non-colored path: still need to sanitize? 
    // prettytable handles raw strings. But newlines break tables.
    // Let's sanitize into arena if special chars found.
    bool dirty = false;
    for (const char* v = val; *v; v++) {
        if (*v == '\t' || *v == '\n' || *v == '\r') {
            dirty = true;
            break;
        }
    }
    
    if (dirty) {
        size_t len = strlen(val);
        char* buf = arena_alloc(ctx->arena, len + 1);
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
    
    return val;
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
            // Handle utf-8? Assuming 1 byte = 1 char for now as per original code
            len++;
            p++;
        }
    }
    return len;
}


static void print_row_format(const Row* row, size_t col_count, OutputFormat format,
                             const ColumnSelection* selection, const Row* header,
                             bool is_last_row, Arena* scratch) {
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

                // Clean and escape both the Key (header) and the Value (field)
                // Use scratch arena
                char* escaped_key = trim_and_escape_json(scratch, field_name);
                char* escaped_val = trim_and_escape_json(scratch, field);

                printf("\"%s\": \"%s\"", escaped_key != NULL ? escaped_key : "",
                       escaped_val != NULL ? escaped_val : "");

                // No need to free, arena reset handles it

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

        default:
            break;
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
    (void)use_bgcolor; // Unused since we switched to prettytable
    
    if (row_count == 0 || rows[0]->count == 0) {
        fprintf(stderr, "Error: No data to print\n");
        return;
    }

    size_t original_col_count = rows[0]->count;
    size_t col_count = selection != NULL ? selection->count : original_col_count;

    // Resolve where clause column indices if present
    if (where != NULL && has_header) {
        resolve_ast_indices(where->root, rows[0]);
    }

    // Pre-calculate visible columns mapping
    // This maps virtual index i (0..N-1) to actual CSV column index.
    // If selection is present, use it. Else skip hidden columns.
    
    // We allocate this in a local arena scope or scratch arena?
    // Let's create a temporary arena for this function's logic
    Arena* print_arena = arena_create(0);
    if (!print_arena) {
        fprintf(stderr, "Error: Failed to create print arena\n");
        return;
    }

    size_t* col_mapping = NULL;
    int visible_cols = 0;

    if (selection != NULL) {
        visible_cols = selection->count;
        col_mapping = ARENA_ALLOC_ARRAY(print_arena, size_t, (size_t)visible_cols);
        for(int i=0; i<visible_cols; i++) {
            col_mapping[i] = selection->indices[i];
        }
    } else {
        // Count visible
        for(size_t i=0; i<original_col_count; i++) {
            if (!is_column_hidden(i)) visible_cols++;
        }
        col_mapping = ARENA_ALLOC_ARRAY(print_arena, size_t, (size_t)visible_cols);
        int idx = 0;
        for(size_t i=0; i<original_col_count; i++) {
            if (!is_column_hidden(i)) {
                col_mapping[idx++] = i;
            }
        }
    }

    // Filter rows into a list
    size_t start_row = (has_header && row_count > 0) ? 1 : 0;
    size_t data_row_capacity = row_count; // Upper bound
    Row** filtered_rows = ARENA_ALLOC_ARRAY(print_arena, Row*, data_row_capacity);
    size_t filtered_count = 0;

    for (size_t i = start_row; i < row_count; i++) {
        bool matches = row_matches_filter(rows[i], filter_pattern);
        if (matches && where != NULL) {
            matches = evaluate_where_filter(rows[i], where);
        }
        if (matches) {
            filtered_rows[filtered_count++] = rows[i];
        }
    }

    // Handle Table using PrettyTable
    if (format == OUTPUT_TABLE) {
        TableContext ctx = {
            .rows = filtered_rows,
            .col_mapping = col_mapping,
            .use_colors = use_colors,
            .arena = print_arena,
            .header = (has_header) ? rows[0] : NULL
        };
        
        prettytable_config cfg;
        prettytable_config_init(&cfg);
        cfg.num_rows = filtered_count;
        cfg.num_cols = visible_cols;
        cfg.get_header = get_header_cb;
        cfg.get_cell = get_cell_cb;
        cfg.get_length = get_length_cb;
        cfg.user_data = &ctx;
        cfg.show_header = has_header;
        cfg.style = &PRETTYTABLE_STYLE_BOX;
        cfg.show_row_count = true;
        
        prettytable_print(&cfg);

    } else {
        // Fallback for CSV, TSV, JSON, MARKDOWN (using existing logic but looping over filtered_rows)
        // Note: filtered_rows contains the pointers to the original rows.
        
        Arena* scratch = arena_create(0);
        
        if (format == OUTPUT_JSON) {
            printf("[\n");
        }
        
        // Print header for Markdown/CSV/TSV if needed
        if (has_header && format != OUTPUT_JSON) {
             print_row_format(rows[0], col_count, format, selection, NULL, false, scratch);
             if (format == OUTPUT_MARKDOWN) {
                 print_markdown_separator(col_count, selection);
             }
        }
        
        for (size_t i = 0; i < filtered_count; i++) {
            bool is_last = (i == filtered_count - 1);
            // For Markdown/CSV/TSV, we don't need the header row argument in print_row_format usually,
            // but JSON needs it for keys.
            print_row_format(filtered_rows[i], col_count, format, selection, has_header ? rows[0] : NULL, is_last, scratch);
            if (scratch) arena_reset(scratch);
        }
        
        if (format == OUTPUT_JSON) {
            printf("]\n");
        }
        
        // Report filtered count for Markdown
        if (format == OUTPUT_MARKDOWN && ((filter_pattern != NULL && filter_pattern[0] != '\0') || where != NULL)) {
             size_t total_data_rows = row_count - (has_header ? 1 : 0);
             printf("\nFiltered: %zu/%zu rows matched\n", filtered_count, total_data_rows);
        }
        
        if (scratch) arena_destroy(scratch);
    }
    
    arena_destroy(print_arena);
}

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
        arena_destroy(arena);
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

    // Initialize the CSV reader with default arena memory.
    CsvReader* reader = csv_reader_new(filename, 0);
    if (reader == NULL) {
        flag_parser_free(parser);
        arena_destroy(arena);
        return EXIT_FAILURE;
    }

    // Configure and parse
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
        fprintf(stderr, "Use --delimiter='\\\\t' for Tab-seperated Value file\n");
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
            sort_ctx.desc = sort_desc;
            sort_ctx.active = true;

            // If we have a header, we must NOT sort row[0].
            // We shift the array pointer by 1 and decrease count by 1.
            size_t sort_start_offset = (has_header) ? 1 : 0;
            size_t sort_count = (count > sort_start_offset) ? count - sort_start_offset : 0;

            if (sort_count > 1) {
                qsort(rows + sort_start_offset, sort_count, sizeof(Row*), compare_rows);
            }
        } else {
            fprintf(stderr, "Warning: Could not resolve sort column '%s'. Sorting skipped.\n", sort_col);
        }
    }

    // Parse column selection if provided
    ColumnSelection selection = {0};
    ColumnSelection* sel_ptr = NULL;
    if (select_str != NULL) {
        const Row* header = has_header ? rows[0] : NULL;
        if (parse_column_selection(select_str, header, &selection)) {
            sel_ptr = &selection;
        }
    }

    // Parse where clause if provided
    WhereFilter where = {0};
    WhereFilter* where_ptr = NULL;

    if (where_str != NULL) {
        if (parse_where_clause(arena, where_str, &where)) {
            where_ptr = &where;
        }
    }

    print_table(rows, count, has_header, format, use_colors, use_bgcolor, filter_pattern, where_ptr, sel_ptr);
    
    // No need to free where_filter, arena handles it
    
    csv_reader_free(reader);
    flag_parser_free(parser);
    arena_destroy(arena);
    return EXIT_SUCCESS;
}
