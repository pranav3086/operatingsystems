/*
 * test_predict.c -- The statistical predictor and its two dashboard routes.
 *
 * Closed-form results (rates, Poisson probabilities, burst statistics, the
 * schedules) are checked exactly, against values worked out by hand. Random
 * output is checked two ways: bit-for-bit reproducibility under a fixed
 * seed, and agreement with the theoretical distribution within a stated
 * tolerance over many seeds -- never by expecting one random sample to hit a
 * probability exactly.
 *
 * The workload used throughout is the textbook one from the algorithm
 * suite: P1(0,7,2) P2(2,4,1) P3(4,1,3) P4(5,4,2).
 *   arrival gaps   2, 2, 1 over t=0..5  -> lambda = 3 / 5 = 0.6
 *   bursts         7, 4, 1, 4           -> mean 4, sd sqrt(18/3) = sqrt(6)
 */
#include "suites.h"
#include "test_util.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "datasets.h"
#include "http.h"
#include "json.h"
#include "predict.h"
#include "validate.h"
#include "webapp.h"

/* |got - want| <= tol, for statistics that only converge. */
#define CHECK_NEAR(got, want, tol)                                          \
    t_check(fabs((double)(got) - (double)(want)) <= (tol), __FILE__,        \
            __LINE__, "%s = %.6f, expected %.6f +/- %g", #got,              \
            (double)(got), (double)(want), (double)(tol))

static const PSpec CLASSIC[] = {
    { "P1", 0, 7, 2 }, { "P2", 2, 4, 1 }, { "P3", 4, 1, 3 }, { "P4", 5, 4, 2 }
};

static bool has_warning(const ForecastResult *r, const char *needle)
{
    int i;
    for (i = 0; i < r->warn_count; i++)
        if (strstr(r->warnings[i], needle) != NULL) return true;
    return false;
}

/* ───────────────────────────── the formulas ─────────────────────── */

static void test_rate(void)
{
    double l = -1.0;

    t_case("arrival rate: lambda = N / T");
    CHECK(predict_rate(4, 16.0, &l));  CHECK_DBL(l, 0.25);
    CHECK(predict_rate(3, 5.0, &l));   CHECK_DBL(l, 0.6);
    CHECK(predict_rate(0, 10.0, &l));  CHECK_DBL(l, 0.0);

    t_case("arrival rate: zero or invalid exposure is refused");
    CHECK(!predict_rate(3, 0.0, &l));
    CHECK(!predict_rate(3, -2.0, &l));
    CHECK(!predict_rate(-1, 5.0, &l));
    CHECK(!predict_rate(3, NAN, &l));
    CHECK(!predict_rate(3, INFINITY, &l));
}

static void test_poisson(void)
{
    double sum = 0.0;
    int k;

    t_case("Poisson pmf against hand-computed values (mu = 2)");
    CHECK_DBL(predict_poisson_pmf(0, 2.0), 0.1353352832);
    CHECK_DBL(predict_poisson_pmf(1, 2.0), 0.2706705665);
    CHECK_DBL(predict_poisson_pmf(2, 2.0), 0.2706705665);
    CHECK_DBL(predict_poisson_pmf(3, 2.0), 0.1804470443);
    CHECK_DBL(predict_poisson_pmf(-1, 2.0), 0.0);

    t_case("Poisson pmf sums to one, expected count is mu");
    {
        double mean = 0.0;
        for (k = 0; k <= 80; k++) {
            double p = predict_poisson_pmf(k, 3.0);
            sum += p;
            mean += k * p;
        }
        CHECK_NEAR(sum, 1.0, 1e-12);
        CHECK_NEAR(mean, 3.0, 1e-10);
    }

    t_case("Poisson cdf and the central 90% range (mu = 3)");
    CHECK_DBL(predict_poisson_cdf(0, 3.0), exp(-3.0));
    CHECK_DBL(predict_poisson_cdf(1, 3.0), 4.0 * exp(-3.0));
    /* cdf(0)=.0498 < .05 <= cdf(1)=.1991;  cdf(5)=.9161 < .95 <= cdf(6) */
    CHECK_INT(predict_poisson_quantile(0.05, 3.0), 1);
    CHECK_INT(predict_poisson_quantile(0.95, 3.0), 6);
    CHECK_DBL(predict_poisson_cdf(-1, 3.0), 0.0);

    t_case("Poisson edge cases: mu = 0, invalid mu, a very large mean");
    CHECK_DBL(predict_poisson_pmf(0, 0.0), 1.0);
    CHECK_DBL(predict_poisson_pmf(1, 0.0), 0.0);
    CHECK_DBL(predict_poisson_cdf(0, 0.0), 1.0);
    CHECK_INT(predict_poisson_quantile(0.95, 0.0), 0);
    CHECK(isnan(predict_poisson_pmf(1, -1.0)));
    CHECK(isnan(predict_poisson_pmf(1, NAN)));
    CHECK(isnan(predict_poisson_cdf(1, INFINITY)));
    CHECK_INT(predict_poisson_quantile(0.5, -3.0), -1);
    /* mu = 1e6: (mu^k e^-mu)/k! would overflow if computed directly. The
     * mode's mass is ~ 1/sqrt(2 pi mu). */
    CHECK_NEAR(predict_poisson_pmf(1000000, 1e6),
               1.0 / sqrt(2.0 * 3.14159265358979 * 1e6), 1e-6);
    CHECK_NEAR(predict_poisson_cdf(1000000, 1e6), 0.5, 0.01);
    CHECK_NEAR(predict_poisson_quantile(0.5, 1e6), 1e6, 2.0);
    CHECK_DBL(predict_poisson_cdf(2000000, 1e6), 1.0);
}

static void test_exponential(void)
{
    PredictRng r;
    double sum = 0.0, below = 0.0, umin = 1.0, umax = 0.0, usum = 0.0;
    int i;
    const int N = 20000;

    t_case("exponential gap: -ln(U) / lambda");
    CHECK_DBL(predict_exp_gap(exp(-1.0), 0.5), 2.0);
    CHECK_DBL(predict_exp_gap(0.5, 1.0), log(2.0));

    t_case("exponential gap: U outside (0,1) or a bad rate is refused");
    CHECK(predict_exp_gap(0.0, 1.0) < 0.0);
    CHECK(predict_exp_gap(1.0, 1.0) < 0.0);
    CHECK(predict_exp_gap(NAN, 1.0) < 0.0);
    CHECK(predict_exp_gap(0.5, 0.0) < 0.0);
    CHECK(predict_exp_gap(0.5, -1.0) < 0.0);
    CHECK(predict_exp_gap(0.5, INFINITY) < 0.0);

    t_case("uniform stream stays strictly inside (0,1)");
    predict_rng_seed(&r, 7);
    for (i = 0; i < 100000; i++) {
        double u = predict_rng_uniform(&r);
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        usum += u;
    }
    CHECK(umin > 0.0);
    CHECK(umax < 1.0);
    CHECK_NEAR(usum / 100000.0, 0.5, 0.005);   /* sd of mean ~ 0.0009 */

    t_case("exponential gaps: mean 1/lambda, median ln2/lambda (seeded)");
    predict_rng_seed(&r, 42);
    for (i = 0; i < N; i++) {
        double g = predict_exp_gap(predict_rng_uniform(&r), 0.5);
        sum += g;
        if (g < log(2.0) / 0.5) below += 1.0;
    }
    /* mean 2, sd of the mean 2/sqrt(20000) = 0.014: tolerance ~4 sd */
    CHECK_NEAR(sum / N, 2.0, 0.06);
    /* half fall below the median; sd 0.0035 */
    CHECK_NEAR(below / N, 0.5, 0.015);
}

static void test_arrival_generator(void)
{
    PredictRng r;
    int out[200], i, n;
    bool tr = true;

    t_case("arrival generator: zero / invalid rate yields nothing, safely");
    predict_rng_seed(&r, 1);
    CHECK_INT(predict_arrivals(0.0, 10, 50, 200, &r, out, &tr), 0);
    CHECK(!tr);
    CHECK_INT(predict_arrivals(NAN, 10, 50, 200, &r, out, &tr), 0);
    CHECK_INT(predict_arrivals(-1.0, 10, 50, 200, &r, out, &tr), 0);
    CHECK_INT(predict_arrivals(1.0, 10, 0, 200, &r, out, &tr), 0);

    t_case("arrival generator: whole time units inside (start, start+h]");
    predict_rng_seed(&r, 3);
    n = predict_arrivals(5.0, 10, 3, 200, &r, out, &tr);
    CHECK_MSG(n > 0, "a rate of 5/unit over 3 units produced no arrivals");
    CHECK(!tr);
    for (i = 0; i < n; i++) {
        CHECK_MSG(out[i] >= 11 && out[i] <= 13,
                  "arrival %d at t=%d is outside 11..13", i, out[i]);
        if (i > 0) CHECK_MSG(out[i] >= out[i - 1], "arrivals out of order");
    }

    t_case("arrival generator: a limit truncates and says so");
    predict_rng_seed(&r, 3);
    n = predict_arrivals(5.0, 10, 100, 7, &r, out, &tr);
    CHECK_INT(n, 7);
    CHECK(tr);
}

static void test_burst_summary(void)
{
    Dataset ds;
    double m = 0.0, sd = 0.0;
    bool ok = false;

    t_case("burst mean and sample standard deviation");
    t_build(&ds, "classic", CLASSIC, 4);
    CHECK(predict_burst_summary(&ds, &m, &sd, &ok));
    CHECK_DBL(m, 4.0);
    CHECK(ok);
    CHECK_DBL(sd, sqrt(6.0));
    dataset_free(&ds);

    t_case("one burst: no standard deviation; empty: refused");
    t_build(&ds, "one", CLASSIC, 1);
    CHECK(predict_burst_summary(&ds, &m, &sd, &ok));
    CHECK_DBL(m, 7.0);
    CHECK(!ok);
    dataset_free(&ds);
    dataset_init(&ds, "empty");
    CHECK(!predict_burst_summary(&ds, &m, &sd, &ok));
    dataset_free(&ds);
}

static void test_tests(void)
{
    t_case("exact binomial test");
    CHECK_DBL(predict_binom_two_sided(5, 10, 0.5), 1.0);
    CHECK_DBL(predict_binom_two_sided(0, 10, 0.5), 2.0 / 1024.0);
    CHECK_DBL(predict_binom_two_sided(8, 10, 0.5), 112.0 / 1024.0);

    t_case("incomplete gamma and the chi-square tail");
    CHECK_DBL(predict_gamma_p(1.0, 0.5), 1.0 - exp(-0.5));   /* series */
    CHECK_DBL(predict_gamma_p(1.0, 3.0), 1.0 - exp(-3.0));   /* cont. fr. */
    CHECK_DBL(predict_chi2_sf(3.0, 2), exp(-1.5));
    CHECK_DBL(predict_chi2_sf(40.0, 2), exp(-20.0));
    CHECK_NEAR(predict_chi2_sf(11.0705, 5), 0.05, 1e-4);     /* table value */
    CHECK_DBL(predict_chi2_sf(0.0, 4), 1.0);
}

/* ───────────────────────────── the forecast ─────────────────────── */

static void test_forecast_classic(void)
{
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];

    t_build(&ds, "classic", CLASSIC, 4);

    t_case("no window given: rate from the gaps, assumption stated");
    forecast_config_default(&cfg);
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_INT(r.mode, OBS_FROM_ARRIVAL_GAPS);
    CHECK_INT(r.obs_start, 0);
    CHECK_INT(r.obs_end, 5);
    CHECK_INT(r.exposure, 5);
    CHECK_INT(r.processes, 4);
    CHECK_INT(r.observed_events, 3);
    CHECK_INT(r.intervals, 3);
    CHECK_DBL(r.lambda, 0.6);
    CHECK_DBL(r.lambda_se, 0.6 / sqrt(3.0));
    CHECK_DBL(r.mean_gap, 5.0 / 3.0);
    CHECK_MSG(strstr(r.assumption, "Assumed") != NULL,
              "the inferred window must be labelled as an assumption");

    t_case("count model over the default horizon (= window, 5 units)");
    CHECK_INT(r.horizon, 5);
    CHECK_INT(r.forecast_start, 5);
    CHECK_INT(r.forecast_end, 10);
    CHECK_DBL(r.expected, 3.0);
    CHECK_DBL(r.p_zero, exp(-3.0));
    CHECK_DBL(r.p_at_least_one, 1.0 - exp(-3.0));
    CHECK_INT(r.range_low, 1);
    CHECK_INT(r.range_high, 6);
    CHECK_INT(r.cap, MAX_PROCESSES - 4);
    CHECK(!r.cap_from_user);

    t_case("burst and priority summary");
    CHECK_DBL(r.burst_mean, 4.0);
    CHECK(r.burst_sd_valid);
    CHECK_DBL(r.burst_sd, sqrt(6.0));
    CHECK_INT(r.burst_min, 1);
    CHECK_INT(r.burst_max, 7);
    CHECK_INT(r.burst_distinct, 3);
    CHECK_INT(r.priority_min, 1);
    CHECK_INT(r.priority_max, 3);
    CHECK_INT(r.priority_distinct, 3);
    CHECK_INT(r.seed, 42);

    t_case("four processes are flagged as too few to rely on");
    CHECK(has_warning(&r, "Only 3 gaps between arrivals were observed"));
    CHECK(!r.split_valid);     /* two interior arrivals: too few to test */
    CHECK(!r.disp_valid);
    CHECK_INT(r.simultaneous, 0);
    CHECK_INT(r.generated, pred.count);
    dataset_free(&pred);

    t_case("explicit window [0,10]: lambda = 4 / 10");
    cfg.mode = OBS_EXPLICIT;
    cfg.obs_start = 0;
    cfg.obs_end = 10;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_DBL(r.lambda, 0.4);
    CHECK_INT(r.observed_events, 4);
    CHECK_INT(r.exposure, 10);
    CHECK_INT(r.forecast_start, 10);
    CHECK_INT(r.forecast_end, 20);
    CHECK_DBL(r.expected, 4.0);
    CHECK(has_warning(&r, "Only 4 arrivals were observed"));
    dataset_free(&pred);

    dataset_free(&ds);
}

static void test_forecast_rejects(void)
{
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];

    t_case("insufficient data: one arrival, or all arrivals at once");
    t_build(&ds, "one", CLASSIC, 1);
    forecast_config_default(&cfg);
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_MSG(strstr(err, "observation window") != NULL, "%s", err);
    CHECK_INT(pred.count, 0);
    dataset_free(&pred);
    /* ...but with a stated window a single arrival is a valid sample */
    cfg.mode = OBS_EXPLICIT;
    cfg.obs_start = 0;
    cfg.obs_end = 20;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_DBL(r.lambda, 0.05);
    CHECK(has_warning(&r, "Only 1 arrival was observed"));
    CHECK(has_warning(&r, "Only one burst time"));
    dataset_free(&pred);
    dataset_free(&ds);

    {
        static const PSpec SAME[] = {
            { "A", 3, 2, 0 }, { "B", 3, 5, 0 }, { "C", 3, 1, 0 }
        };
        t_build(&ds, "same", SAME, 3);
        forecast_config_default(&cfg);
        CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
        CHECK_MSG(strstr(err, "zero time") != NULL, "%s", err);
        dataset_free(&pred);
        dataset_free(&ds);
    }

    t_build(&ds, "classic", CLASSIC, 4);

    t_case("invalid observation windows");
    forecast_config_default(&cfg);
    cfg.mode = OBS_EXPLICIT;
    cfg.obs_start = 0;
    cfg.obs_end = 4;                          /* P4 arrives at 5 */
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_MSG(strstr(err, "contain every observed arrival") != NULL, "%s",
              err);
    dataset_free(&pred);
    cfg.obs_start = 3;
    cfg.obs_end = 3;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_MSG(strstr(err, "end after it starts") != NULL, "%s", err);
    dataset_free(&pred);
    cfg.obs_start = 1;                        /* P1 arrives at 0 */
    cfg.obs_end = 9;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
    cfg.obs_start = 0;
    cfg.obs_end = MAX_ARRIVAL + 1;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);

    t_case("invalid horizons, caps and seeds");
    forecast_config_default(&cfg);
    cfg.horizon = -1;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
    cfg.horizon = MAX_ARRIVAL;                /* 5 + 100000 > 100000 */
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_MSG(strstr(err, "runs past") != NULL, "%s", err);
    dataset_free(&pred);
    cfg.horizon = MAX_ARRIVAL - 5;            /* exactly reaches the limit */
    cfg.max_generated = 3;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
    forecast_config_default(&cfg);
    cfg.max_generated = MAX_PROCESSES - 4 + 1;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
    cfg.max_generated = -2;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
    forecast_config_default(&cfg);
    cfg.seed = -1;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
#if LONG_MAX > 2147483647L      /* long is 32 bits on Windows */
    cfg.seed = PREDICT_MAX_SEED + 1L;
    CHECK(!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);
#endif
    cfg.seed = PREDICT_MAX_SEED;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    dataset_free(&pred);

    dataset_free(&ds);
}

/* Four arrivals, one per unit: lambda = 3/3 = 1, so a 20-unit horizon
 * expects 20 predicted processes. Ids are chosen to collide with the
 * predictor's own F1, F2, F3 -- case-insensitively for f2. */
static const PSpec CLASHING[] = {
    { "F1", 0, 3, 4 }, { "f2", 1, 5, 1 }, { "F3", 2, 2, 7 }, { "P1", 3, 9, 2 }
};

static void test_forecast_records(void)
{
    Dataset ds, pred, merged;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];
    int i, j;

    t_build(&ds, "clashing", CLASHING, 4);
    forecast_config_default(&cfg);
    cfg.horizon = 20;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_MSG(pred.count >= 5, "expected ~20 arrivals, got %d", pred.count);

    t_case("predicted ids are unique and never clash with observed ids");
    if (pred.count > 0) CHECK_STR(pred.items[0].id, "F4");
    for (i = 0; i < pred.count; i++) {
        CHECK_MSG(dataset_find_id(&ds, pred.items[i].id) < 0,
                  "%s clashes with an observed id", pred.items[i].id);
        for (j = 0; j < i; j++)
            CHECK_MSG(strcmp(pred.items[i].id, pred.items[j].id) != 0,
                      "%s appears twice", pred.items[i].id);
    }

    t_case("arrivals inside the horizon; bursts and priorities observed");
    for (i = 0; i < pred.count; i++) {
        const Process *p = &pred.items[i];
        bool seen = false;
        char e[ERR_MSG_MAX];
        CHECK_MSG(validate_process(p, e, sizeof e), "%s: %s", p->id, e);
        CHECK_MSG(p->arrival > r.forecast_start && p->arrival <= r.forecast_end,
                  "%s arrives at %d, outside (%d,%d]", p->id, p->arrival,
                  r.forecast_start, r.forecast_end);
        /* resampled as a pair, so the (burst, priority) pair was observed */
        for (j = 0; j < ds.count; j++)
            if (ds.items[j].burst == p->burst
                && ds.items[j].priority == p->priority) seen = true;
        CHECK_MSG(seen, "%s has burst %d / priority %d, never observed",
                  p->id, p->burst, p->priority);
        if (i > 0) CHECK(p->arrival >= pred.items[i - 1].arrival);
    }

    t_case("tied predicted arrivals are counted, and the merge is valid");
    {
        int ties = 0;
        for (i = 0; i < pred.count; i++) {
            int a = pred.items[i].arrival;
            if ((i > 0 && pred.items[i - 1].arrival == a)
                || (i + 1 < pred.count && pred.items[i + 1].arrival == a))
                ties++;
        }
        CHECK_INT(r.tied_arrivals, ties);
    }
    CHECK(forecast_merge(&ds, &pred, &merged, err, sizeof err));
    CHECK_INT(merged.count, ds.count + pred.count);
    CHECK(validate_dataset(&merged, err, sizeof err));
    for (i = 0; i < ds.count; i++)
        CHECK_STR(merged.items[i].id, ds.items[i].id);   /* originals first */

    t_case("the merged workload runs under all six algorithms");
    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule sch;
        char rerr[200];
        bool ok = algo_run(&sch, &merged, (AlgorithmId)i, 2, false, rerr,
                           sizeof rerr);
        CHECK_MSG(ok, "%s failed on the forecast workload: %s",
                  algo_abbrev((AlgorithmId)i), rerr);
        if (ok) {
            CHECK_INT(sch.n, merged.count);
            CHECK(sch.finalized);
            CHECK_INT(sch.busy_time, dataset_total_burst(&merged));
        }
        schedule_free(&sch);
    }
    dataset_free(&merged);

    t_case("merge refuses an id clash");
    {
        Dataset bad;
        Process p;
        dataset_init(&bad, "bad");
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "p1");
        p.arrival = 30;
        p.burst = 2;
        CHECK(dataset_add(&bad, &p));
        CHECK(!forecast_merge(&ds, &bad, &merged, err, sizeof err));
        CHECK_MSG(strstr(err, "already in use") != NULL, "%s", err);
        dataset_free(&merged);
        dataset_free(&bad);
    }

    dataset_free(&pred);
    dataset_free(&ds);
}

static bool same_dataset(const Dataset *a, const Dataset *b)
{
    int i;
    if (a->count != b->count) return false;
    for (i = 0; i < a->count; i++) {
        if (strcmp(a->items[i].id, b->items[i].id) != 0
            || a->items[i].arrival != b->items[i].arrival
            || a->items[i].burst != b->items[i].burst
            || a->items[i].priority != b->items[i].priority) return false;
    }
    return true;
}

static void test_reproducibility(void)
{
    Dataset ds, a, b, before, after;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];

    t_build(&ds, "clashing", CLASHING, 4);
    forecast_config_default(&cfg);
    cfg.horizon = 20;

    t_case("same workload, settings and seed: identical forecast");
    CHECK(forecast_run(&ds, &cfg, &a, &r, err, sizeof err));
    CHECK(forecast_run(&ds, &cfg, &b, &r, err, sizeof err));
    CHECK(same_dataset(&a, &b));
    dataset_free(&b);

    t_case("a different seed gives a different forecast");
    cfg.seed = 43;
    CHECK(forecast_run(&ds, &cfg, &b, &r, err, sizeof err));
    CHECK(!same_dataset(&a, &b));
    dataset_free(&b);
    dataset_free(&a);

    t_case("the predictor leaves the engine's own generator alone");
    CHECK(dataset_random(&before, 8, 7, 20, 9, 5));
    cfg.seed = 7;
    CHECK(forecast_run(&ds, &cfg, &a, &r, err, sizeof err));
    CHECK(dataset_random(&after, 8, 7, 20, 9, 5));
    CHECK(same_dataset(&before, &after));
    dataset_free(&a);
    dataset_free(&before);
    dataset_free(&after);
    dataset_free(&ds);
}

static void test_count_distribution(void)
{
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];
    const int RUNS = 2000;
    double sum = 0.0, sumsq = 0.0, zeros = 0.0, within = 0.0;
    long s;

    t_case("over 2000 seeds the generated count behaves as Poisson(3)");
    t_build(&ds, "classic", CLASSIC, 4);
    forecast_config_default(&cfg);         /* lambda 0.6, horizon 5 */
    for (s = 0; s < RUNS; s++) {
        int k;
        cfg.seed = s;
        if (!forecast_run(&ds, &cfg, &pred, &r, err, sizeof err)) {
            CHECK_MSG(false, "seed %ld failed: %s", s, err);
            break;
        }
        k = r.generated;
        sum += k;
        sumsq += (double)k * k;
        if (k == 0) zeros += 1.0;
        if (k >= r.range_low && k <= r.range_high) within += 1.0;
        dataset_free(&pred);
    }
    {
        double mean = sum / RUNS;
        double var = (sumsq - sum * sum / RUNS) / (RUNS - 1);
        /* sd of the mean sqrt(3/2000) = 0.039; of the variance ~0.10;
         * of P(K=0) 0.0049 -- every tolerance is about four of them. */
        CHECK_NEAR(mean, 3.0, 0.16);
        CHECK_NEAR(var, 3.0, 0.45);
        CHECK_NEAR(zeros / RUNS, exp(-3.0), 0.02);
        /* P(1 <= K <= 6) = cdf(6) - cdf(0) = 0.9665 - 0.0498 */
        CHECK_NEAR(within / RUNS, predict_poisson_cdf(6, 3.0) - exp(-3.0),
                   0.02);
    }
    dataset_free(&ds);
}

static void test_truncation(void)
{
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];
    PSpec ten[10];
    char ids[10][4];
    int i;

    for (i = 0; i < 10; i++) {
        snprintf(ids[i], sizeof ids[i], "P%d", i);
        ten[i].id = ids[i];
        ten[i].arrival = i;
        ten[i].burst = 1 + i % 3;
        ten[i].priority = 0;
    }
    t_build(&ds, "ten", ten, 10);

    t_case("a user cap censors the count, and is reported");
    forecast_config_default(&cfg);           /* lambda = 9/9 = 1 */
    cfg.horizon = 50;
    cfg.max_generated = 3;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_INT(r.generated, 3);
    CHECK(r.truncated);
    CHECK(r.cap_from_user);
    CHECK(has_warning(&r, "your cap"));
    CHECK(r.p_cap_exceeded > 0.999);
    dataset_free(&pred);

    t_case("a huge expected count stops at the engine's process limit");
    forecast_config_default(&cfg);
    cfg.horizon = MAX_ARRIVAL - 9;           /* expects ~100000 arrivals */
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_INT(r.generated, MAX_PROCESSES - 10);
    CHECK(r.truncated);
    CHECK(has_warning(&r, "process limit"));
    CHECK(has_warning(&r, "times the observation window"));
    dataset_free(&pred);
    dataset_free(&ds);
}

static void test_diagnostics(void)
{
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult r;
    char err[200];
    PSpec spec[20];
    char ids[20][4];
    int i;

    for (i = 0; i < 20; i++) {
        snprintf(ids[i], sizeof ids[i], "P%d", i);
        spec[i].id = ids[i];
        spec[i].burst = 2;
        spec[i].priority = 0;
    }

    t_case("all arrivals early in the window: rate change detected");
    for (i = 0; i < 20; i++) spec[i].arrival = i;     /* t=0..19 */
    t_build(&ds, "front-loaded", spec, 20);
    forecast_config_default(&cfg);
    cfg.mode = OBS_EXPLICIT;
    cfg.obs_start = 0;
    cfg.obs_end = 100;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK(r.split_valid);
    CHECK_INT(r.split_first, 20);
    CHECK_INT(r.split_second, 0);
    CHECK_DBL(r.split_p, 2.0 / 1048576.0);           /* 2 * 0.5^20 */
    CHECK(has_warning(&r, "differs between the first half"));
    CHECK(r.disp_valid);
    CHECK(r.disp_p_over < 0.05);
    CHECK(has_warning(&r, "burstier than Poisson"));
    CHECK(!has_warning(&r, "Only"));                 /* 20 >= 10 */
    CHECK(has_warning(&r, "Every observed burst is 2"));
    dataset_free(&pred);
    dataset_free(&ds);

    t_case("evenly spaced arrivals: more regular than Poisson");
    for (i = 0; i < 20; i++) spec[i].arrival = 2 * i;  /* t=0,2,..,38 */
    t_build(&ds, "metronome", spec, 20);
    cfg.obs_end = 40;
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_INT(r.split_first, 10);
    CHECK_DBL(r.split_p, 1.0);
    CHECK(!has_warning(&r, "differs between"));
    /* 6 bins of counts 4,3,3,4,3,3: D = (4/3) / (10/3) = 0.4 on 5 df */
    CHECK_INT(r.disp_bins, 6);
    CHECK_DBL(r.disp_vmr, 0.08);
    CHECK(r.disp_p_under < 0.05);
    CHECK(has_warning(&r, "more evenly spaced"));
    dataset_free(&pred);
    dataset_free(&ds);

    t_case("simultaneous arrivals are reported");
    for (i = 0; i < 20; i++) spec[i].arrival = i / 2;  /* pairs */
    t_build(&ds, "pairs", spec, 20);
    forecast_config_default(&cfg);
    CHECK(forecast_run(&ds, &cfg, &pred, &r, err, sizeof err));
    CHECK_INT(r.simultaneous, 20);
    CHECK(has_warning(&r, "share an arrival time"));
    dataset_free(&pred);
    dataset_free(&ds);
}

/* ───────────────────────────── the routes ───────────────────────── */

static int route(const char *method, const char *path, const char *body,
                 JsonValue **out)
{
    WebApp app;
    HttpResponse res;
    char err[JSON_ERR_MAX];
    int status;

    memset(&app, 0, sizeof app);
    memset(&res, 0, sizeof res);
    webapp_route(&app, method, path, body, &res);
    status = res.status;
    *out = NULL;
    if (res.body != NULL) (void)json_parse(res.body, out, err, sizeof err);
    if (res.body_owned) free(res.body);
    return status;
}

#define CLASSIC_PROCS                                                   \
    "\"processes\":["                                                   \
    "{\"id\":\"P1\",\"arrival\":0,\"burst\":7,\"priority\":2},"         \
    "{\"id\":\"P2\",\"arrival\":2,\"burst\":4,\"priority\":1},"         \
    "{\"id\":\"P3\",\"arrival\":4,\"burst\":1,\"priority\":3},"         \
    "{\"id\":\"P4\",\"arrival\":5,\"burst\":4,\"priority\":2}]"

static double member_dbl(const JsonValue *o, const char *key)
{
    double d = NAN;
    (void)json_member_double(o, key, &d);
    return d;
}

static int member_int(const JsonValue *o, const char *key)
{
    int n = -999;
    (void)json_member_int(o, key, &n);
    return n;
}

static void test_route_forecast(void)
{
    JsonValue *v = NULL;

    t_case("POST /api/forecast: statistics and predicted processes");
    CHECK_INT(route("POST", "/api/forecast", "{" CLASSIC_PROCS "}", &v), 200);
    if (v == NULL) return;
    {
        const JsonValue *o = json_get(v, "observation");
        const JsonValue *f = json_get(v, "forecast");
        const JsonValue *b = json_get(v, "bursts");
        const JsonValue *pred = json_get(v, "predicted");
        int i;

        CHECK_STR(json_as_string(json_get(o, "mode")), "arrival-gaps");
        CHECK_INT(member_int(o, "duration"), 5);
        CHECK_INT(member_int(o, "intervals"), 3);
        CHECK(json_as_string(json_get(o, "assumption")) != NULL);
        CHECK_DBL(member_dbl(json_get(v, "rate"), "lambda"), 0.6);
        CHECK_DBL(member_dbl(f, "expected"), 3.0);
        CHECK_DBL(member_dbl(f, "pZero"), 0.049787);
        CHECK_DBL(member_dbl(f, "pAtLeastOne"), 0.950213);
        CHECK_INT(member_int(json_get(f, "range90"), "low"), 1);
        CHECK_INT(member_int(json_get(f, "range90"), "high"), 6);
        CHECK_MSG(json_len(json_get(f, "pmf")) > 5, "pmf is missing");
        CHECK_DBL(member_dbl(b, "mean"), 4.0);
        CHECK_DBL(member_dbl(b, "sd"), 2.4495);
        CHECK_INT(member_int(v, "seed"), 42);
        CHECK_INT(json_len(pred), member_int(f, "generated"));
        CHECK_MSG(json_len(json_get(v, "warnings")) > 0,
                  "a four-process workload must carry a warning");
        for (i = 0; i < json_len(pred); i++) {
            bool flag = false;
            const JsonValue *p = json_at(pred, i);
            CHECK(json_member_bool(p, "predicted", &flag) && flag);
            CHECK(member_int(p, "arrival") > 5);
            CHECK(member_int(p, "arrival") <= 10);
        }
        {
            bool valid = true;
            CHECK(json_member_bool(json_get(json_get(v, "stability"),
                                            "split"), "valid", &valid));
            CHECK(!valid);   /* two interior arrivals: too few to test */
        }
    }
    json_free(v);

    t_case("POST /api/forecast honours window, horizon, cap and seed");
    CHECK_INT(route("POST", "/api/forecast",
                    "{" CLASSIC_PROCS ",\"observationStart\":0,"
                    "\"observationEnd\":10,\"horizon\":40,"
                    "\"maxGenerated\":2,\"seed\":9}", &v), 200);
    if (v != NULL) {
        const JsonValue *f = json_get(v, "forecast");
        CHECK_STR(json_as_string(json_get(json_get(v, "observation"),
                                          "mode")), "explicit");
        CHECK_DBL(member_dbl(json_get(v, "rate"), "lambda"), 0.4);
        CHECK_INT(member_int(f, "start"), 10);
        CHECK_INT(member_int(f, "end"), 50);
        CHECK_DBL(member_dbl(f, "expected"), 16.0);
        CHECK_INT(member_int(f, "cap"), 2);
        CHECK_INT(json_len(json_get(v, "predicted")), 2);
        CHECK_INT(member_int(v, "seed"), 9);
        json_free(v);
    }

    t_case("observationEnd alone measures from t=0");
    CHECK_INT(route("POST", "/api/forecast",
                    "{" CLASSIC_PROCS ",\"observationEnd\":8}", &v), 200);
    if (v != NULL) {
        CHECK_INT(member_int(json_get(v, "observation"), "start"), 0);
        CHECK_DBL(member_dbl(json_get(v, "rate"), "lambda"), 0.5);
        json_free(v);
    }

    t_case("the same request twice gives the same forecast");
    {
        JsonValue *a = NULL, *b = NULL;
        const char *body = "{" CLASSIC_PROCS ",\"horizon\":30,\"seed\":5}";
        route("POST", "/api/forecast", body, &a);
        route("POST", "/api/forecast", body, &b);
        if (a != NULL && b != NULL) {
            const JsonValue *pa = json_get(a, "predicted");
            const JsonValue *pb = json_get(b, "predicted");
            int i;
            CHECK_INT(json_len(pa), json_len(pb));
            for (i = 0; i < json_len(pa) && i < json_len(pb); i++) {
                CHECK_INT(member_int(json_at(pa, i), "arrival"),
                          member_int(json_at(pb, i), "arrival"));
                CHECK_INT(member_int(json_at(pa, i), "burst"),
                          member_int(json_at(pb, i), "burst"));
            }
        }
        json_free(a);
        json_free(b);
    }
}

static void test_route_forecast_rejects(void)
{
    JsonValue *v = NULL;
    size_t i;
    static const struct { const char *path, *body, *needle; } BAD[] = {
        { "/api/forecast", "{" CLASSIC_PROCS ",\"horizon\":0}", "horizon" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"horizon\":2.5}",
          "whole number" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"horizon\":\"5\"}",
          "horizon" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"horizon\":1e300}",
          "whole number" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"seed\":-1}", "seed" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"observationStart\":0}",
          "observationEnd" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"observationEnd\":4}",
          "contain every observed arrival" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"observationEnd\":-3}",
          "observation window" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"maxGenerated\":0}", "cap" },
        { "/api/forecast", "{" CLASSIC_PROCS ",\"maxGenerated\":100000}",
          "cap" },
        { "/api/forecast", "{\"processes\":[{\"id\":\"P1\",\"arrival\":0,"
          "\"burst\":3}]}", "observation window" },
        { "/api/forecast", "{\"processes\":[]}", "at least one" },
        { "/api/forecast", "{\"processes\":[{\"id\":\"P1\",\"arrival\":0,"
          "\"burst\":0}]}", "burst" },
        { "/api/forecast", "not json", "" },
        { "/api/forecast/simulate",
          "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS "}", "predicted" },
        { "/api/forecast/simulate",
          "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS ",\"predicted\":"
          "[{\"id\":\"p1\",\"arrival\":9,\"burst\":2}]}", "already in use" },
        { "/api/forecast/simulate",
          "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS ",\"predicted\":"
          "[{\"id\":\"F1\",\"arrival\":9,\"burst\":0}]}", "burst" },
        { "/api/forecast/simulate",
          "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS ",\"predicted\":"
          "[{\"id\":\"F1\",\"arrival\":9,\"burst\":1},"
          "{\"id\":\"F1\",\"arrival\":9,\"burst\":1}]}", "duplicate" },
        { "/api/forecast/simulate",
          "{" CLASSIC_PROCS ",\"predicted\":[]}", "algorithm" },
        { "/api/forecast/simulate",
          "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS ",\"predicted\":[7]}",
          "not an object" }
    };

    t_case("forecast routes reject bad input with a reason");
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        int status = route("POST", BAD[i].path, BAD[i].body, &v);
        const char *msg = (v != NULL) ? json_as_string(json_get(v, "error"))
                                      : NULL;
        CHECK_MSG(status == 400, "expected 400 for %s %s (got %d)",
                  BAD[i].path, BAD[i].body, status);
        CHECK_MSG(msg != NULL, "no error message for: %s", BAD[i].body);
        if (msg != NULL && BAD[i].needle[0] != '\0') {
            CHECK_MSG(strstr(msg, BAD[i].needle) != NULL,
                      "message \"%s\" should mention \"%s\"", msg,
                      BAD[i].needle);
        }
        json_free(v);
        v = NULL;
    }

    t_case("forecast method guards");
    CHECK_INT(route("GET", "/api/forecast", "", &v), 405);
    json_free(v);
    CHECK_INT(route("GET", "/api/forecast/simulate", "", &v), 405);
    json_free(v);
}

static const JsonValue *row(const JsonValue *cmp, const char *key)
{
    int i;
    for (i = 0; i < json_len(cmp); i++) {
        const char *k = json_as_string(json_get(json_at(cmp, i), "key"));
        if (k != NULL && strcmp(k, key) == 0) return json_at(cmp, i);
    }
    return NULL;
}

static void test_route_forecast_simulate(void)
{
    JsonValue *v = NULL;

    /* SRTF on the classic workload plus one forecast arrival F1(6, 1).
     *   original   P1 0-2 P2 2-4 P3 4-5 P2 5-7 P4 7-11 P1 11-16
     *              waits 9, 1, 0, 2              -> avg 3.00
     *   augmented  at t=6 F1 (1) does not beat P2 (1 left): P2 runs to 7;
     *              at t=7 F1 (1) < P4 (4) < P1 (5)
     *              P1 0-2 P2 2-4 P3 4-5 P2 5-7 F1 7-8 P4 8-12 P1 12-17
     *              waits P1 10, P2 1, P3 0, P4 3, F1 1
     *              all five: 15/5 = 3.00; the original four: 14/4 = 3.50 */
    t_case("hand-worked: one forecast arrival delays the user's processes");
    CHECK_INT(route("POST", "/api/forecast/simulate",
                    "{\"algorithm\":\"srtf\"," CLASSIC_PROCS ",\"predicted\":"
                    "[{\"id\":\"F1\",\"arrival\":6,\"burst\":1,"
                    "\"priority\":0}]}", &v), 200);
    if (v == NULL) return;
    {
        const JsonValue *orig = json_get(v, "original");
        const JsonValue *aug = json_get(v, "augmented");
        const JsonValue *cmp = json_get(v, "comparison");
        const JsonValue *r;
        int i;

        CHECK_INT(member_int(v, "originalCount"), 4);
        CHECK_INT(member_int(v, "predictedCount"), 1);
        CHECK_DBL(member_dbl(json_get(orig, "metrics"), "avgWaiting"), 3.0);
        CHECK_DBL(member_dbl(json_get(aug, "metrics"), "avgWaiting"), 3.0);
        CHECK_INT(json_len(json_get(aug, "processes")), 5);
        CHECK_INT(member_int(json_get(aug, "window"), "end"), 17);

        for (i = 0; i < json_len(json_get(aug, "processes")); i++) {
            const JsonValue *p = json_at(json_get(aug, "processes"), i);
            bool flag = true;
            CHECK(json_member_bool(p, "predicted", &flag));
            CHECK_MSG(flag == (i >= 4), "process %d predicted flag is %d",
                      i, (int)flag);
        }

        r = row(cmp, "origAvgWaiting");
        CHECK(r != NULL);
        CHECK_DBL(member_dbl(r, "original"), 3.0);
        CHECK_DBL(member_dbl(r, "augmented"), 3.5);
        CHECK_DBL(member_dbl(r, "change"), 0.5);
        r = row(cmp, "totalTime");
        CHECK_DBL(member_dbl(r, "original"), 16.0);
        CHECK_DBL(member_dbl(r, "augmented"), 17.0);

        t_case("every comparison row is augmented - original");
        for (i = 0; i < json_len(cmp); i++) {
            const JsonValue *x = json_at(cmp, i);
            CHECK_NEAR(member_dbl(x, "change"),
                       member_dbl(x, "augmented") - member_dbl(x, "original"),
                       1e-3);
        }
    }
    json_free(v);

    /* FCFS, with the forecast arriving after everything has finished:
     *   augmented P1 0-7 P2 7-11 P3 11-12 P4 12-16 IDLE 16-20 F1 20-22
     *   avg waiting 19/5 = 3.80, util 18/22 = 81.82%, idle 4 */
    t_case("hand-worked: a late forecast arrival adds idle time");
    CHECK_INT(route("POST", "/api/forecast/simulate",
                    "{\"algorithm\":\"fcfs\"," CLASSIC_PROCS ",\"predicted\":"
                    "[{\"id\":\"F1\",\"arrival\":20,\"burst\":2}]}", &v), 200);
    if (v != NULL) {
        const JsonValue *cmp = json_get(v, "comparison");
        CHECK_DBL(member_dbl(row(cmp, "avgWaiting"), "original"), 4.75);
        CHECK_DBL(member_dbl(row(cmp, "avgWaiting"), "augmented"), 3.8);
        CHECK_DBL(member_dbl(row(cmp, "cpuUtilization"), "augmented"), 81.82);
        CHECK_DBL(member_dbl(row(cmp, "idleTime"), "augmented"), 4.0);
        CHECK_DBL(member_dbl(row(cmp, "origAvgWaiting"), "change"), 0.0);
        json_free(v);
    }

    t_case("an empty forecast leaves the schedule unchanged");
    CHECK_INT(route("POST", "/api/forecast/simulate",
                    "{\"algorithm\":\"rr\",\"quantum\":2," CLASSIC_PROCS
                    ",\"predicted\":[]}", &v), 200);
    if (v != NULL) {
        const JsonValue *cmp = json_get(v, "comparison");
        int i;
        for (i = 0; i < json_len(cmp); i++)
            CHECK_DBL(member_dbl(json_at(cmp, i), "change"), 0.0);
        json_free(v);
    }
}

/* Forecast -> simulate round trip through both routes, for every
 * algorithm, checking the original half matches /api/simulate exactly. */
static void test_route_round_trip(void)
{
    static const char *const SLUG[ALGO_COUNT] = {
        "fcfs", "sjf", "srtf", "prio-np", "prio-p", "rr"
    };
    JsonValue *fc = NULL;
    char pred[16384];
    size_t used = 0;
    int i, npred;

    t_case("forecast -> simulate under all six algorithms");
    CHECK_INT(route("POST", "/api/forecast",
                    "{" CLASSIC_PROCS ",\"horizon\":25,\"seed\":11}", &fc),
              200);
    if (fc == NULL) return;
    npred = json_len(json_get(fc, "predicted"));
    CHECK_MSG(npred > 0, "expected arrivals over 25 units at rate 0.6");

    used += (size_t)snprintf(pred + used, sizeof pred - used, "[");
    for (i = 0; i < npred; i++) {
        const JsonValue *p = json_at(json_get(fc, "predicted"), i);
        used += (size_t)snprintf(pred + used, sizeof pred - used,
                                 "%s{\"id\":\"%s\",\"arrival\":%d,"
                                 "\"burst\":%d,\"priority\":%d}",
                                 i ? "," : "",
                                 json_as_string(json_get(p, "id")),
                                 member_int(p, "arrival"),
                                 member_int(p, "burst"),
                                 member_int(p, "priority"));
    }
    snprintf(pred + used, sizeof pred - used, "]");
    json_free(fc);

    for (i = 0; i < ALGO_COUNT; i++) {
        char body[20000];
        char plain[512];
        JsonValue *v = NULL, *s = NULL;

        snprintf(body, sizeof body,
                 "{\"algorithm\":\"%s\",\"quantum\":3," CLASSIC_PROCS
                 ",\"predicted\":%s}", SLUG[i], pred);
        snprintf(plain, sizeof plain,
                 "{\"algorithm\":\"%s\",\"quantum\":3," CLASSIC_PROCS "}",
                 SLUG[i]);

        CHECK_MSG(route("POST", "/api/forecast/simulate", body, &v) == 200,
                  "%s: forecast simulate failed", SLUG[i]);
        CHECK_MSG(route("POST", "/api/simulate", plain, &s) == 200,
                  "%s: plain simulate failed", SLUG[i]);
        if (v != NULL && s != NULL) {
            const JsonValue *om = json_get(json_get(v, "original"),
                                           "metrics");
            const JsonValue *sm = json_get(s, "metrics");
            const JsonValue *aug = json_get(v, "augmented");
            CHECK_DBL(member_dbl(om, "avgWaiting"),
                      member_dbl(sm, "avgWaiting"));
            CHECK_DBL(member_dbl(om, "avgTurnaround"),
                      member_dbl(sm, "avgTurnaround"));
            CHECK_INT(member_int(om, "contextSwitches"),
                      member_int(sm, "contextSwitches"));
            CHECK_INT(json_len(json_get(aug, "processes")), 4 + npred);
            CHECK_MSG(json_len(json_get(aug, "trace")) > 0,
                      "%s: the augmented trace is missing", SLUG[i]);
        }
        json_free(v);
        json_free(s);
    }
}

void suite_predict(void)
{
    t_suite("predict");
    test_rate();
    test_poisson();
    test_exponential();
    test_arrival_generator();
    test_burst_summary();
    test_tests();
    test_forecast_classic();
    test_forecast_rejects();
    test_forecast_records();
    test_reproducibility();
    test_count_distribution();
    test_truncation();
    test_diagnostics();
    test_route_forecast();
    test_route_forecast_rejects();
    test_route_forecast_simulate();
    test_route_round_trip();
}
