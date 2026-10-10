/*
 * test_metrics.c -- The metric derivation and its consistency checks.
 *
 * These tests build timelines by hand -- including deliberately broken ones
 * -- to prove that metrics.c computes the documented formulas and refuses a
 * timeline that could not have come from a correct scheduler.
 */
#include "suites.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "algorithms.h"
#include "metrics.h"

static const PSpec TWO[] = { { "P1", 0, 4, 0 }, { "P2", 2, 2, 0 } };

static void test_formulas(void)
{
    Dataset ds;
    Schedule sch;

    t_case("the documented formulas");
    t_build(&ds, "formulas", TWO, 2);

    /* Hand-built timeline: P1 [0,2), P2 [2,4), P1 [4,6). */
    CHECK(schedule_init(&sch, &ds, ALGO_SRTF, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 2));
    CHECK(schedule_add_segment(&sch, 1, 2, 4));
    CHECK(schedule_add_segment(&sch, 0, 4, 6));
    CHECK(metrics_finalize(&sch));

    CHECK_INT(sch.window_start, 0);
    CHECK_INT(sch.window_end, 6);
    CHECK_INT(sch.total_time, 6);
    CHECK_INT(sch.busy_time, 6);
    CHECK_INT(sch.idle_time, 0);
    CHECK_DBL(sch.cpu_utilization, 100.0);
    CHECK_DBL(sch.throughput, 2.0 / 6.0);

    /* P1: finishes 6, turnaround 6, waiting 2, response 0.
     * P2: finishes 4, turnaround 2, waiting 0, response 0. */
    CHECK_PROC(&sch, "P1", 6, 6, 2, 0);
    CHECK_PROC(&sch, "P2", 4, 2, 0, 0);
    CHECK_DBL(sch.avg_completion, 5.0);
    CHECK_DBL(sch.avg_turnaround, 4.0);
    CHECK_DBL(sch.avg_waiting, 1.0);
    CHECK_DBL(sch.avg_response, 0.0);

    /* One hand-over away from P1 and one back: two context switches, and
     * only the first one is a preemption (P2 ran to completion). */
    CHECK_INT(sch.context_switches, 2);
    CHECK_INT(sch.preemptions, 1);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_merge_and_switch_counting(void)
{
    Dataset ds;
    Schedule sch;

    t_case("adjacent slices of one process merge and cost no switch");
    t_build(&ds, "merge", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_RR, 2, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 2));
    CHECK(schedule_add_segment(&sch, 0, 2, 4));   /* merges into [0,4) */
    CHECK(schedule_add_segment(&sch, 1, 4, 6));
    CHECK_INT(sch.seg_count, 2);
    CHECK_INT(sch.segments[0].start, 0);
    CHECK_INT(sch.segments[0].end, 4);
    CHECK(metrics_finalize(&sch));
    CHECK_INT(sch.context_switches, 1);
    CHECK_INT(sch.preemptions, 0);   /* P1 had finished before handing over */

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_idle_accounting(void)
{
    static const PSpec spec[] = { { "P1", 0, 2, 0 }, { "P2", 6, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("idle time is charged to the window, not to any process");
    t_build(&ds, "idle", spec, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 2));
    CHECK(schedule_add_segment(&sch, IDLE_INDEX, 2, 6));
    CHECK(schedule_add_segment(&sch, 1, 6, 8));
    CHECK(metrics_finalize(&sch));

    CHECK_INT(sch.idle_time, 4);
    CHECK_INT(sch.busy_time, 4);
    CHECK_INT(sch.total_time, 8);
    CHECK_DBL(sch.cpu_utilization, 50.0);
    CHECK_PROC(&sch, "P2", 8, 2, 0, 0);   /* waited for nothing */
    CHECK_INT(sch.context_switches, 1);
    CHECK_INT(sch.preemptions, 0);

    schedule_free(&sch);
    dataset_free(&ds);
}

/* ------------------------------------------------- rejection of bad input */

static void test_rejects_gap(void)
{
    Dataset ds;
    Schedule sch;

    t_case("a hole in the timeline is rejected");
    t_build(&ds, "bad-gap", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 2));
    CHECK(schedule_add_segment(&sch, 1, 4, 6));   /* [2,4) missing */
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "gap") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rejects_overrun(void)
{
    Dataset ds;
    Schedule sch;

    t_case("giving a process more CPU than its burst is rejected");
    t_build(&ds, "bad-overrun", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 9));   /* burst is only 4 */
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "burst") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rejects_missing_process(void)
{
    Dataset ds;
    Schedule sch;

    t_case("a process that never runs is rejected");
    t_build(&ds, "bad-missing", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 4));
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "P2") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rejects_early_run(void)
{
    Dataset ds;
    Schedule sch;

    t_case("running a process before it arrives is rejected");
    t_build(&ds, "bad-early", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 1, 0, 2));   /* P2 arrives at t=2 */
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "arrives") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rejects_trailing_idle(void)
{
    Dataset ds;
    Schedule sch;

    t_case("a timeline that ends idle is rejected");
    t_build(&ds, "bad-tail", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 4));
    CHECK(schedule_add_segment(&sch, 1, 4, 6));
    CHECK(schedule_add_segment(&sch, IDLE_INDEX, 6, 9));
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "idle") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rejects_wrong_start(void)
{
    static const PSpec spec[] = { { "P1", 4, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("a timeline that starts before the first arrival is rejected");
    t_build(&ds, "bad-start", spec, 1);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, IDLE_INDEX, 0, 4));
    CHECK(schedule_add_segment(&sch, 0, 4, 6));
    CHECK(!metrics_finalize(&sch));
    CHECK(strstr(metrics_last_error(), "earliest arrival") != NULL);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_zero_length_segments_ignored(void)
{
    Dataset ds;
    Schedule sch;

    t_case("zero-length slices are dropped, not stored");
    t_build(&ds, "zero", TWO, 2);

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(schedule_add_segment(&sch, 0, 0, 0));
    CHECK_INT(sch.seg_count, 0);
    CHECK(schedule_add_segment(&sch, 0, 0, 4));
    CHECK(schedule_add_segment(&sch, 1, 4, 4));
    CHECK_INT(sch.seg_count, 1);
    CHECK(schedule_add_segment(&sch, 1, 4, 6));
    CHECK_INT(sch.seg_count, 2);
    CHECK(metrics_finalize(&sch));

    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_empty_schedule(void)
{
    Dataset ds;
    Schedule sch;

    t_case("an empty workload finalises to all zeroes");
    dataset_init(&ds, "empty");

    CHECK(schedule_init(&sch, &ds, ALGO_FCFS, 0, false));
    CHECK(metrics_finalize(&sch));
    CHECK_INT(sch.n, 0);
    CHECK_INT(sch.total_time, 0);
    CHECK_DBL(sch.cpu_utilization, 0.0);
    CHECK_DBL(sch.throughput, 0.0);

    schedule_free(&sch);
    dataset_free(&ds);
}

void suite_metrics(void)
{
    t_suite("metrics");
    test_formulas();
    test_merge_and_switch_counting();
    test_idle_accounting();
    test_rejects_gap();
    test_rejects_overrun();
    test_rejects_missing_process();
    test_rejects_early_run();
    test_rejects_trailing_idle();
    test_rejects_wrong_start();
    test_zero_length_segments_ignored();
    test_empty_schedule();
}
