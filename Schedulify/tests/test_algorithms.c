/*
 * test_algorithms.c -- Every algorithm against hand-calculated schedules.
 *
 * All expected values in this file were worked out on paper first; the
 * derivations are reproduced in README.md so they can be checked.
 */
#include "suites.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "algorithms.h"

/* The four-process workload used by the classic SJF/SRTF comparison. */
static const PSpec CLASSIC[] = {
    { "P1", 0, 7, 2 }, { "P2", 2, 4, 1 },
    { "P3", 4, 1, 3 }, { "P4", 5, 4, 2 }
};

static void check_line(const Schedule *sch, const char *expected,
                       const char *file, int line)
{
    char got[512];
    t_timeline(sch, got, sizeof got);
    t_check_str(got, expected, file, line, "timeline");
}
#define CHECK_TIMELINE(sch, exp) check_line((sch), (exp), __FILE__, __LINE__)

/* ------------------------------------------------------------------ FCFS */

static void test_fcfs_basic(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 5, 0 }, { "P2", 1, 3, 0 }, { "P3", 2, 8, 0 }
    };
    Dataset ds;
    Schedule sch;

    t_case("FCFS: P1(0,5) P2(1,3) P3(2,8)");
    t_build(&ds, "fcfs-basic", spec, 3);

    if (RUN(&sch, &ds, ALGO_FCFS, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-5,P2:5-8,P3:8-16");
        CHECK_PROC(&sch, "P1", 5, 5, 0, 0);
        CHECK_PROC(&sch, "P2", 8, 7, 4, 4);
        CHECK_PROC(&sch, "P3", 16, 14, 6, 6);
        CHECK_DBL(sch.avg_turnaround, 26.0 / 3.0);
        CHECK_DBL(sch.avg_waiting, 10.0 / 3.0);
        CHECK_DBL(sch.avg_response, 10.0 / 3.0);
        CHECK_DBL(sch.avg_completion, 29.0 / 3.0);
        CHECK_INT(sch.total_time, 16);
        CHECK_INT(sch.busy_time, 16);
        CHECK_INT(sch.idle_time, 0);
        CHECK_DBL(sch.cpu_utilization, 100.0);
        CHECK_DBL(sch.throughput, 3.0 / 16.0);
        CHECK_INT(sch.context_switches, 2);
        CHECK_INT(sch.preemptions, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_fcfs_idle(void)
{
    static const PSpec spec[] = { { "P1", 0, 3, 0 }, { "P2", 7, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("FCFS with an idle CPU: P1(0,3) P2(7,2)");
    t_build(&ds, "fcfs-idle", spec, 2);

    if (RUN(&sch, &ds, ALGO_FCFS, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-3,IDLE:3-7,P2:7-9");
        CHECK_PROC(&sch, "P1", 3, 3, 0, 0);
        CHECK_PROC(&sch, "P2", 9, 2, 0, 0);
        CHECK_INT(sch.total_time, 9);
        CHECK_INT(sch.busy_time, 5);
        CHECK_INT(sch.idle_time, 4);
        CHECK_DBL(sch.cpu_utilization, 500.0 / 9.0);
        CHECK_DBL(sch.throughput, 2.0 / 9.0);
        CHECK_INT(sch.context_switches, 1);
        CHECK_INT(sch.preemptions, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_fcfs_late_start(void)
{
    static const PSpec spec[] = { { "P1", 5, 3, 0 }, { "P2", 6, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("FCFS where nothing arrives before t=5");
    t_build(&ds, "fcfs-late", spec, 2);

    if (RUN(&sch, &ds, ALGO_FCFS, 0)) {
        CHECK_TIMELINE(&sch, "P1:5-8,P2:8-10");
        CHECK_PROC(&sch, "P1", 8, 3, 0, 0);
        CHECK_PROC(&sch, "P2", 10, 4, 2, 2);
        /* The window starts at the first arrival, not at t=0. */
        CHECK_INT(sch.window_start, 5);
        CHECK_INT(sch.window_end, 10);
        CHECK_INT(sch.total_time, 5);
        CHECK_INT(sch.idle_time, 0);
        CHECK_DBL(sch.cpu_utilization, 100.0);
        CHECK_DBL(sch.throughput, 2.0 / 5.0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

/* ------------------------------------------------------------------- SJF */

static void test_sjf_classic(void)
{
    Dataset ds;
    Schedule sch;

    t_case("SJF (non-preemptive): P1(0,7) P2(2,4) P3(4,1) P4(5,4)");
    t_build(&ds, "sjf-classic", CLASSIC, 4);

    if (RUN(&sch, &ds, ALGO_SJF, 0)) {
        /* P1 holds the CPU to t=7; then P3 (burst 1), then P2 and P4 are
         * tied on burst 4 so the earlier arrival (P2) goes first. */
        CHECK_TIMELINE(&sch, "P1:0-7,P3:7-8,P2:8-12,P4:12-16");
        CHECK_PROC(&sch, "P1", 7, 7, 0, 0);
        CHECK_PROC(&sch, "P2", 12, 10, 6, 6);
        CHECK_PROC(&sch, "P3", 8, 4, 3, 3);
        CHECK_PROC(&sch, "P4", 16, 11, 7, 7);
        CHECK_DBL(sch.avg_turnaround, 8.0);
        CHECK_DBL(sch.avg_waiting, 4.0);
        CHECK_DBL(sch.avg_response, 4.0);
        CHECK_INT(sch.context_switches, 3);
        CHECK_INT(sch.preemptions, 0);
        CHECK_INT(sch.total_time, 16);
        CHECK_INT(sch.idle_time, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_sjf_idle_and_ties(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 2, 0 }, { "P2", 5, 1, 0 }, { "P3", 5, 3, 0 }
    };
    Dataset ds;
    Schedule sch;

    t_case("SJF across an idle gap: P1(0,2) P2(5,1) P3(5,3)");
    t_build(&ds, "sjf-idle", spec, 3);

    if (RUN(&sch, &ds, ALGO_SJF, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-2,IDLE:2-5,P2:5-6,P3:6-9");
        CHECK_PROC(&sch, "P1", 2, 2, 0, 0);
        CHECK_PROC(&sch, "P2", 6, 1, 0, 0);
        CHECK_PROC(&sch, "P3", 9, 4, 1, 1);
        CHECK_INT(sch.idle_time, 3);
        CHECK_INT(sch.busy_time, 6);
        CHECK_INT(sch.total_time, 9);
        CHECK_DBL(sch.cpu_utilization, 200.0 / 3.0);
        CHECK_INT(sch.context_switches, 2);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_sjf_tiebreak_input_order(void)
{
    /* Everything arrives together and bursts repeat, so only rule 3 of the
     * tie-break chain -- input order -- can decide. */
    static const PSpec spec[] = {
        { "A", 0, 4, 0 }, { "B", 0, 4, 0 }, { "C", 0, 2, 0 },
        { "D", 0, 2, 0 }, { "E", 0, 6, 0 }, { "F", 0, 6, 0 }
    };
    Dataset ds;
    Schedule sch;

    t_case("SJF tie-break falls back on input order");
    t_build(&ds, "sjf-ties", spec, 6);

    if (RUN(&sch, &ds, ALGO_SJF, 0)) {
        CHECK_TIMELINE(&sch,
                       "C:0-2,D:2-4,A:4-8,B:8-12,E:12-18,F:18-24");
        CHECK_INT(sch.context_switches, 5);
        CHECK_INT(sch.preemptions, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

/* ------------------------------------------------------------------ SRTF */

static void test_srtf_classic(void)
{
    Dataset ds;
    Schedule sch;

    t_case("SRTF (preemptive): P1(0,7) P2(2,4) P3(4,1) P4(5,4)");
    t_build(&ds, "srtf-classic", CLASSIC, 4);

    if (RUN(&sch, &ds, ALGO_SRTF, 0)) {
        CHECK_TIMELINE(&sch,
                       "P1:0-2,P2:2-4,P3:4-5,P2:5-7,P4:7-11,P1:11-16");
        CHECK_PROC(&sch, "P1", 16, 16, 9, 0);
        CHECK_PROC(&sch, "P2", 7, 5, 1, 0);
        CHECK_PROC(&sch, "P3", 5, 1, 0, 0);
        CHECK_PROC(&sch, "P4", 11, 6, 2, 2);
        CHECK_DBL(sch.avg_turnaround, 7.0);
        CHECK_DBL(sch.avg_waiting, 3.0);
        CHECK_DBL(sch.avg_response, 0.5);
        CHECK_INT(sch.context_switches, 5);
        CHECK_INT(sch.preemptions, 2);
        CHECK_INT(sch.total_time, 16);
        CHECK_INT(sch.idle_time, 0);
        CHECK_DBL(sch.cpu_utilization, 100.0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_srtf_no_switch_on_equal_remaining(void)
{
    /* P2 arrives at t=2 with burst 3; P1 also has 3 left. Equal is not
     * strictly smaller, so P1 must keep the CPU. */
    static const PSpec spec[] = { { "P1", 0, 5, 0 }, { "P2", 2, 3, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("SRTF does not preempt on an equal remaining time");
    t_build(&ds, "srtf-equal", spec, 2);

    if (RUN(&sch, &ds, ALGO_SRTF, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-5,P2:5-8");
        CHECK_INT(sch.preemptions, 0);
        CHECK_INT(sch.context_switches, 1);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_srtf_preemption_storm(void)
{
    /* Every arrival is shorter than what is running, so every arrival
     * preempts. */
    static const PSpec spec[] = {
        { "P1", 0, 9, 0 }, { "P2", 1, 7, 0 }, { "P3", 2, 5, 0 },
        { "P4", 3, 3, 0 }, { "P5", 4, 1, 0 }
    };
    Dataset ds;
    Schedule sch;

    t_case("SRTF preempts on every arrival");
    t_build(&ds, "srtf-storm", spec, 5);

    if (RUN(&sch, &ds, ALGO_SRTF, 0)) {
        CHECK_TIMELINE(&sch,
                       "P1:0-1,P2:1-2,P3:2-3,P4:3-4,P5:4-5,P4:5-7,"
                       "P3:7-11,P2:11-17,P1:17-25");
        CHECK_PROC(&sch, "P5", 5, 1, 0, 0);
        CHECK_PROC(&sch, "P4", 7, 4, 1, 0);
        CHECK_PROC(&sch, "P3", 11, 9, 4, 0);
        CHECK_PROC(&sch, "P2", 17, 16, 9, 0);
        CHECK_PROC(&sch, "P1", 25, 25, 16, 0);
        CHECK_INT(sch.preemptions, 4);
        CHECK_INT(sch.context_switches, 8);
        CHECK_DBL(sch.avg_response, 0.0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

/* -------------------------------------------------------------- priority */

static void test_priority_np(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 10, 3 }, { "P2", 0, 1, 1 }, { "P3", 0, 2, 4 },
        { "P4", 0, 1, 5 },  { "P5", 0, 5, 2 }
    };
    Dataset ds;
    Schedule sch;

    t_case("Priority (non-preemptive), all arriving at t=0");
    t_build(&ds, "prio-np", spec, 5);

    if (RUN(&sch, &ds, ALGO_PRIORITY_NP, 0)) {
        CHECK_TIMELINE(&sch, "P2:0-1,P5:1-6,P1:6-16,P3:16-18,P4:18-19");
        CHECK_PROC(&sch, "P1", 16, 16, 6, 6);
        CHECK_PROC(&sch, "P2", 1, 1, 0, 0);
        CHECK_PROC(&sch, "P3", 18, 18, 16, 16);
        CHECK_PROC(&sch, "P4", 19, 19, 18, 18);
        CHECK_PROC(&sch, "P5", 6, 6, 1, 1);
        CHECK_DBL(sch.avg_turnaround, 12.0);
        CHECK_DBL(sch.avg_waiting, 8.2);
        CHECK_DBL(sch.avg_response, 8.2);
        CHECK_INT(sch.context_switches, 4);
        CHECK_INT(sch.preemptions, 0);
        CHECK_INT(sch.total_time, 19);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_priority_preemptive(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 4, 2 }, { "P2", 1, 3, 1 }, { "P3", 2, 2, 3 }
    };
    Dataset ds;
    Schedule sch;

    t_case("Priority (preemptive): P2 displaces P1 at t=1");
    t_build(&ds, "prio-p", spec, 3);

    if (RUN(&sch, &ds, ALGO_PRIORITY_P, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-1,P2:1-4,P1:4-7,P3:7-9");
        CHECK_PROC(&sch, "P1", 7, 7, 3, 0);
        CHECK_PROC(&sch, "P2", 4, 3, 0, 0);
        CHECK_PROC(&sch, "P3", 9, 7, 5, 5);
        CHECK_DBL(sch.avg_turnaround, 17.0 / 3.0);
        CHECK_DBL(sch.avg_waiting, 8.0 / 3.0);
        CHECK_DBL(sch.avg_response, 5.0 / 3.0);
        CHECK_INT(sch.context_switches, 3);
        CHECK_INT(sch.preemptions, 1);
        CHECK_INT(sch.total_time, 9);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_priority_equal_does_not_preempt(void)
{
    static const PSpec spec[] = { { "P1", 0, 4, 2 }, { "P2", 1, 2, 2 } };
    Dataset ds;
    Schedule sch;

    t_case("Preemptive priority leaves an equal priority alone");
    t_build(&ds, "prio-equal", spec, 2);

    if (RUN(&sch, &ds, ALGO_PRIORITY_P, 0)) {
        CHECK_TIMELINE(&sch, "P1:0-4,P2:4-6");
        CHECK_INT(sch.preemptions, 0);
        CHECK_INT(sch.context_switches, 1);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

/* ------------------------------------------------------------ round robin */

static void test_rr_quantum2(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 5, 0 }, { "P2", 1, 4, 0 },
        { "P3", 2, 2, 0 }, { "P4", 4, 1, 0 }
    };
    Dataset ds;
    Schedule sch;

    t_case("Round Robin q=2: P1(0,5) P2(1,4) P3(2,2) P4(4,1)");
    t_build(&ds, "rr-q2", spec, 4);

    if (RUN(&sch, &ds, ALGO_RR, 2)) {
        /* Queue trace:
         *   t=0  [P1]                     run P1 0-2
         *   t=2  admit P2,P3 -> [P2,P3]; requeue P1 -> [P2,P3,P1]
         *        run P2 2-4
         *   t=4  admit P4 -> [P3,P1,P4]; requeue P2 -> [P3,P1,P4,P2]
         *        run P3 4-6 (finishes)    -> [P1,P4,P2]
         *        run P1 6-8, requeue      -> [P4,P2,P1]
         *        run P4 8-9 (finishes)    -> [P2,P1]
         *        run P2 9-11 (finishes)   -> [P1]
         *        run P1 11-12 (finishes)
         */
        CHECK_TIMELINE(&sch,
                       "P1:0-2,P2:2-4,P3:4-6,P1:6-8,P4:8-9,P2:9-11,P1:11-12");
        CHECK_PROC(&sch, "P1", 12, 12, 7, 0);
        CHECK_PROC(&sch, "P2", 11, 10, 6, 1);
        CHECK_PROC(&sch, "P3", 6, 4, 2, 2);
        CHECK_PROC(&sch, "P4", 9, 5, 4, 4);
        CHECK_DBL(sch.avg_turnaround, 31.0 / 4.0);
        CHECK_DBL(sch.avg_waiting, 19.0 / 4.0);
        CHECK_DBL(sch.avg_response, 7.0 / 4.0);
        CHECK_INT(sch.context_switches, 6);
        CHECK_INT(sch.preemptions, 3);
        CHECK_INT(sch.total_time, 12);
        CHECK_INT(sch.idle_time, 0);
        CHECK_DBL(sch.throughput, 4.0 / 12.0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rr_single_process_no_switch(void)
{
    static const PSpec spec[] = { { "P1", 0, 5, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("Round Robin q=2 with one process: slices must merge");
    t_build(&ds, "rr-single", spec, 1);

    if (RUN(&sch, &ds, ALGO_RR, 2)) {
        CHECK_TIMELINE(&sch, "P1:0-5");
        CHECK_INT(sch.seg_count, 1);
        CHECK_INT(sch.context_switches, 0);
        CHECK_INT(sch.preemptions, 0);
        CHECK_PROC(&sch, "P1", 5, 5, 0, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rr_quantum_larger_than_bursts(void)
{
    /* A quantum no smaller than every burst degenerates to FCFS. */
    static const PSpec spec[] = {
        { "P1", 0, 5, 0 }, { "P2", 1, 3, 0 }, { "P3", 2, 8, 0 }
    };
    Dataset ds;
    Schedule rr, fcfs;
    char a[256], b[256];

    t_case("Round Robin with a huge quantum equals FCFS");
    t_build(&ds, "rr-huge-q", spec, 3);

    if (RUN(&rr, &ds, ALGO_RR, 100) && RUN(&fcfs, &ds, ALGO_FCFS, 0)) {
        t_timeline(&rr, a, sizeof a);
        t_timeline(&fcfs, b, sizeof b);
        CHECK_STR(a, b);
        CHECK_INT(rr.preemptions, 0);
        CHECK_DBL(rr.avg_waiting, fcfs.avg_waiting);
    }
    schedule_free(&rr);
    schedule_free(&fcfs);
    dataset_free(&ds);
}

static void test_rr_quantum1(void)
{
    static const PSpec spec[] = { { "P1", 0, 3, 0 }, { "P2", 0, 3, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("Round Robin q=1 interleaves two equal processes");
    t_build(&ds, "rr-q1", spec, 2);

    if (RUN(&sch, &ds, ALGO_RR, 1)) {
        CHECK_TIMELINE(&sch,
                       "P1:0-1,P2:1-2,P1:2-3,P2:3-4,P1:4-5,P2:5-6");
        CHECK_PROC(&sch, "P1", 5, 5, 2, 0);
        CHECK_PROC(&sch, "P2", 6, 6, 3, 1);
        CHECK_INT(sch.context_switches, 5);
        CHECK_INT(sch.preemptions, 4);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rr_idle(void)
{
    static const PSpec spec[] = { { "P1", 0, 2, 0 }, { "P2", 5, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("Round Robin over an idle CPU");
    t_build(&ds, "rr-idle", spec, 2);

    if (RUN(&sch, &ds, ALGO_RR, 2)) {
        CHECK_TIMELINE(&sch, "P1:0-2,IDLE:2-5,P2:5-7");
        CHECK_INT(sch.idle_time, 3);
        CHECK_INT(sch.busy_time, 4);
        CHECK_INT(sch.total_time, 7);
        CHECK_INT(sch.context_switches, 1);
        CHECK_INT(sch.preemptions, 0);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

static void test_rr_arrival_at_quantum_boundary(void)
{
    /* P2 arrives at exactly t=2, the instant P1's quantum expires. The
     * documented rule puts the newcomer ahead of the expired process. */
    static const PSpec spec[] = { { "P1", 0, 4, 0 }, { "P2", 2, 2, 0 } };
    Dataset ds;
    Schedule sch;

    t_case("Round Robin: arrival exactly on the quantum boundary goes first");
    t_build(&ds, "rr-boundary", spec, 2);

    if (RUN(&sch, &ds, ALGO_RR, 2)) {
        CHECK_TIMELINE(&sch, "P1:0-2,P2:2-4,P1:4-6");
        CHECK_PROC(&sch, "P1", 6, 6, 2, 0);
        CHECK_PROC(&sch, "P2", 4, 2, 0, 0);
        CHECK_INT(sch.context_switches, 2);
        CHECK_INT(sch.preemptions, 1);
    }
    schedule_free(&sch);
    dataset_free(&ds);
}

/* ---------------------------------------------------- cross-cutting ---- */

static void test_single_process_all_algorithms(void)
{
    static const PSpec spec[] = { { "P1", 3, 4, 7 } };
    Dataset ds;
    int i;

    t_case("one process: every algorithm must agree");
    t_build(&ds, "single", spec, 1);

    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule sch;
        if (!RUN(&sch, &ds, (AlgorithmId)i, 2)) {
            schedule_free(&sch);
            continue;
        }
        CHECK_TIMELINE(&sch, "P1:3-7");
        CHECK_PROC(&sch, "P1", 7, 4, 0, 0);
        CHECK_INT(sch.context_switches, 0);
        CHECK_INT(sch.preemptions, 0);
        CHECK_INT(sch.idle_time, 0);
        CHECK_DBL(sch.cpu_utilization, 100.0);
        schedule_free(&sch);
    }
    dataset_free(&ds);
}

static void test_conservation_laws(void)
{
    /* Whatever the algorithm, the same workload must finish at the same
     * time, keep the CPU busy for the same number of units and give every
     * process exactly its burst. */
    static const PSpec spec[] = {
        { "P1", 0, 6, 2 }, { "P2", 1, 8, 1 }, { "P3", 2, 7, 3 },
        { "P4", 3, 3, 1 }, { "P5", 6, 4, 4 }, { "P6", 10, 5, 2 },
        { "P7", 12, 2, 0 }, { "P8", 15, 9, 3 }
    };
    Dataset ds;
    int i;
    int ref_end = -1, ref_busy = -1, ref_idle = -1;

    t_case("work conservation across all six algorithms");
    t_build(&ds, "mixed", spec, 8);

    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule sch;
        int k;
        if (!RUN(&sch, &ds, (AlgorithmId)i, 3)) {
            schedule_free(&sch);
            continue;
        }
        if (ref_end < 0) {
            ref_end = sch.window_end;
            ref_busy = sch.busy_time;
            ref_idle = sch.idle_time;
        }
        CHECK_MSG(sch.window_end == ref_end,
                  "%s makespan %d != %d", algo_abbrev((AlgorithmId)i),
                  sch.window_end, ref_end);
        CHECK_MSG(sch.busy_time == ref_busy,
                  "%s busy %d != %d", algo_abbrev((AlgorithmId)i),
                  sch.busy_time, ref_busy);
        CHECK_MSG(sch.idle_time == ref_idle,
                  "%s idle %d != %d", algo_abbrev((AlgorithmId)i),
                  sch.idle_time, ref_idle);
        CHECK_MSG(sch.busy_time == dataset_total_burst(&ds),
                  "%s busy time must equal the total burst",
                  algo_abbrev((AlgorithmId)i));
        CHECK_MSG(sch.busy_time + sch.idle_time == sch.total_time,
                  "%s busy+idle must fill the window",
                  algo_abbrev((AlgorithmId)i));
        for (k = 0; k < sch.n; k++) {
            CHECK_MSG(sch.stats[k].executed == sch.procs[k].burst,
                      "%s gave %s %d of %d units",
                      algo_abbrev((AlgorithmId)i), sch.procs[k].id,
                      sch.stats[k].executed, sch.procs[k].burst);
            CHECK_MSG(sch.stats[k].waiting >= 0,
                      "%s produced a negative waiting time for %s",
                      algo_abbrev((AlgorithmId)i), sch.procs[k].id);
            CHECK_MSG(sch.stats[k].response >= 0,
                      "%s produced a negative response time for %s",
                      algo_abbrev((AlgorithmId)i), sch.procs[k].id);
            CHECK_MSG(sch.stats[k].response <= sch.stats[k].waiting,
                      "%s: response must never exceed waiting for %s",
                      algo_abbrev((AlgorithmId)i), sch.procs[k].id);
        }
        schedule_free(&sch);
    }
    dataset_free(&ds);
}

static void test_determinism(void)
{
    static const PSpec spec[] = {
        { "P1", 0, 4, 2 }, { "P2", 0, 4, 2 }, { "P3", 0, 2, 1 },
        { "P4", 0, 2, 1 }, { "P5", 0, 6, 3 }, { "P6", 0, 6, 3 }
    };
    Dataset ds;
    int i;

    t_case("running twice produces an identical timeline");
    t_build(&ds, "ties", spec, 6);

    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule a, b;
        char sa[512], sb[512];
        if (!RUN(&a, &ds, (AlgorithmId)i, 2) ||
            !RUN(&b, &ds, (AlgorithmId)i, 2)) {
            schedule_free(&a);
            schedule_free(&b);
            continue;
        }
        t_timeline(&a, sa, sizeof sa);
        t_timeline(&b, sb, sizeof sb);
        CHECK_STR(sa, sb);
        schedule_free(&a);
        schedule_free(&b);
    }
    dataset_free(&ds);
}

static void test_sjf_optimality(void)
{
    /* With every process present at t=0, SJF minimises average waiting
     * time; no other algorithm may beat it on this workload. */
    static const PSpec spec[] = {
        { "P1", 0, 9, 1 }, { "P2", 0, 2, 2 }, { "P3", 0, 7, 3 },
        { "P4", 0, 4, 4 }, { "P5", 0, 1, 5 }
    };
    Dataset ds;
    Schedule sjf;
    int i;

    t_case("SJF minimises average waiting time when all arrive together");
    t_build(&ds, "sjf-optimal", spec, 5);

    if (!RUN(&sjf, &ds, ALGO_SJF, 2)) {
        schedule_free(&sjf);
        dataset_free(&ds);
        return;
    }
    CHECK_TIMELINE(&sjf, "P5:0-1,P2:1-3,P4:3-7,P3:7-14,P1:14-23");
    CHECK_DBL(sjf.avg_waiting, 25.0 / 5.0);

    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule other;
        if (!RUN(&other, &ds, (AlgorithmId)i, 2)) {
            schedule_free(&other);
            continue;
        }
        CHECK_MSG(other.avg_waiting >= sjf.avg_waiting - 1e-9,
                  "%s beat SJF on average waiting time (%.4f < %.4f)",
                  algo_abbrev((AlgorithmId)i), other.avg_waiting,
                  sjf.avg_waiting);
        schedule_free(&other);
    }
    schedule_free(&sjf);
    dataset_free(&ds);
}

void suite_algorithms(void)
{
    t_suite("algorithms");
    test_fcfs_basic();
    test_fcfs_idle();
    test_fcfs_late_start();
    test_sjf_classic();
    test_sjf_idle_and_ties();
    test_sjf_tiebreak_input_order();
    test_srtf_classic();
    test_srtf_no_switch_on_equal_remaining();
    test_srtf_preemption_storm();
    test_priority_np();
    test_priority_preemptive();
    test_priority_equal_does_not_preempt();
    test_rr_quantum2();
    test_rr_single_process_no_switch();
    test_rr_quantum_larger_than_bursts();
    test_rr_quantum1();
    test_rr_idle();
    test_rr_arrival_at_quantum_boundary();
    test_single_process_all_algorithms();
    test_conservation_laws();
    test_determinism();
    test_sjf_optimality();
}
