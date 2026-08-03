#ifndef SHAPE_H
#define SHAPE_H

/* shape.h - a structural sanity check for a parsed tree against the built-in
 * syntax vocabulary's known requirements.
 *
 * There is no interpreter in this project (see README.md), but the built-in
 * STX_* tags exist so that one could be added later without needing a second
 * grammar-authoring convention - and that future interpreter's tree-walking
 * code would need specific parts from a node tagged, say, STX_WHILE: at
 * least a condition. This check verifies a parsed tree actually has those
 * parts *now*, at grammar-design time, instead of only discovering their
 * absence via a confusing runtime error the first time that branch of the
 * tree happens to run.
 *
 * What this deliberately cannot catch: two built-in tags that need the same
 * shape. STX_IF and STX_WHILE both just need one condition and one body -
 * tagging an if-shaped rule STX_WHILE by mistake produces an identically
 * structured tree, so there is nothing structural to detect. That is a
 * naming/intent mistake, not a shape mistake; nothing short of reading the
 * grammar (or, someday, an interpreter that actually behaves wrong when you
 * run the example) can catch it.
 */

#include "registry.h"
#include "ast_node.h"

#include <stdio.h>

/* Walks `root`, checking every node whose tag has a known contract. Prints
 * one line per violation to `out`, each naming the rule's line, its tag, and
 * what it's missing. Returns the number of violations found (0 = clean). */
size_t checkShapes(Registry *reg, const SyntaxNode *root, FILE *out);

#endif // SHAPE_H
