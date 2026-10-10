/*
 * test_validation.c -- Field validation, integer parsing and the Dataset
 * container.
 */
#include "suites.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "datasets.h"
#include "validate.h"

#define ERRBUF char err[ERR_MSG_MAX]

static void test_ids(void)
{
    ERRBUF;

    t_case("process ids");
    CHECK(validate_id("P1", err, sizeof err));
    CHECK(validate_id("worker-7", err, sizeof err));
    CHECK(validate_id("A", err, sizeof err));
    CHECK(validate_id("123456789012345", err, sizeof err));  /* 15 chars */

    CHECK(!validate_id("", err, sizeof err));
    CHECK(!validate_id(NULL, err, sizeof err));
    CHECK(!validate_id("1234567890123456", err, sizeof err)); /* 16 chars */
    CHECK(!validate_id("has space", err, sizeof err));
    CHECK(!validate_id("has,comma", err, sizeof err));
    CHECK(!validate_id("has;semi", err, sizeof err));
    CHECK(!validate_id("has\"quote", err, sizeof err));
    CHECK(!validate_id("tab\there", err, sizeof err));
}

static void test_numeric_fields(void)
{
    ERRBUF;

    t_case("arrival, burst, priority and quantum bounds");
    CHECK(validate_arrival(0, err, sizeof err));
    CHECK(validate_arrival(MAX_ARRIVAL, err, sizeof err));
    CHECK(!validate_arrival(-1, err, sizeof err));
    CHECK(!validate_arrival(MAX_ARRIVAL + 1, err, sizeof err));

    CHECK(validate_burst(1, err, sizeof err));
    CHECK(validate_burst(MAX_BURST, err, sizeof err));
    CHECK(!validate_burst(0, err, sizeof err));       /* must want some CPU */
    CHECK(!validate_burst(-3, err, sizeof err));
    CHECK(!validate_burst(MAX_BURST + 1, err, sizeof err));

    CHECK(validate_priority(MIN_PRIORITY, err, sizeof err));
    CHECK(validate_priority(MAX_PRIORITY, err, sizeof err));
    CHECK(!validate_priority(MIN_PRIORITY - 1, err, sizeof err));
    CHECK(!validate_priority(MAX_PRIORITY + 1, err, sizeof err));

    CHECK(validate_quantum(1, err, sizeof err));
    CHECK(validate_quantum(MAX_QUANTUM, err, sizeof err));
    CHECK(!validate_quantum(0, err, sizeof err));
    CHECK(!validate_quantum(MAX_QUANTUM + 1, err, sizeof err));

    /* The message must say something useful. */
    CHECK(!validate_burst(0, err, sizeof err));
    CHECK(strstr(err, "burst") != NULL);
}

static void test_parse_int(void)
{
    int v = -999;

    t_case("strict integer parsing");
    CHECK(parse_int("0", &v));          CHECK_INT(v, 0);
    CHECK(parse_int("42", &v));         CHECK_INT(v, 42);
    CHECK(parse_int("  42  ", &v));     CHECK_INT(v, 42);
    CHECK(parse_int("-7", &v));         CHECK_INT(v, -7);
    CHECK(parse_int("+7", &v));         CHECK_INT(v, 7);

    CHECK(!parse_int("", &v));
    CHECK(!parse_int("   ", &v));
    CHECK(!parse_int("12x", &v));
    CHECK(!parse_int("x12", &v));
    CHECK(!parse_int("1 2", &v));
    CHECK(!parse_int("3.5", &v));
    CHECK(!parse_int("0x10", &v));
    CHECK(!parse_int("99999999999999999999", &v));   /* overflow */
    CHECK(!parse_int(NULL, &v));

    CHECK(parse_int_range("5", 1, 10, &v));   CHECK_INT(v, 5);
    CHECK(!parse_int_range("0", 1, 10, &v));
    CHECK(!parse_int_range("11", 1, 10, &v));
    CHECK(parse_int_range("1", 1, 10, &v));
    CHECK(parse_int_range("10", 1, 10, &v));
}

static void test_trim(void)
{
    char a[] = "   hello   ";
    char b[] = "\t\n x \r\n";
    char c[] = "     ";
    char d[] = "none";

    t_case("whitespace trimming");
    CHECK_STR(trim(a), "hello");
    CHECK_STR(trim(b), "x");
    CHECK_STR(trim(c), "");
    CHECK_STR(trim(d), "none");
}

static void test_dataset_validation(void)
{
    static const PSpec good[] = {
        { "P1", 0, 5, 0 }, { "P2", 1, 3, 1 }
    };
    Dataset ds;
    ERRBUF;

    t_case("whole-dataset validation");

    dataset_init(&ds, "empty");
    CHECK(!validate_dataset(&ds, err, sizeof err));
    CHECK(strstr(err, "empty") != NULL);
    dataset_free(&ds);

    t_build(&ds, "good", good, 2);
    CHECK(validate_dataset(&ds, err, sizeof err));

    /* A duplicate id must be caught and both rows named. */
    {
        Process dup;
        memset(&dup, 0, sizeof dup);
        snprintf(dup.id, sizeof dup.id, "P1");
        dup.arrival = 4;
        dup.burst = 2;
        CHECK(dataset_add(&ds, &dup));
        CHECK(!validate_dataset(&ds, err, sizeof err));
        CHECK(strstr(err, "duplicate") != NULL);
        CHECK(strstr(err, "P1") != NULL);
        CHECK(dataset_remove(&ds, 2));
        CHECK(validate_dataset(&ds, err, sizeof err));
    }

    /* A bad field must be reported with its row number. */
    ds.items[1].burst = 0;
    CHECK(!validate_dataset(&ds, err, sizeof err));
    CHECK(strstr(err, "row 2") != NULL);

    dataset_free(&ds);
}

static void test_dataset_container(void)
{
    static const PSpec spec[] = {
        { "P1", 5, 5, 0 }, { "P2", 1, 3, 1 }, { "P3", 5, 2, 2 },
        { "P4", 0, 1, 3 }
    };
    Dataset ds, clone;

    t_case("Dataset add / find / remove / clone / sort");
    t_build(&ds, "container", spec, 4);

    CHECK_INT(ds.count, 4);
    CHECK_INT(dataset_find_id(&ds, "P3"), 2);
    CHECK_INT(dataset_find_id(&ds, "p3"), 2);     /* case-insensitive */
    CHECK_INT(dataset_find_id(&ds, "nope"), -1);
    CHECK_INT(dataset_min_arrival(&ds), 0);
    CHECK_INT(dataset_total_burst(&ds), 11);

    CHECK(dataset_clone(&clone, &ds));
    CHECK_INT(clone.count, 4);
    CHECK_STR(clone.items[0].id, "P1");
    CHECK_STR(clone.name, "container");

    /* The sort is stable: P1 and P3 both arrive at t=5 and must keep their
     * relative order. */
    dataset_sort_by_arrival(&clone);
    CHECK_STR(clone.items[0].id, "P4");   /* arrival 0 */
    CHECK_STR(clone.items[1].id, "P2");   /* arrival 1 */
    CHECK_STR(clone.items[2].id, "P1");   /* arrival 5, listed first */
    CHECK_STR(clone.items[3].id, "P3");   /* arrival 5, listed second */

    CHECK(dataset_remove(&ds, 0));
    CHECK_INT(ds.count, 3);
    CHECK_STR(ds.items[0].id, "P2");
    CHECK(!dataset_remove(&ds, 99));
    CHECK(!dataset_remove(&ds, -1));

    dataset_clear(&ds);
    CHECK_INT(ds.count, 0);

    dataset_free(&clone);
    dataset_free(&ds);
}

static void test_dataset_capacity(void)
{
    Dataset ds;
    int i;
    bool all_ok = true;

    t_case("the dataset grows to MAX_PROCESSES and then refuses more");
    dataset_init(&ds, "full");

    for (i = 0; i < MAX_PROCESSES; i++) {
        Process p;
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "P%d", i + 1);
        p.arrival = i;
        p.burst = 1;
        if (!dataset_add(&ds, &p)) { all_ok = false; break; }
    }
    CHECK(all_ok);
    CHECK_INT(ds.count, MAX_PROCESSES);

    {
        Process extra;
        memset(&extra, 0, sizeof extra);
        snprintf(extra.id, sizeof extra.id, "OVER");
        extra.burst = 1;
        CHECK(!dataset_add(&ds, &extra));
    }
    dataset_free(&ds);
}

static void test_random_is_reproducible(void)
{
    Dataset a, b, c;
    int i;
    bool same = true, different = false;

    t_case("the random generator is seed-reproducible");

    CHECK(dataset_random(&a, 12, 2024, 15, 12, 7));
    CHECK(dataset_random(&b, 12, 2024, 15, 12, 7));
    CHECK(dataset_random(&c, 12, 2025, 15, 12, 7));

    CHECK_INT(a.count, 12);
    for (i = 0; i < a.count; i++) {
        if (a.items[i].arrival != b.items[i].arrival
            || a.items[i].burst != b.items[i].burst
            || a.items[i].priority != b.items[i].priority) {
            same = false;
        }
        if (i < c.count && (a.items[i].arrival != c.items[i].arrival
                            || a.items[i].burst != c.items[i].burst)) {
            different = true;
        }
        CHECK_MSG(a.items[i].burst >= MIN_BURST && a.items[i].burst <= 12,
                  "generated burst %d is out of range", a.items[i].burst);
        CHECK_MSG(a.items[i].arrival >= 0 && a.items[i].arrival <= 15,
                  "generated arrival %d is out of range",
                  a.items[i].arrival);
        CHECK_MSG(a.items[i].priority >= 0 && a.items[i].priority <= 7,
                  "generated priority %d is out of range",
                  a.items[i].priority);
    }
    CHECK_MSG(same, "the same seed produced a different workload");
    CHECK_MSG(different, "a different seed produced an identical workload");

    {
        char err[ERR_MSG_MAX];
        CHECK(validate_dataset(&a, err, sizeof err));
    }

    dataset_free(&a);
    dataset_free(&b);
    dataset_free(&c);
}

static void test_examples_are_valid(void)
{
    int i;

    t_case("every predefined example passes validation");
    for (i = 0; i < examples_count(); i++) {
        Dataset ds;
        char err[ERR_MSG_MAX];
        CHECK_MSG(example_load(i, &ds), "example %d failed to load", i + 1);
        CHECK_MSG(validate_dataset(&ds, err, sizeof err),
                  "example %d (%s) is invalid: %s", i + 1, example_name(i),
                  err);
        CHECK_MSG(validate_quantum(example_quantum(i), err, sizeof err),
                  "example %d has an invalid quantum", i + 1);
        dataset_free(&ds);
    }
}

void suite_validation(void)
{
    t_suite("validation");
    test_ids();
    test_numeric_fields();
    test_parse_int();
    test_trim();
    test_dataset_validation();
    test_dataset_container();
    test_dataset_capacity();
    test_random_is_reproducible();
    test_examples_are_valid();
}
