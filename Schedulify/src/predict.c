/*
 * predict.c -- Statistical predictive workload analysis.
 *
 * See predict.h for the model. Two rules shape everything below:
 *
 *  1. The count model and the arrival times are one process, not two. The
 *     exponential gaps are drawn until they pass the horizon, and the number
 *     of arrivals that fit *is* the Poisson(lambda*h) draw. The Poisson
 *     formulas are used only to describe the distribution that draw came
 *     from (expected count, P(K=0), the central range).
 *
 *  2. Nothing is presented as more certain than the data allow. The window
 *     the rate was measured over is always reported with how it was chosen,
 *     and every diagnostic that could not be run says why.
 */
#include "predict.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "validate.h"

/* ------------------------------------------------------------ helpers -- */

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static void warn(ForecastResult *res, const char *fmt, ...)
{
    va_list ap;
    if (res->warn_count >= PREDICT_WARN_MAX) return;
    va_start(ap, fmt);
    vsnprintf(res->warnings[res->warn_count], PREDICT_WARN_TEXT, fmt, ap);
    va_end(ap);
    res->warn_count++;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

void forecast_config_default(ForecastConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->mode = OBS_FROM_ARRIVAL_GAPS;
    cfg->horizon = 0;
    cfg->max_generated = 0;
    cfg->seed = (long)PREDICT_DEFAULT_SEED;
}

/* --------------------------------------------------------- random ------ */

static unsigned long long splitmix64(unsigned long long x)
{
    unsigned long long z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void predict_rng_seed(PredictRng *r, long seed)
{
    r->s = splitmix64((unsigned long long)seed);
    if (r->s == 0) r->s = 0x9E3779B97F4A7C15ULL;   /* xorshift's one bad state */
}

static unsigned long long rng_next(PredictRng *r)
{
    unsigned long long x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 2685821657736338717ULL;
}

double predict_rng_uniform(PredictRng *r)
{
    /* 53 random bits, centred in their cell: never exactly 0 or 1, so
     * -ln(U) is always finite and positive. */
    return ((double)(rng_next(r) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

int predict_rng_index(PredictRng *r, int n)
{
    if (n <= 1) return 0;
    return (int)((rng_next(r) >> 11) % (unsigned long long)n);
}

/* -------------------------------------------------------- the models -- */

bool predict_rate(int count, double exposure, double *lambda)
{
    double v;
    if (count < 0 || !(exposure > 0.0) || !isfinite(exposure)) return false;
    v = (double)count / exposure;
    if (!isfinite(v)) return false;
    *lambda = v;
    return true;
}

double predict_poisson_pmf(int k, double mu)
{
    if (!isfinite(mu) || mu < 0.0) return NAN;
    if (k < 0) return 0.0;
    if (mu == 0.0) return (k == 0) ? 1.0 : 0.0;
    /* log space: (mu^k e^-mu) / k! overflows long before mu*h gets large */
    return exp((double)k * log(mu) - mu - lgamma((double)k + 1.0));
}

/* Below this k the mass of Poisson(mu) is < 1e-30; above poisson_hi it is
 * likewise negligible. Summing only the band between keeps a cdf over a
 * mean of millions to a few hundred thousand terms. */
static int poisson_lo(double mu)
{
    double lo = mu - 12.0 * sqrt(mu) - 12.0;
    return (lo <= 0.0) ? 0 : (int)lo;
}

static double poisson_hi(double mu)
{
    return mu + 12.0 * sqrt(mu) + 12.0;
}

double predict_poisson_cdf(int k, double mu)
{
    int i, lo;
    double p, sum = 0.0;

    if (!isfinite(mu) || mu < 0.0) return NAN;
    if (k < 0) return 0.0;
    if (mu == 0.0) return 1.0;
    if ((double)k >= poisson_hi(mu)) return 1.0;

    lo = poisson_lo(mu);
    if (k < lo) return 0.0;
    p = predict_poisson_pmf(lo, mu);
    for (i = lo; i <= k; i++) {
        sum += p;
        p *= mu / (double)(i + 1);
    }
    return (sum > 1.0) ? 1.0 : sum;
}

int predict_poisson_quantile(double prob, double mu)
{
    int i, lo, hi;
    double p, sum = 0.0;

    if (!isfinite(mu) || mu < 0.0 || !(prob >= 0.0 && prob <= 1.0)) return -1;
    if (mu == 0.0) return 0;

    lo = poisson_lo(mu);
    hi = (int)poisson_hi(mu);
    p = predict_poisson_pmf(lo, mu);
    for (i = lo; i < hi; i++) {
        sum += p;
        if (sum >= prob) return i;
        p *= mu / (double)(i + 1);
    }
    return hi;
}

double predict_exp_gap(double u, double lambda)
{
    if (!(u > 0.0 && u < 1.0)) return -1.0;
    if (!(lambda > 0.0) || !isfinite(lambda)) return -1.0;
    return -log(u) / lambda;
}

int predict_arrivals(double lambda, int start, int horizon, int max,
                     PredictRng *r, int *out, bool *truncated)
{
    double t = (double)start;
    double end = (double)start + (double)horizon;
    int k = 0;

    *truncated = false;
    if (!(lambda > 0.0) || !isfinite(lambda) || horizon <= 0) return 0;

    for (;;) {
        double gap = predict_exp_gap(predict_rng_uniform(r), lambda);
        int at;

        if (!(gap >= 0.0) || !isfinite(gap)) break;
        t += gap;
        if (!(t <= end)) break;               /* past the horizon: done      */
        if (k >= max) { *truncated = true; break; }

        /* The simulator's clock is integral. Rounding *up* keeps every
         * predicted arrival strictly after the observation and inside the
         * horizon: t in (start, start+h] maps into [start+1, start+h]. */
        at = (int)ceil(t);
        if (at <= start) at = start + 1;
        if (at > start + horizon) at = start + horizon;
        out[k++] = at;
    }
    return k;
}

bool predict_burst_summary(const Dataset *ds, double *mean, double *sd,
                           bool *sd_valid)
{
    int i;
    double sum = 0.0, ss = 0.0, m;

    if (ds == NULL || ds->count < 1) return false;
    for (i = 0; i < ds->count; i++) sum += (double)ds->items[i].burst;
    m = sum / (double)ds->count;
    for (i = 0; i < ds->count; i++) {
        double d = (double)ds->items[i].burst - m;
        ss += d * d;
    }
    *mean = m;
    *sd_valid = (ds->count >= 2);
    *sd = *sd_valid ? sqrt(ss / (double)(ds->count - 1)) : 0.0;
    return true;
}

static double binom_pmf(int k, int n, double p)
{
    if (k < 0 || k > n) return 0.0;
    if (p <= 0.0) return (k == 0) ? 1.0 : 0.0;
    if (p >= 1.0) return (k == n) ? 1.0 : 0.0;
    return exp(lgamma((double)n + 1.0) - lgamma((double)k + 1.0)
               - lgamma((double)(n - k) + 1.0)
               + (double)k * log(p) + (double)(n - k) * log1p(-p));
}

double predict_binom_two_sided(int k, int n, double p)
{
    int i;
    double pk, sum = 0.0;

    if (n < 0 || k < 0 || k > n || !(p >= 0.0 && p <= 1.0)) return NAN;
    pk = binom_pmf(k, n, p);
    for (i = 0; i <= n; i++) {
        double pi = binom_pmf(i, n, p);
        if (pi <= pk * (1.0 + 1e-7)) sum += pi;
    }
    return (sum > 1.0) ? 1.0 : sum;
}

/* Series for P(a,x), good for x < a+1. */
static double gamma_p_series(double a, double x)
{
    double ap = a, del = 1.0 / a, sum = del;
    int n;
    for (n = 0; n < 1000; n++) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (fabs(del) < fabs(sum) * 1e-15) break;
    }
    return sum * exp(-x + a * log(x) - lgamma(a));
}

/* Continued fraction for Q(a,x) = 1 - P(a,x), good for x >= a+1
 * (modified Lentz). */
static double gamma_q_cf(double a, double x)
{
    const double tiny = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    int i;
    for (i = 1; i < 1000; i++) {
        double an = -(double)i * ((double)i - a), del;
        b += 2.0;
        d = an * d + b;
        if (fabs(d) < tiny) d = tiny;
        c = b + an / c;
        if (fabs(c) < tiny) c = tiny;
        d = 1.0 / d;
        del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 1e-15) break;
    }
    return exp(-x + a * log(x) - lgamma(a)) * h;
}

double predict_gamma_p(double a, double x)
{
    if (!(a > 0.0) || !isfinite(a) || x != x) return NAN;
    if (x <= 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    if (x < a + 1.0) return gamma_p_series(a, x);
    return 1.0 - gamma_q_cf(a, x);
}

double predict_chi2_sf(double x, int df)
{
    double a = (double)df / 2.0, hx = x / 2.0;
    if (df < 1 || x != x) return NAN;
    if (x <= 0.0) return 1.0;
    if (!isfinite(x)) return 0.0;
    /* Compute the tail directly where it is small, so a p-value of 1e-9
     * does not come out as 1 - 0.999999999 rounded to zero. */
    if (hx < a + 1.0) return 1.0 - gamma_p_series(a, hx);
    return gamma_q_cf(a, hx);
}

/* --------------------------------------------------------- diagnostics */

/* The arrivals the stability checks may use, and the window they are
 * uniform over under a homogeneous Poisson process. With an explicit window
 * that is every arrival over [start, end]. When the window was inferred from
 * the arrivals themselves, the first and last arrival *define* the window,
 * so only the ones strictly between them carry information about it. */
static int test_events(const int *sorted, int n, const ForecastResult *res,
                       const int **events)
{
    if (res->mode == OBS_EXPLICIT) {
        *events = sorted;
        return n;
    }
    *events = sorted + 1;
    return (n >= 2) ? n - 2 : 0;
}

static void run_diagnostics(const int *sorted, int n, ForecastResult *res)
{
    const int *ev = NULL;
    int ne = test_events(sorted, n, res, &ev);
    double T = (double)res->exposure;
    int i;

    res->test_events = ne;

    /* simultaneous observed arrivals: impossible in a continuous Poisson
     * process, common in teaching workloads */
    res->simultaneous = 0;
    for (i = 0; i < n; i++) {
        bool tie = (i > 0 && sorted[i] == sorted[i - 1])
                || (i + 1 < n && sorted[i] == sorted[i + 1]);
        if (tie) res->simultaneous++;
    }

    /* Rate in the first half of the window against the second. Given the
     * number of events, each falls in the first half with probability 1/2
     * if the rate is constant -- an exact binomial test. */
    res->split_valid = false;
    if (ne >= PREDICT_SPLIT_MIN && T > 0.0) {
        double mid = (double)res->obs_start + T / 2.0;
        int first = 0;
        for (i = 0; i < ne; i++)
            if ((double)ev[i] < mid) first++;
        res->split_valid = true;
        res->split_first = first;
        res->split_second = ne - first;
        res->split_rate_first = (double)first / (T / 2.0);
        res->split_rate_second = (double)(ne - first) / (T / 2.0);
        res->split_p = predict_binom_two_sided(first, ne, 0.5);
    }

    /* Index of dispersion: cut the window into equal bins and compare the
     * variance of the bin counts with their mean. Poisson counts have
     * variance == mean; D = sum (c - m)^2 / m ~ chi2(bins - 1). */
    res->disp_valid = false;
    if (ne >= PREDICT_DISP_MIN && T > 0.0) {
        int bins = ne / 3, counts[20];
        double m, ss = 0.0, D, var;

        if (bins < 4) bins = 4;
        if (bins > 20) bins = 20;
        memset(counts, 0, sizeof counts);
        for (i = 0; i < ne; i++) {
            int b = (int)(((double)(ev[i] - res->obs_start) * bins) / T);
            if (b < 0) b = 0;
            if (b >= bins) b = bins - 1;
            counts[b]++;
        }
        m = (double)ne / (double)bins;
        for (i = 0; i < bins; i++) {
            double d = (double)counts[i] - m;
            ss += d * d;
        }
        var = ss / (double)(bins - 1);
        D = ss / m;
        res->disp_valid = true;
        res->disp_bins = bins;
        res->disp_vmr = var / m;
        res->disp_p_over = predict_chi2_sf(D, bins - 1);
        res->disp_p_under = 1.0 - res->disp_p_over;
    }
}

static void add_warnings(ForecastResult *res)
{
    int N = res->observed_events;
    const char *what;

    if (res->mode == OBS_EXPLICIT)
        what = (N == 1) ? "arrival was" : "arrivals were";
    else
        what = (N == 1) ? "gap between arrivals was"
                        : "gaps between arrivals were";

    if (N < PREDICT_MIN_RELIABLE) {
        warn(res, "Only %d %s observed. The rate's standard error is about "
                  "%.0f%% of the estimate, so this forecast is "
                  "illustrative, not a reliable description of a real "
                  "workload.", N, what,
             100.0 / sqrt((double)(N > 0 ? N : 1)));
    }
    if (!res->burst_sd_valid) {
        warn(res, "Only one burst time was observed, so its spread cannot be "
                  "estimated and every predicted burst equals %d.",
             res->burst_min);
    } else if (res->burst_distinct == 1) {
        warn(res, "Every observed burst is %d, so every predicted burst is "
                  "too. Resampling cannot invent values that were never "
                  "observed.", res->burst_min);
    }
    if (res->simultaneous > 0) {
        warn(res, "%d observed processes share an arrival time with another. "
                  "A Poisson process has no simultaneous arrivals; batch "
                  "arrivals like these suggest the workload is not "
                  "Poisson.", res->simultaneous);
    }
    if (res->split_valid && res->split_p < 0.05) {
        warn(res, "The arrival rate differs between the first half of the "
                  "window (%.3f/unit) and the second (%.3f/unit), exact "
                  "binomial p = %.3f. A constant-rate model may not fit.",
             res->split_rate_first, res->split_rate_second, res->split_p);
    }
    if (res->disp_valid && res->disp_p_over < 0.05) {
        warn(res, "Arrivals are burstier than Poisson (variance/mean of "
                  "counts = %.2f over %d bins, p = %.3f). Real arrivals "
                  "cluster more than this model assumes.",
             res->disp_vmr, res->disp_bins, res->disp_p_over);
    } else if (res->disp_valid && res->disp_p_under < 0.05) {
        warn(res, "Arrivals are more evenly spaced than Poisson "
                  "(variance/mean of counts = %.2f over %d bins, p = %.3f). "
                  "Random exponential gaps will look more irregular than "
                  "this workload.", res->disp_vmr, res->disp_bins,
             res->disp_p_under);
    }
    if ((double)res->horizon > 3.0 * (double)res->exposure) {
        warn(res, "The horizon (%d) is %.1f times the observation window "
                  "(%d). Extrapolating that far assumes the rate never "
                  "changes.", res->horizon,
             (double)res->horizon / (double)res->exposure, res->exposure);
    }
    if (res->truncated) {
        warn(res, "Generation stopped at %d processes (%s) before the "
                  "horizon was reached. The count is censored there and no "
                  "longer follows Poisson(%.2f); under the model this "
                  "happens with probability %.3f.", res->cap,
             res->cap_from_user ? "your cap" : "the engine's process limit",
             res->expected, res->p_cap_exceeded);
    }
    if (res->generated == 0) {
        warn(res, "This draw produced no arrivals in the horizon. That is a "
                  "legitimate outcome (P(K=0) = %.3f); try another seed or a "
                  "longer horizon.", res->p_zero);
    }
}

/* --------------------------------------------------------- the driver -- */

bool forecast_run(const Dataset *observed, const ForecastConfig *cfg,
                  Dataset *predicted, ForecastResult *res,
                  char *err, size_t errsz)
{
    int *sorted = NULL, *arrivals = NULL;
    int n, i, room, lo, hi, next_id = 1;
    long end;
    char name[DATASET_NAME_MAX];
    PredictRng rng;
    bool ok = false;

    memset(res, 0, sizeof *res);
    snprintf(name, sizeof name, "forecast (seed %ld)", cfg->seed);
    dataset_init(predicted, name);

    n = (observed != NULL) ? observed->count : 0;
    if (n < 1) {
        set_err(err, errsz, "add at least one process before forecasting");
        return false;
    }
    if (cfg->seed < 0 || cfg->seed > PREDICT_MAX_SEED) {
        set_err(err, errsz, "the seed must be between 0 and %ld",
                PREDICT_MAX_SEED);
        return false;
    }

    sorted = malloc((size_t)n * sizeof *sorted);
    if (sorted == NULL) {
        set_err(err, errsz, "out of memory");
        return false;
    }
    for (i = 0; i < n; i++) sorted[i] = observed->items[i].arrival;
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_int);
    lo = sorted[0];
    hi = sorted[n - 1];

    res->processes = n;
    res->intervals = n - 1;
    res->mode = cfg->mode;
    res->seed = cfg->seed;
    res->mean_gap = (n >= 2) ? (double)(hi - lo) / (double)(n - 1) : NAN;

    /* ---- the observation window and the rate ---- */
    if (cfg->mode == OBS_EXPLICIT) {
        if (cfg->obs_start < MIN_ARRIVAL || cfg->obs_end > MAX_ARRIVAL) {
            set_err(err, errsz, "the observation window must lie within "
                    "t=%d..%d", MIN_ARRIVAL, MAX_ARRIVAL);
            goto done;
        }
        if (cfg->obs_end <= cfg->obs_start) {
            set_err(err, errsz, "the observation window must end after it "
                    "starts (got %d to %d)", cfg->obs_start, cfg->obs_end);
            goto done;
        }
        if (cfg->obs_start > lo || cfg->obs_end < hi) {
            set_err(err, errsz, "the observation window t=%d..%d must "
                    "contain every observed arrival (t=%d..%d)",
                    cfg->obs_start, cfg->obs_end, lo, hi);
            goto done;
        }
        res->obs_start = cfg->obs_start;
        res->obs_end = cfg->obs_end;
        res->observed_events = n;
        snprintf(res->assumption, sizeof res->assumption,
                 "Observation window given as t=%d to t=%d (%d units): "
                 "lambda = %d arrivals / %d units.", res->obs_start,
                 res->obs_end, res->obs_end - res->obs_start, n,
                 res->obs_end - res->obs_start);
    } else {
        if (n < 2) {
            set_err(err, errsz, "one arrival does not define a rate: enter "
                    "the observation window, or add more processes");
            goto done;
        }
        if (hi == lo) {
            set_err(err, errsz, "all %d processes arrive at t=%d, so the "
                    "gaps between arrivals total zero time and no rate can "
                    "be estimated: enter the observation window instead", n,
                    lo);
            goto done;
        }
        res->obs_start = lo;
        res->obs_end = hi;
        res->observed_events = n - 1;
        snprintf(res->assumption, sizeof res->assumption,
                 "No observation window given. Assumed: observation began "
                 "at the first arrival (t=%d) and ended at the last (t=%d), "
                 "so lambda = %d gaps / %d units. If the workload was "
                 "watched for longer, enter the real window.", lo, hi,
                 n - 1, hi - lo);
    }
    res->exposure = res->obs_end - res->obs_start;
    if (!predict_rate(res->observed_events, (double)res->exposure,
                      &res->lambda)) {
        set_err(err, errsz, "the observation window has zero length");
        goto done;
    }
    res->lambda_se = (res->observed_events > 0)
                   ? res->lambda / sqrt((double)res->observed_events) : NAN;

    /* ---- the horizon ---- */
    if (cfg->horizon < 0) {
        set_err(err, errsz, "the forecast horizon must be at least 1");
        goto done;
    }
    res->horizon = (cfg->horizon > 0) ? cfg->horizon : res->exposure;
    end = (long)res->obs_end + (long)res->horizon;
    if (end > MAX_ARRIVAL) {
        set_err(err, errsz, "a horizon of %d after t=%d runs past the "
                "largest arrival time the engine accepts (t=%d); use at "
                "most %d", res->horizon, res->obs_end, MAX_ARRIVAL,
                MAX_ARRIVAL - res->obs_end);
        goto done;
    }
    res->forecast_start = res->obs_end;
    res->forecast_end = (int)end;

    /* ---- the cap ---- */
    room = MAX_PROCESSES - n;
    if (room < 1) {
        set_err(err, errsz, "the workload already holds the maximum of %d "
                "processes, so nothing can be added", MAX_PROCESSES);
        goto done;
    }
    if (cfg->max_generated < 0 || cfg->max_generated > room) {
        set_err(err, errsz, "the cap on predicted processes must be between "
                "1 and %d (%d observed + predicted may not exceed %d)",
                room, n, MAX_PROCESSES);
        goto done;
    }
    res->cap_from_user = (cfg->max_generated > 0);
    res->cap = res->cap_from_user ? cfg->max_generated : room;

    /* ---- the count model ---- */
    res->expected = res->lambda * (double)res->horizon;
    res->p_zero = exp(-res->expected);
    res->p_at_least_one = -expm1(-res->expected);
    res->range_low = predict_poisson_quantile(0.05, res->expected);
    res->range_high = predict_poisson_quantile(0.95, res->expected);
    res->p_cap_exceeded = 1.0 - predict_poisson_cdf(res->cap, res->expected);
    if (res->p_cap_exceeded < 0.0) res->p_cap_exceeded = 0.0;

    /* ---- bursts and priorities: the empirical distribution ---- */
    if (!predict_burst_summary(observed, &res->burst_mean, &res->burst_sd,
                               &res->burst_sd_valid)) {
        set_err(err, errsz, "no burst times to sample from");
        goto done;
    }
    {
        int b[MAX_PROCESSES], p[MAX_PROCESSES];
        for (i = 0; i < n; i++) {
            b[i] = observed->items[i].burst;
            p[i] = observed->items[i].priority;
        }
        qsort(b, (size_t)n, sizeof b[0], cmp_int);
        qsort(p, (size_t)n, sizeof p[0], cmp_int);
        res->burst_min = b[0];
        res->burst_max = b[n - 1];
        res->priority_min = p[0];
        res->priority_max = p[n - 1];
        res->burst_distinct = res->priority_distinct = 1;
        for (i = 1; i < n; i++) {
            if (b[i] != b[i - 1]) res->burst_distinct++;
            if (p[i] != p[i - 1]) res->priority_distinct++;
        }
    }

    /* ---- generate ---- */
    arrivals = malloc((size_t)res->cap * sizeof *arrivals);
    if (arrivals == NULL) {
        set_err(err, errsz, "out of memory");
        goto done;
    }
    predict_rng_seed(&rng, cfg->seed);
    res->generated = predict_arrivals(res->lambda, res->forecast_start,
                                      res->horizon, res->cap, &rng, arrivals,
                                      &res->truncated);

    for (i = 0; i < res->generated; i++) {
        Process proc;
        /* Resample a whole observed process, so the burst and the priority
         * keep whatever relationship they had; each on its own is still a
         * draw from its observed distribution. */
        const Process *src = &observed->items[predict_rng_index(&rng, n)];

        memset(&proc, 0, sizeof proc);
        do {
            snprintf(proc.id, sizeof proc.id, PREDICT_ID_PREFIX "%d",
                     next_id++);
        } while (dataset_find_id(observed, proc.id) >= 0
                 || dataset_find_id(predicted, proc.id) >= 0);
        proc.arrival = arrivals[i];
        proc.burst = src->burst;
        proc.priority = src->priority;

        if (!validate_process(&proc, err, errsz)) goto done;
        if (!dataset_add(predicted, &proc)) {
            set_err(err, errsz, "out of memory");
            goto done;
        }
    }

    res->tied_arrivals = 0;
    for (i = 0; i < predicted->count; i++) {
        int a = predicted->items[i].arrival;
        bool tie = (i > 0 && predicted->items[i - 1].arrival == a)
                || (i + 1 < predicted->count
                    && predicted->items[i + 1].arrival == a);
        if (tie) res->tied_arrivals++;
    }

    run_diagnostics(sorted, n, res);
    add_warnings(res);
    ok = true;

done:
    free(sorted);
    free(arrivals);
    if (!ok) {
        dataset_free(predicted);
        dataset_init(predicted, name);
    }
    return ok;
}

bool forecast_merge(const Dataset *observed, const Dataset *predicted,
                    Dataset *merged, char *err, size_t errsz)
{
    int i;

    if (!dataset_clone(merged, observed)) {
        dataset_init(merged, "observed + forecast");
        set_err(err, errsz, "out of memory");
        return false;
    }
    dataset_set_name(merged, "observed + forecast");

    if (observed->count + predicted->count > MAX_PROCESSES) {
        set_err(err, errsz, "%d observed + %d predicted processes exceed the "
                "limit of %d", observed->count, predicted->count,
                MAX_PROCESSES);
        return false;
    }
    for (i = 0; i < predicted->count; i++) {
        const Process *p = &predicted->items[i];
        char sub[ERR_MSG_MAX];

        if (!validate_process(p, sub, sizeof sub)) {
            set_err(err, errsz, "predicted process %d: %s", i + 1, sub);
            return false;
        }
        if (dataset_find_id(merged, p->id) >= 0) {
            set_err(err, errsz, "predicted process id \"%s\" is already in "
                    "use", p->id);
            return false;
        }
        if (!dataset_add(merged, p)) {
            set_err(err, errsz, "out of memory");
            return false;
        }
    }
    return true;
}
