#include "where-parser.h"
#include <ctype.h>
#include <solidc/cstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations for the parser
static ASTNode* parse_expression(char** stream);
static void ast_free(ASTNode* node);

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
 */
static WhereClause* parse_single_condition(char* cond_str) {
    // Trim input
    cond_str = trim_string(cond_str);
    if (!cond_str || !*cond_str) return NULL;

    const char* operators[] = {">=", "<=", "!=", "contains", ">", "<", "="};
    CompareOp ops[] = {OP_GREATER_EQ, OP_LESS_EQ, OP_NOT_EQUALS, OP_CONTAINS, OP_GREATER, OP_LESS, OP_EQUALS};
    size_t num_ops = sizeof(operators) / sizeof(operators[0]);

    char* op_pos = NULL;
    CompareOp found_op = OP_CONTAINS;

    // Find operator
    for (size_t i = 0; i < num_ops; i++) {
        char* pos = strcasestr(cond_str, operators[i]);
        if (pos != NULL) {
            op_pos = pos;
            found_op = ops[i];
            break;
        }
    }

    if (op_pos == NULL) {
        fprintf(stderr, "Error: No valid operator in clause: '%s'\n", cond_str);
        return NULL;
    }

    // Determine the length of the found operator (simplified approach)
    size_t op_len;
    // NOTE: This switch statement replaces the complex ternary operator chain
    switch (found_op) {
        case OP_GREATER_EQ:
            op_len = strlen(operators[0]);
            break;
        case OP_LESS_EQ:
            op_len = strlen(operators[1]);
            break;
        case OP_NOT_EQUALS:
            op_len = strlen(operators[2]);
            break;
        case OP_CONTAINS:
            op_len = strlen(operators[3]);
            break;
        case OP_GREATER:
            op_len = strlen(operators[4]);
            break;
        case OP_LESS:
            op_len = strlen(operators[5]);
            break;
        case OP_EQUALS:
        default:
            op_len = strlen(operators[6]);
            break;
    }

    *op_pos = '\0';  // Split string
    char* col_name = cond_str;
    char* value = op_pos + op_len;

    col_name = trim_string(col_name);
    value = trim_string(value);

    // Value can be empty string in some cases, but col cannot.
    // If value is NULL (from trim failure), that's an error.
    if (!col_name || !*col_name || !value) {
        return NULL;
    }

    WhereClause* wc = calloc(1, sizeof(WhereClause));
    if (!wc) {
        fprintf(stderr, "Error: Memory allocation failed for WhereClause\n");
        return NULL;
    }
    wc->column_name = strdup(col_name);
    if (!wc->column_name) {
        fprintf(stderr, "Error: Memory allocation failed for column name\n");
        free(wc);
        return NULL;
    }
    wc->value = strdup(value);
    if (!wc->value) {
        fprintf(stderr, "Error: Memory allocation failed for value\n");
        free(wc->column_name);
        free(wc);
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
static ASTNode* parse_factor(char** stream) {
    if (!stream || !*stream) return NULL;

    char* s = *stream;
    while (isspace((unsigned char)*s)) s++;
    *stream = s;

    // Check for Parentheses
    if (check_token(stream, "(")) {
        ASTNode* node = parse_expression(stream);
        // If expression parsing failed, propagate error
        if (!node) return NULL;

        if (!check_token(stream, ")")) {
            fprintf(stderr, "Error: Mismatched parentheses.\n");
            ast_free(node);
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

        // Handle end of string check for keywords at exact end?
        // Logic handles this by trimming.
        cursor++;
    }

    size_t len = (size_t)(cursor - start);
    if (len == 0) return NULL;  // Empty condition

    // Check for calloc failure
    char* cond_buf = calloc(1, len + 1);
    if (!cond_buf) {
        fprintf(stderr, "Error: Memory allocation failed for condition buffer\n");
        return NULL;
    }
    strncpy(cond_buf, start, len);

    WhereClause* wc = parse_single_condition(cond_buf);
    free(cond_buf);
    *stream = cursor;  // Advance stream

    if (!wc) return NULL;

    // Check for ASTNode allocation failure
    ASTNode* node = calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed for ASTNode\n");
        // Free the WhereClause we just successfully parsed
        free(wc->column_name);
        free(wc->value);
        free(wc);
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
static ASTNode* parse_term(char** stream) {
    ASTNode* left = parse_factor(stream);
    if (!left) return NULL;

    while (check_token(stream, "AND")) {
        ASTNode* right = parse_factor(stream);
        if (!right) {
            fprintf(stderr, "Error: Missing operand after AND\n");
            ast_free(left);
            return NULL;
        }

        ASTNode* parent = calloc(1, sizeof(ASTNode));
        if (!parent) {
            fprintf(stderr, "Error: Memory allocation failed for AND Logic Node\n");
            ast_free(left);
            ast_free(right);
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
static ASTNode* parse_expression(char** stream) {
    ASTNode* left = parse_term(stream);
    if (!left) return NULL;

    while (check_token(stream, "OR")) {
        ASTNode* right = parse_term(stream);
        if (!right) {
            fprintf(stderr, "Error: Missing operand after OR\n");
            ast_free(left);
            return NULL;
        }

        ASTNode* parent = calloc(1, sizeof(ASTNode));
        if (!parent) {
            fprintf(stderr, "Error: Memory allocation failed for OR Logic Node\n");
            ast_free(left);
            ast_free(right);
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
bool parse_where_clause(const char* where_str, WhereFilter* filter) {
    if (!where_str || !*where_str || !filter) return false;

    char* input = strdup(where_str);
    if (!input) {
        fprintf(stderr, "Error: Memory allocation failed for where string copy\n");
        return false;
    }

    char* cursor = input;

    filter->root = parse_expression(&cursor);

    // Check if we parsed anything successfully
    if (!filter->root) {
        free(input);
        return false;
    }

    // Check if we consumed the whole string (ignoring trailing spaces)
    while (isspace((unsigned char)*cursor)) cursor++;

    if (*cursor != '\0') {
        fprintf(stderr, "Error: Unexpected characters at end of where clause: '%s'\n", cursor);
        ast_free(filter->root);
        filter->root = NULL;
        free(input);
        return false;
    }

    free(input);
    return (filter->root != NULL);
}

/**
 * Recursively frees the AST.
 */
static void ast_free(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_LOGIC) {
        ast_free(node->left);
        ast_free(node->right);
    } else if (node->type == NODE_CONDITION && node->clause) {
        free(node->clause->column_name);
        free(node->clause->value);
        free(node->clause);
    }
    free(node);
}

void where_filter_free(WhereFilter* filter) {
    if (filter) {
        ast_free(filter->root);
        filter->root = NULL;
    }
}

// Helper for Resolving Column Indices (Recursive)
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

    switch (clause->op) {
        case OP_CONTAINS:
            return strcasestr(field, clause->value) != NULL;

        case OP_EQUALS:
            return strcasecmp(field, clause->value) == 0;

        case OP_NOT_EQUALS:
            return strcasecmp(field, clause->value) != 0;

        case OP_GREATER:
        case OP_LESS:
        case OP_GREATER_EQ:
        case OP_LESS_EQ: {
            char* field_end;
            char* value_end;
            double field_num = strtod(field, &field_end);
            double value_num = strtod(clause->value, &value_end);

            // Check if both parsed successfully
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

bool evaluate_where_filter(const Row* row, const WhereFilter* filter) {
    if (!filter || !filter->root) return true;
    return eval_ast(row, filter->root);
}
