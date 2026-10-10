/*
 * csvio.h -- CSV import and export.
 *
 * WORKLOAD FORMAT (import and export)
 * -----------------------------------
 *   id,arrival,burst,priority
 *   P1,0,5,2
 *   P2,1,3,1
 *
 * Rules the importer applies:
 *   - a leading UTF-8 BOM is skipped;
 *   - blank lines and lines whose first non-blank character is '#' are
 *     comments and are ignored;
 *   - the header row is optional and is detected case-insensitively; a file
 *     without one is read as data from the first line;
 *   - the priority column is optional and defaults to 0;
 *   - surrounding whitespace around any field is ignored;
 *   - every field goes through exactly the same validation as keyboard
 *     input, and the first offending line is reported with its line number.
 *
 * Two kinds of result file can be written: the per-process metrics of one
 * schedule, and the side-by-side comparison of all six algorithms.
 */
#ifndef SCHEDULIFY_CSVIO_H
#define SCHEDULIFY_CSVIO_H

#include <stdbool.h>
#include <stddef.h>

#include "compare.h"
#include "process.h"

/* Reads a workload.  ds is initialised by this call on success.  On failure
 * err receives a message naming the line at fault and ds is left empty. */
bool csv_import_dataset(const char *path, Dataset *ds, char *err,
                        size_t errsz);

/* Writes the workload in the format above. */
bool csv_export_dataset(const char *path, const Dataset *ds, char *err,
                        size_t errsz);

/* Writes per-process metrics plus a summary block for one schedule. */
bool csv_export_schedule(const char *path, const Schedule *sch, char *err,
                         size_t errsz);

/* Writes one row per algorithm with every aggregate metric. */
bool csv_export_comparison(const char *path, const Comparison *cmp,
                           char *err, size_t errsz);

/* Writes the execution timeline (one row per segment) of one schedule. */
bool csv_export_timeline(const char *path, const Schedule *sch, char *err,
                         size_t errsz);

#endif /* SCHEDULIFY_CSVIO_H */
