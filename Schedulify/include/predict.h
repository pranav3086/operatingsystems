/*
 * predict.h -- Statistical predictive workload analysis.
 *
 * Generates one *possible* future continuation of the current workload from
 * the workload itself -- no historical dataset, no learned model:
 *
 *   arrival rate    lambda = N / T over a stated observation window
 *   arrival count   K ~ Poisson(lambda * h) over a forecast horizon h
 *   arrival times   exponential inter-arrival gaps, delta = -ln(U) / lambda,
 *                   drawn until the horizon is passed (so the generated count
 *                   *is* the Poisson draw; there is no second arrival stream)
 *   burst/priority  resampled with replacement from the observed processes
 *                   (an empirical bootstrap -- no normality assumption)
 *
 * The result is a statistically generated scenario, not a forecast of known
 * events. Every assumption that went into it is reported alongside it, and
 * the stability diagnostics say when the data do not support a homogeneous
 * Poisson model at all.
 *
 * This module never schedules anything and the schedulers never call it. It
 * only produces an ordinary Dataset, which then goes through the same
 * validation and the same six algorithms as any other workload. Its random
 * numbers come from its own generator, so the scheduling engine's behaviour
 * for a fixed workload is untouched.
 */
#ifndef SCHEDULIFY_PREDICT_H
#define SCHEDULIFY_PREDICT_H

#include <stdbool.h>
#include <stddef.h>

#include "process.h"

#define PREDICT_DEFAULT_SEED   42UL
#define PREDICT_MAX_SEED       2147483647L
#define PREDICT_ID_PREFIX      "F"
#define PREDICT_WARN_MAX       12
#define PREDICT_WARN_TEXT      240
#define PREDICT_NOTE_TEXT      320

/* Below this many observed arrivals the rate estimate is flagged as weak. */
#define PREDICT_MIN_RELIABLE   10

/* Events needed before the half-window rate comparison and the dispersion
 * check are run at all; below these they are reported as not run. */
#define PREDICT_SPLIT_MIN      4
#define PREDICT_DISP_MIN       12

/* ---- configuration ------------------------------------------------------ */

typedef enum {
    /* No window supplied: lambda = (n-1) / (last arrival - first arrival),
     * i.e. the observation is taken to run from the first arrival to the
     * last one. This is stated in the result, never applied silently. */
    OBS_FROM_ARRIVAL_GAPS = 0,
    /* The user states the window [start, end]; lambda = n / (end - start). */
    OBS_EXPLICIT
} ObservationMode;

typedef struct {
    ObservationMode mode;
    int obs_start;          /* OBS_EXPLICIT only                             */
    int obs_end;            /* OBS_EXPLICIT only                             */
    int horizon;            /* forecast length; 0 = same as the window       */
    int max_generated;      /* 0 = only the engine's process limit applies   */
    long seed;              /* 0..PREDICT_MAX_SEED                           */
} ForecastConfig;

/* Fills cfg with the defaults: inferred window, horizon = window length,
 * no extra cap, seed PREDICT_DEFAULT_SEED. */
void forecast_config_default(ForecastConfig *cfg);

/* ---- result ------------------------------------------------------------- */

typedef struct {
    /* observation */
    ObservationMode mode;
    int    obs_start, obs_end;     /* the window actually used               */
    int    exposure;               /* T = obs_end - obs_start                */
    int    processes;              /* n, the observed workload size          */
    int    observed_events;        /* N in lambda = N / T                    */
    int    intervals;              /* n - 1 inter-arrival gaps               */
    double lambda;
    double lambda_se;              /* lambda / sqrt(N), Poisson std error    */
    double mean_gap;               /* mean observed inter-arrival gap        */
    char   assumption[PREDICT_NOTE_TEXT];

    /* the count model over the horizon */
    int    horizon;
    int    forecast_start;         /* arrivals fall in (start, end]          */
    int    forecast_end;
    double expected;               /* E[K] = lambda * h                      */
    double p_zero;                 /* P(K = 0)                               */
    double p_at_least_one;         /* P(K >= 1)                              */
    int    range_low, range_high;  /* central 90% of Poisson(lambda * h)     */

    /* what was generated */
    int    cap;                    /* effective generation limit             */
    bool   cap_from_user;
    double p_cap_exceeded;         /* P(K > cap) under the model             */
    int    generated;
    bool   truncated;              /* the cap stopped generation             */
    int    tied_arrivals;          /* predicted arrivals sharing a time unit */

    /* bursts and priorities */
    double burst_mean;
    double burst_sd;
    bool   burst_sd_valid;         /* needs n >= 2                           */
    int    burst_min, burst_max;
    int    burst_distinct;
    int    priority_min, priority_max, priority_distinct;

    /* stability diagnostics */
    int    simultaneous;           /* observed arrivals sharing a time       */
    int    test_events;            /* arrivals the two checks below can use  */
    bool   split_valid;
    int    split_first, split_second;
    double split_rate_first, split_rate_second;
    double split_p;                /* exact two-sided binomial p-value       */
    bool   disp_valid;
    int    disp_bins;
    double disp_vmr;               /* variance-to-mean ratio of bin counts   */
    double disp_p_over;            /* P(chi2 >= D): small => bursty          */
    double disp_p_under;           /* P(chi2 <= D): small => too regular     */

    long   seed;

    char   warnings[PREDICT_WARN_MAX][PREDICT_WARN_TEXT];
    int    warn_count;
} ForecastResult;

/* Runs the whole analysis on `observed` (which must already be a valid
 * workload) and initialises `predicted` with the generated processes. On
 * failure returns false with a user-facing reason in err; `predicted` is
 * still safe to pass to dataset_free(). */
bool forecast_run(const Dataset *observed, const ForecastConfig *cfg,
                  Dataset *predicted, ForecastResult *res,
                  char *err, size_t errsz);

/* Appends `predicted` after `observed` into `merged` (initialised here),
 * checking id clashes and the process limit. Original processes keep their
 * positions 0..n-1, so index >= observed->count means "predicted". */
bool forecast_merge(const Dataset *observed, const Dataset *predicted,
                    Dataset *merged, char *err, size_t errsz);

/* ---- the statistics, exposed for the tests ------------------------------ */

/* lambda = count / exposure. False when exposure <= 0, count < 0 or the
 * result is not finite. */
bool predict_rate(int count, double exposure, double *lambda);

/* Poisson(mu) probability mass, cumulative distribution and quantile.
 * mu == 0 is the point mass at 0; a negative or non-finite mu yields NaN
 * (pmf, cdf) or -1 (quantile). */
double predict_poisson_pmf(int k, double mu);
double predict_poisson_cdf(int k, double mu);
int    predict_poisson_quantile(double p, double mu);

/* One exponential inter-arrival gap from a uniform u in (0,1):
 * -ln(u) / lambda.  Returns a negative value when u is outside (0,1) or
 * lambda is not a positive finite number. */
double predict_exp_gap(double u, double lambda);

/* Sample mean and (n-1) standard deviation of the burst times. sd_valid is
 * false when fewer than two values exist. False for an empty dataset. */
bool predict_burst_summary(const Dataset *ds, double *mean, double *sd,
                           bool *sd_valid);

/* Exact two-sided binomial test of k successes in n trials at probability p
 * (sum of the probabilities of outcomes no more likely than k). */
double predict_binom_two_sided(int k, int n, double p);

/* Regularised lower incomplete gamma P(a, x), and the chi-square upper tail
 * P(X >= x) for df degrees of freedom. */
double predict_gamma_p(double a, double x);
double predict_chi2_sf(double x, int df);

/* The predictor's own random stream: xorshift64*, seeded through splitmix64
 * so every seed (including 0) gives a full-period, platform-independent
 * sequence. Deliberately separate from the generator in datasets.c. */
typedef struct {
    unsigned long long s;
} PredictRng;

void   predict_rng_seed(PredictRng *r, long seed);
double predict_rng_uniform(PredictRng *r);      /* strictly inside (0,1)  */
int    predict_rng_index(PredictRng *r, int n); /* uniform in [0, n)      */

/* Generates arrival times in (start, start + horizon] from exponential gaps
 * at the given rate, rounding each up to the next whole time unit (the
 * simulator's clock is integral). Writes at most `max` values into out and
 * sets *truncated when the limit stopped generation before the horizon.
 * Returns the number written; lambda <= 0 yields zero arrivals. */
int predict_arrivals(double lambda, int start, int horizon, int max,
                     PredictRng *r, int *out, bool *truncated);

#endif /* SCHEDULIFY_PREDICT_H */
