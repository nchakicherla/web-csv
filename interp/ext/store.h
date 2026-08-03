#ifndef WC_STORE_H
#define WC_STORE_H

/* store.h - the session's set of live columns.
 *
 * Two kinds of entries, both freed in bulk by storeTerm():
 *
 *   named   - CSV-loaded columns a script reaches via col("amount"). JS
 *             (re)sets these as the user uploads/reloads a CSV; setting an
 *             existing name frees the old column first.
 *   tracked - intermediate/result columns a builtin creates (e.g.
 *             filter_gt's output) that aren't addressable by name but still
 *             need a home so they don't leak for the rest of the session.
 */

#include "column.h"

void storeInit(void);
void storeTerm(void);

void storeSetNamed(const char *name, Column *col);
Column *storeGetNamed(const char *name);

void storeTrack(Column *col);

#endif // WC_STORE_H
