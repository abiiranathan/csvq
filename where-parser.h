#ifndef FILTER_H
#define FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <solidc/csvparser.h>
#include <solidc/arena.h>
#include "types.h"

bool evaluate_where_filter(const Row* row, const WhereFilter* filter);

/**
 * Entry point for parsing the where clause.
 */
bool parse_where_clause(Arena* arena, const char* where_str, WhereFilter* filter);

// Helper function for Resolving Column Indices (Recursive)
void resolve_ast_indices(ASTNode* node, const Row* header);


#ifdef __cplusplus
}
#endif

#endif  // FILTER_H
