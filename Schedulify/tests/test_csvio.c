/*
 * test_csvio.c -- CSV import and export, including the round trip.
 *
 * The temporary files are created in the working directory and removed
 * again, so the suite leaves nothing behind.
 */
#include "suites.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "algorithms.h"
#include "compare.h"
#include "csvio.h"
#include "validate.h"

#define TMP_IN   "schedulify_test_in.csv"
#define TMP_OUT  "schedulify_test_out.csv"

static bool write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL) return false;
    fputs(text, fp);
    return fclose(fp) == 0;
}

/* True when the file contains the given substring somewhere. */
static bool file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    bool found = false;

    if (fp == NULL) return false;
    while (!found && fgets(line, sizeof line, fp) != NULL) {
        if (strstr(line, needle) != NULL) found = true;
    }
    fclose(fp);
    return found;
}

static void test_import_plain(void)
{
    Dataset ds;
    char err[ERR_MSG_MAX];

    t_case("import with a header row");
    CHECK(write_file(TMP_IN,
                     "id,arrival,burst,priority\n"
                     "P1,0,5,2\n"
                     "P2,1,3,1\n"
                     "P3,4,8,3\n"));

    if (csv_import_dataset(TMP_IN, &ds, err, sizeof err)) {
        CHECK_INT(ds.count, 3);
        CHECK_STR(ds.items[0].id, "P1");
        CHECK_INT(ds.items[0].arrival, 0);
        CHECK_INT(ds.items[0].burst, 5);
        CHECK_INT(ds.items[0].priority, 2);
        CHECK_STR(ds.items[2].id, "P3");
        CHECK_INT(ds.items[2].arrival, 4);
        CHECK_INT(ds.items[2].burst, 8);
        CHECK_INT(ds.items[2].priority, 3);
        /* The dataset is named after the file, without its extension. */
        CHECK(strstr(ds.name, "schedulify_test_in") != NULL);
    } else {
        CHECK_MSG(false, "import failed: %s", err);
    }
    dataset_free(&ds);
    remove(TMP_IN);
}

static void test_import_tolerances(void)
{
    Dataset ds;
    char err[ERR_MSG_MAX];

    t_case("header optional, comments and blank lines skipped, "
           "priority defaulted, whitespace ignored");
    CHECK(write_file(TMP_IN,
                     "# a comment\n"
                     "\n"
                     "   \n"
                     "  P1 ,  0 ,  5 \n"      /* no priority column */
                     "# another comment\n"
                     "P2,2,4,7\n"));

    if (csv_import_dataset(TMP_IN, &ds, err, sizeof err)) {
        CHECK_INT(ds.count, 2);
        CHECK_STR(ds.items[0].id, "P1");
        CHECK_INT(ds.items[0].arrival, 0);
        CHECK_INT(ds.items[0].burst, 5);
        CHECK_INT(ds.items[0].priority, MIN_PRIORITY);
        CHECK_STR(ds.items[1].id, "P2");
        CHECK_INT(ds.items[1].priority, 7);
    } else {
        CHECK_MSG(false, "import failed: %s", err);
    }
    dataset_free(&ds);
    remove(TMP_IN);
}

static void test_import_rejects(void)
{
    Dataset ds;
    char err[ERR_MSG_MAX];

    t_case("malformed files are rejected with a line number");

    CHECK(write_file(TMP_IN, "id,arrival,burst\nP1,0,5\nP2,oops,3\n"));
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "line 3") != NULL, "message was: %s", err);
    CHECK_INT(ds.count, 0);
    dataset_free(&ds);

    CHECK(write_file(TMP_IN, "P1,0,5\nP2,1\n"));
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "line 2") != NULL, "message was: %s", err);
    dataset_free(&ds);

    CHECK(write_file(TMP_IN, "P1,0,0\n"));       /* burst must be >= 1 */
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "burst") != NULL, "message was: %s", err);
    dataset_free(&ds);

    CHECK(write_file(TMP_IN, "P1,0,5\nP1,2,3\n")); /* duplicate id */
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "duplicate") != NULL, "message was: %s", err);
    dataset_free(&ds);

    CHECK(write_file(TMP_IN, "P1,-1,5\n"));      /* negative arrival */
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "arrival") != NULL, "message was: %s", err);
    dataset_free(&ds);

    CHECK(write_file(TMP_IN, "# nothing but a comment\n"));
    CHECK(!csv_import_dataset(TMP_IN, &ds, err, sizeof err));
    CHECK_MSG(strstr(err, "no process rows") != NULL, "message was: %s", err);
    dataset_free(&ds);

    CHECK(!csv_import_dataset("no_such_file_in_this_directory.csv", &ds, err,
                              sizeof err));
    CHECK_MSG(strstr(err, "cannot open") != NULL, "message was: %s", err);
    dataset_free(&ds);

    remove(TMP_IN);
}

static void test_import_skips_bom(void)
{
    Dataset ds;
    char err[ERR_MSG_MAX];

    t_case("a UTF-8 byte order mark does not confuse the importer");
    CHECK(write_file(TMP_IN, "\xEF\xBB\xBFid,arrival,burst,priority\n"
                             "P1,0,4,1\n"));

    if (csv_import_dataset(TMP_IN, &ds, err, sizeof err)) {
        CHECK_INT(ds.count, 1);
        CHECK_STR(ds.items[0].id, "P1");
    } else {
        CHECK_MSG(false, "import failed: %s", err);
    }
    dataset_free(&ds);
    remove(TMP_IN);
}

static void test_round_trip(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 6, 2 }, { "P2", 3, 8, 1 }, { "P3", 7, 2, 0 }
    };
    Dataset original, reloaded;
    char err[ERR_MSG_MAX];
    int i;

    t_case("export then import gives back the same workload");
    t_build(&original, "round-trip", spec, 3);

    CHECK_MSG(csv_export_dataset(TMP_OUT, &original, err, sizeof err),
              "export failed: %s", err);

    if (csv_import_dataset(TMP_OUT, &reloaded, err, sizeof err)) {
        CHECK_INT(reloaded.count, original.count);
        for (i = 0; i < original.count && i < reloaded.count; i++) {
            CHECK_STR(reloaded.items[i].id, original.items[i].id);
            CHECK_INT(reloaded.items[i].arrival, original.items[i].arrival);
            CHECK_INT(reloaded.items[i].burst, original.items[i].burst);
            CHECK_INT(reloaded.items[i].priority,
                      original.items[i].priority);
        }
    } else {
        CHECK_MSG(false, "re-import failed: %s", err);
    }

    dataset_free(&reloaded);
    dataset_free(&original);
    remove(TMP_OUT);
}

static void test_export_results(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 7, 2 }, { "P2", 2, 4, 1 },
        { "P3", 4, 1, 3 }, { "P4", 5, 4, 2 }
    };
    Dataset ds;
    Schedule sch;
    char err[ERR_MSG_MAX];

    t_case("exported results carry the computed numbers");
    t_build(&ds, "export", spec, 4);

    if (RUN(&sch, &ds, ALGO_SRTF, 0)) {
        CHECK_MSG(csv_export_schedule(TMP_OUT, &sch, err, sizeof err),
                  "export failed: %s", err);
        CHECK(file_contains(TMP_OUT, "section,id,arrival,burst"));
        CHECK(file_contains(TMP_OUT, "summary,algorithm,SRTF"));
        /* avg turnaround is exactly 7 for this workload under SRTF */
        CHECK(file_contains(TMP_OUT, "summary,avg_turnaround,7.0000"));
        CHECK(file_contains(TMP_OUT, "summary,preemptions,2"));
        CHECK(file_contains(TMP_OUT, "summary,context_switches,5"));
        CHECK(file_contains(TMP_OUT, "segment,0,P1,0,2,2"));
        remove(TMP_OUT);

        CHECK_MSG(csv_export_timeline(TMP_OUT, &sch, err, sizeof err),
                  "timeline export failed: %s", err);
        CHECK(file_contains(TMP_OUT, "index,process,start,end,duration"));
        CHECK(file_contains(TMP_OUT, "0,P1,0,2,2"));
        CHECK(file_contains(TMP_OUT, "5,P1,11,16,5"));
        remove(TMP_OUT);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_export_comparison(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 5, 2 }, { "P2", 1, 4, 1 },
        { "P3", 2, 2, 3 }, { "P4", 4, 1, 1 }
    };
    Dataset ds;
    Comparison cmp;
    char err[ERR_MSG_MAX];
    int i;

    t_case("the comparison export has one row per algorithm");
    t_build(&ds, "cmp-export", spec, 4);

    if (compare_run(&cmp, &ds, 2, err, sizeof err)) {
        for (i = 0; i < ALGO_COUNT; i++) {
            CHECK_MSG(cmp.ok[i], "%s failed: %s",
                      algo_abbrev((AlgorithmId)i), cmp.err[i]);
        }
        CHECK_MSG(csv_export_comparison(TMP_OUT, &cmp, err, sizeof err),
                  "export failed: %s", err);
        CHECK(file_contains(TMP_OUT, "algorithm,kind,quantum"));
        for (i = 0; i < ALGO_COUNT; i++) {
            CHECK_MSG(file_contains(TMP_OUT, algo_abbrev((AlgorithmId)i)),
                      "%s is missing from the export",
                      algo_abbrev((AlgorithmId)i));
        }
        CHECK(!file_contains(TMP_OUT, "FAILED"));
        remove(TMP_OUT);
    } else {
        CHECK_MSG(false, "compare_run failed: %s", err);
    }
    compare_free(&cmp);
    dataset_free(&ds);
}

void suite_csvio(void)
{
    t_suite("csv");
    test_import_plain();
    test_import_tolerances();
    test_import_rejects();
    test_import_skips_bom();
    test_round_trip();
    test_export_results();
    test_export_comparison();
}
