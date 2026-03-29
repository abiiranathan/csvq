#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Comparison operators for where clause. */
typedef enum {
    OP_CONTAINS,    // case-insensitive substring match
    OP_EQUALS,      // case-insensitive equality
    OP_NOT_EQUALS,  // case-insensitive inequality
    OP_GREATER,     // numeric >
    OP_LESS,        // numeric <
    OP_GREATER_EQ,  // numeric >=
    OP_LESS_EQ,     // numeric <=
} CompareOp;

/** Logical operators for where clause conditions. */
typedef enum {
    LOGIC_AND,  // AND operator (all conditions must match)
    LOGIC_OR,   // OR operator (at least one condition must match)
} LogicOp;

/** Where clause filter. */
typedef struct {
    char* column_name;  // Column name to filter on
    size_t column_idx;  // Resolved column index
    CompareOp op;       // Comparison operator
    char* value;        // Value to compare against
    bool is_numeric;    // Whether to treat value as numeric
} WhereClause;

/** AST Node types. */
typedef enum { NODE_LOGIC, NODE_CONDITION } NodeType;

/** AST Node structure. */
typedef struct ASTNode {
    NodeType type;

    // For Logic Nodes
    LogicOp logic_op;  // LOGIC_AND or LOGIC_OR
    struct ASTNode* left;
    struct ASTNode* right;

    // For Condition Nodes
    WhereClause* clause;  // The actual comparison logic
} ASTNode;

/** Wrapper for the AST Root */
typedef struct {
    ASTNode* root;
} WhereFilter;

#ifdef __cplusplus
}
#endif

#endif  // TYPES_H
