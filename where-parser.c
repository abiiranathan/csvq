#include "where-parser.h"
#include <ctype.h>
#include <solidc/cstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations for the parser
static ASTNode* parse_expression(Arena* arena, char** stream);

/**
 * Finds a column index by name in the header row.
 * (defined in csvq.c)
 * @param header The header row.
 * @param name Column name to search for (case-sensitive).
 * @return Column index, or -1 if not found.
 */
extern ssize_t find_column_by_name(const Row* header, const char* name);

/**
 * Helper to parse a single raw condition string (e.g. "age > 25") into a WhereClause struct.
 * This reuses the logic from your original linear parser but applies it to a leaf node.
 *
 * Operators are now checked from longest to shortest to avoid partial matches.
 * For example, ">=" must be checked before ">" to prevent "age>=25" from matching ">".
 */
static WhereClause* parse_single_condition(Arena* arena, char* cond_str) {
    // Trim input
    cond_str = trim_string(cond_str);
    if (!cond_str || !*cond_str) return NULL;

    // Check operators from longest to shortest to avoid partial matches
    // Old order: ">=", "<=", "!=", "contains", ">", "<", "="
    // Problem: If checking ">" before ">=", then "age>=25" would match ">" first
    //
    // New order groups by length and checks longest first:
    // - Length 8: "contains"
    // - Length 2: ">=", "<=", "!="
    // - Length 1: ">", "<", "="
    const char* operators[] = {
        "contains",  // Length 8 - must be first
        ">=",        // Length 2
        "<=",        // Length 2
        "!=",        // Length 2
        ">",         // Length 1
        "<",         // Length 1
        "="          // Length 1
    };

    CompareOp ops[] = {OP_CONTAINS, OP_GREATER_EQ, OP_LESS_EQ, OP_NOT_EQUALS, OP_GREATER, OP_LESS, OP_EQUALS};

    size_t num_ops = sizeof(operators) / sizeof(operators[0]);

    char* op_pos = NULL;
    CompareOp found_op = OP_EQUALS;
    size_t op_len = 0;

    // Find operator - now checking longest first
    for (size_t i = 0; i < num_ops; i++) {
        char* pos = strcasestr(cond_str, operators[i]);
        if (pos != NULL) {
            op_pos = pos;
            found_op = ops[i];
            op_len = strlen(operators[i]);
            break;  // Safe to break now because we check longest first
        }
    }

    if (op_pos == NULL) {
        fprintf(stderr, "Error: No valid operator in clause: '%s'\n", cond_str);
        return NULL;
    }

    // Split string at operator
    *op_pos = '\0';
    char* col_name = cond_str;
    char* value = op_pos + op_len;

    col_name = trim_string(col_name);
    value = trim_string(value);

    // Value can be empty string in some cases, but col cannot.
    if (!col_name || !*col_name || !value) {
        fprintf(stderr, "Error: Invalid condition - column='%s', value='%s'\n", col_name ? col_name : "NULL",
                value ? value : "NULL");
        return NULL;
    }

    WhereClause* wc = ARENA_ALLOC_ZERO(arena, WhereClause);
    if (!wc) {
        fprintf(stderr, "Error: Memory allocation failed for WhereClause\n");
        return NULL;
    }

    wc->column_name = arena_strdup(arena, col_name);
    if (!wc->column_name) {
        fprintf(stderr, "Error: Memory allocation failed for column name\n");
        return NULL;
    }

    wc->value = arena_strdup(arena, value);
    if (!wc->value) {
        fprintf(stderr, "Error: Memory allocation failed for value\n");
        return NULL;
    }

    wc->op = found_op;
    wc->column_idx = (size_t)-1;
    wc->is_numeric =
        (found_op == OP_GREATER || found_op == OP_LESS || found_op == OP_GREATER_EQ || found_op == OP_LESS_EQ);

    return wc;
}

/**
 * Tokenizer helper.
 * Peeks or consumes the next logical token.
 * A "token" is (, ), AND, OR, or a raw condition string.
 */
static inline bool check_token(char** stream, const char* token) {
    if (!stream || !*stream || !token) return false;

    char* s = *stream;
    while (isspace((unsigned char)*s)) s++;  // Skip whitespace

    size_t len = strlen(token);
    if (strncasecmp(s, token, len) == 0) {
        // Ensure strictly whole word match for AND/OR
        if (isalpha((unsigned char)token[0])) {
            char next = s[len];
            if (isalnum((unsigned char)next) || next == '_') return false;
        }
        *stream = s + len;
        return true;
    }
    return false;
}

/**
 * Parses a "Factor": ( Expression ) OR Condition
 */
static ASTNode* parse_factor(Arena* arena, char** stream) {
    if (!stream || !*stream) return NULL;

    char* s = *stream;
    while (isspace((unsigned char)*s)) s++;
    *stream = s;

    // Check for Parentheses
    if (check_token(stream, "(")) {
        ASTNode* node = parse_expression(arena, stream);
        if (!node) return NULL;

        if (!check_token(stream, ")")) {
            fprintf(stderr, "Error: Mismatched parentheses.\n");
            return NULL;
        }
        return node;
    }

    // Parse Condition (Read until AND, OR, ), or End)
    char* start = *stream;
    char* cursor = start;

    // Advance cursor until we hit a reserved word or char
    while (*cursor) {
        if (*cursor == '(' || *cursor == ')') break;

        // Check for " AND " or " OR " boundaries (case insensitive)
        if (strncasecmp(cursor, " AND ", 5) == 0 || strncasecmp(cursor, " OR ", 4) == 0) {
            break;
        }

        cursor++;
    }

    size_t len = (size_t)(cursor - start);
    if (len == 0) return NULL;  // Empty condition

    // Temporary buffer for condition string
    char* cond_buf = calloc(1, len + 1);
    if (!cond_buf) {
        fprintf(stderr, "Error: Memory allocation failed for condition buffer\n");
        return NULL;
    }
    strncpy(cond_buf, start, len);

    WhereClause* wc = parse_single_condition(arena, cond_buf);
    free(cond_buf);
    *stream = cursor;  // Advance stream

    if (!wc) return NULL;

    ASTNode* node = ARENA_ALLOC_ZERO(arena, ASTNode);
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed for ASTNode\n");
        return NULL;
    }

    node->type = NODE_CONDITION;
    node->clause = wc;
    return node;
}

/**
 * Parses a "Term": Factor { AND Factor }
 * AND binds tighter than OR.
 */
static ASTNode* parse_term(Arena* arena, char** stream) {
    ASTNode* left = parse_factor(arena, stream);
    if (!left) return NULL;

    while (check_token(stream, "AND")) {
        ASTNode* right = parse_factor(arena, stream);
        if (!right) {
            fprintf(stderr, "Error: Missing operand after AND\n");
            return NULL;
        }

        ASTNode* parent = ARENA_ALLOC_ZERO(arena, ASTNode);
        if (!parent) {
            fprintf(stderr, "Error: Memory allocation failed for AND Logic Node\n");
            return NULL;
        }

        parent->type = NODE_LOGIC;
        parent->logic_op = LOGIC_AND;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    return left;
}

/**
 * Parses an "Expression": Term { OR Term }
 */
static ASTNode* parse_expression(Arena* arena, char** stream) {
    ASTNode* left = parse_term(arena, stream);
    if (!left) return NULL;

    while (check_token(stream, "OR")) {
        ASTNode* right = parse_term(arena, stream);
        if (!right) {
            fprintf(stderr, "Error: Missing operand after OR\n");
            return NULL;
        }

        ASTNode* parent = ARENA_ALLOC_ZERO(arena, ASTNode);
        if (!parent) {
            fprintf(stderr, "Error: Memory allocation failed for OR Logic Node\n");
            return NULL;
        }

        parent->type = NODE_LOGIC;
        parent->logic_op = LOGIC_OR;
        parent->left = left;
        parent->right = right;
        left = parent;
    }
    return left;
}

/**
 * Entry point for parsing the where clause.
 */
bool parse_where_clause(Arena* arena, const char* where_str, WhereFilter* filter) {
    if (!where_str || !*where_str || !filter) return false;

    char* input = strdup(where_str);
    if (!input) {
        fprintf(stderr, "Error: Memory allocation failed for where string copy\n");
        return false;
    }

    char* cursor = input;

    filter->root = parse_expression(arena, &cursor);

    if (!filter->root) {
        free(input);
        return false;
    }

    // Check if we consumed the whole string (ignoring trailing spaces)
    while (isspace((unsigned char)*cursor)) cursor++;

    if (*cursor != '\0') {
        fprintf(stderr, "Error: Unexpected characters at end of where clause: '%s'\n", cursor);
        filter->root = NULL;
        free(input);
        return false;
    }

    free(input);
    return (filter->root != NULL);
}

/**
 * Helper for Resolving Column Indices (Recursive)
 */
void resolve_ast_indices(ASTNode* node, const Row* header) {
    if (!node) return;

    if (node->type == NODE_LOGIC) {
        resolve_ast_indices(node->left, header);
        resolve_ast_indices(node->right, header);
    } else if (node->type == NODE_CONDITION) {
        if (node->clause->column_idx == (size_t)-1) {
            ssize_t idx = find_column_by_name(header, node->clause->column_name);
            if (idx >= 0) {
                node->clause->column_idx = (size_t)idx;
            } else {
                fprintf(stderr, "Warning: Column '%s' in where clause not found in header.\n",
                        node->clause->column_name);
            }
        }
    }
}

/**
 * Evaluates a where clause against a row.
 * @param row The row to check.
 * @param clause The where clause.
 * @return true if the row matches the clause, false otherwise.
 */
static bool evaluate_where_clause(const Row* row, const WhereClause* clause) {
    if (row == NULL || clause == NULL) {
        return true;
    }

    if (clause->column_idx >= row->count) {
        return false;
    }

    const char* field = row->fields[clause->column_idx];
    if (field == NULL) {
        field = "";
    }

    // Trim the field for comparison (especially important for numeric comparisons)
    // Note: This creates a temporary copy for safety
    char* field_trimmed = trim_string((char*)field);
    if (!field_trimmed) {
        field_trimmed = (char*)field;
    }

    switch (clause->op) {
        case OP_CONTAINS:
            return strcasestr(field_trimmed, clause->value) != NULL;

        case OP_EQUALS: {
            // For equals, trim whitespace from both sides before comparing
            int result = strcasecmp(field_trimmed, clause->value);
            return result == 0;
        }

        case OP_NOT_EQUALS: {
            int result = strcasecmp(field_trimmed, clause->value);
            return result != 0;
        }

        case OP_GREATER:
        case OP_LESS:
        case OP_GREATER_EQ:
        case OP_LESS_EQ: {
            char* field_end;
            char* value_end;
            double field_num = strtod(field_trimmed, &field_end);
            double value_num = strtod(clause->value, &value_end);

            // Check if both parsed successfully (allowing trailing whitespace)
            while (isspace((unsigned char)*field_end)) field_end++;
            while (isspace((unsigned char)*value_end)) value_end++;

            if (*field_end != '\0' || *value_end != '\0') {
                return false;  // Not numeric
            }

            switch (clause->op) {
                case OP_GREATER:
                    return field_num > value_num;
                case OP_LESS:
                    return field_num < value_num;
                case OP_GREATER_EQ:
                    return field_num >= value_num;
                case OP_LESS_EQ:
                    return field_num <= value_num;
                default:
                    return false;
            }
        }
    }

    return false;
}

/**
 * Recursive evaluator for the AST.
 */
static bool eval_ast(const Row* row, const ASTNode* node) {
    if (!node) return true;

    if (node->type == NODE_LOGIC) {
        bool l_res = eval_ast(row, node->left);
        // Short-circuit logic
        if (node->logic_op == LOGIC_AND) {
            return l_res ? eval_ast(row, node->right) : false;
        } else {  // LOGIC_OR
            return l_res ? true : eval_ast(row, node->right);
        }
    } else {
        // NODE_CONDITION
        return evaluate_where_clause(row, node->clause);
    }
}

/**
 * Evaluates the complete WHERE filter against a row.
 */
bool evaluate_where_filter(const Row* row, const WhereFilter* filter) {
    if (!filter || !filter->root) return true;
    return eval_ast(row, filter->root);
}
