#ifndef SYNTAX_TYPES_H
#define SYNTAX_TYPES_H

/* The syntax vocabulary is generated from syntax.def. Values start at 0 and
 * are dense, so they index the registry's name table directly - the previous
 * enum-starts-at-1 / labels-start-at-0 mismatch is structurally impossible now.
 *
 * SYNTAX_TYPE is a plain int rather than a bare enum because grammar files can
 * mint additional names that get IDs at or above STX__COUNT. */

enum {
#define STX(name) name,
#include "syntax.def"
#undef STX
	STX__COUNT
};

typedef int SYNTAX_TYPE;

/* "not a syntax type" - used for anonymous grouping nodes that carry no name.
 * Distinct from STX_ERROR, which is a real rule in the grammar. */
#define STX__NONE (-1)

#endif // SYNTAX_TYPES_H
