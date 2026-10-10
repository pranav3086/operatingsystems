/*
 * main.c -- Command-line handling and the menu-driven terminal interface.
 *
 * Every menu entry here is wired to real code; there is no placeholder.
 * The program is also usable non-interactively, which is how the test
 * scripts drive it:
 *
 *     schedulify --example 2 --algo srtf
 *     schedulify --csv data/sample_processes.csv --compare --quantum 3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "compare.h"
#include "csvio.h"
#include "datasets.h"
#include "metrics.h"
#include "process.h"
#include "stepmode.h"
#include "ui.h"
#include "validate.h"
#include "visualize.h"

typedef struct {
    Dataset ds;
    int     quantum;
} App;

/* ------------------------------------------------------------- helpers -- */

static void show_workload_or_warn(const App *app)
{
    if (app->ds.count == 0) {
        ui_warn("no workload is loaded yet -- use \"Load a predefined "
                "example\", \"Enter a custom workload\", \"Generate a random "
                "workload\" or \"Import from CSV\" first");
    } else {
        viz_dataset_table(&app->ds);
    }
}

static bool require_workload(const App *app)
{
    char err[ERR_MSG_MAX];
    if (app->ds.count == 0) {
        ui_warn("load or enter a workload first");
        return false;
    }
    if (!validate_dataset(&app->ds, err, sizeof err)) {
        ui_error("the current workload is not valid: %s", err);
        return false;
    }
    return true;
}

static void replace_dataset(App *app, Dataset *fresh)
{
    dataset_free(&app->ds);
    app->ds = *fresh;
    memset(fresh, 0, sizeof *fresh);
}

/* ---------------------------------------------------- algorithm picker -- */

/* Writes the chosen algorithm into *out.  PROMPT_CANCEL means "go back". */
static PromptResult pick_algorithm(AlgorithmId *out)
{
    int i, choice;
    PromptResult r;

    ui_section("Choose an algorithm");
    for (i = 0; i < ALGO_COUNT; i++) {
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);
        char key[4];
        char text[180];
        snprintf(key, sizeof key, "%d", i + 1);
        snprintf(text, sizeof text, "%-8s %-30s %s", info->abbrev,
                 info->name, info->summary);
        ui_menu_item(key, text);
    }
    r = prompt_int("Algorithm", 1, ALGO_COUNT, &choice);
    if (r != PROMPT_OK) return r;
    *out = (AlgorithmId)(choice - 1);
    return PROMPT_OK;
}

/* Asks for the quantum only when the algorithm needs one. */
static PromptResult ensure_quantum(App *app, AlgorithmId algo)
{
    if (!algo_needs_quantum(algo)) return PROMPT_OK;
    return prompt_int_default("Time quantum", MIN_QUANTUM, MAX_QUANTUM,
                              app->quantum, &app->quantum);
}

/* ------------------------------------------------------------ run one --- */

static PromptResult menu_run_single(App *app)
{
    AlgorithmId algo;
    Schedule sch;
    char err[ERR_MSG_MAX];
    PromptResult r;

    if (!require_workload(app)) return PROMPT_OK;

    r = pick_algorithm(&algo);
    if (r != PROMPT_OK) return r;
    r = ensure_quantum(app, algo);
    if (r != PROMPT_OK) return r;

    if (!algo_run(&sch, &app->ds, algo, app->quantum, false, err,
                  sizeof err)) {
        ui_error("%s", err);
        schedule_free(&sch);
        return PROMPT_OK;
    }

    viz_dataset_table(&app->ds);
    viz_report(&sch);
    schedule_free(&sch);
    return PROMPT_OK;
}

/* ------------------------------------------------------------ compare --- */

static PromptResult menu_compare(App *app)
{
    Comparison cmp;
    char err[ERR_MSG_MAX];
    PromptResult r;

    if (!require_workload(app)) return PROMPT_OK;

    r = prompt_int_default("Time quantum for Round Robin", MIN_QUANTUM,
                           MAX_QUANTUM, app->quantum, &app->quantum);
    if (r != PROMPT_OK) return r;

    if (!compare_run(&cmp, &app->ds, app->quantum, err, sizeof err)) {
        ui_error("%s", err);
        compare_free(&cmp);
        return PROMPT_OK;
    }

    viz_dataset_table(&app->ds);
    compare_report(&cmp);
    compare_free(&cmp);
    return PROMPT_OK;
}

/* ----------------------------------------------------------- step mode -- */

static PromptResult menu_step(App *app)
{
    AlgorithmId algo;
    PromptResult r;

    if (!require_workload(app)) return PROMPT_OK;

    r = pick_algorithm(&algo);
    if (r != PROMPT_OK) return r;
    r = ensure_quantum(app, algo);
    if (r != PROMPT_OK) return r;

    return step_mode_run(&app->ds, algo, app->quantum);
}

/* ------------------------------------------------------------ examples -- */

static PromptResult menu_examples(App *app)
{
    int i, choice;
    PromptResult r;
    Dataset fresh;

    ui_section("Predefined examples");
    for (i = 0; i < examples_count(); i++) {
        char key[12];
        char text[220];
        snprintf(key, sizeof key, "%d", i + 1);
        snprintf(text, sizeof text, "%s", example_name(i));
        ui_menu_item(key, text);
        ui_wrap_dim(example_description(i), 6);
    }

    r = prompt_int("Example", 1, examples_count(), &choice);
    if (r != PROMPT_OK) return r;

    if (!example_load(choice - 1, &fresh)) {
        ui_error("could not load that example");
        return PROMPT_OK;
    }
    replace_dataset(app, &fresh);
    app->quantum = example_quantum(choice - 1);

    ui_success("loaded \"%s\"; Round Robin quantum set to %d",
               app->ds.name, app->quantum);
    viz_dataset_table(&app->ds);
    return PROMPT_OK;
}

/* -------------------------------------------------------------- custom -- */

static PromptResult menu_custom(App *app)
{
    Dataset fresh;
    int n, i;
    PromptResult r;
    char name[DATASET_NAME_MAX];

    ui_section("Enter a custom workload");
    ui_info("Priority convention: a lower number means a higher priority.");

    r = prompt_int("How many processes", 1, MAX_PROCESSES, &n);
    if (r != PROMPT_OK) return r;

    dataset_init(&fresh, "custom");

    for (i = 0; i < n; i++) {
        Process p;
        char idbuf[PROC_ID_MAX];
        char label[64];

        memset(&p, 0, sizeof p);
        printf("\n  %sprocess %d of %d%s\n", ui_c(C_TITLE), i + 1, n,
               ui_c(C_RESET));

        snprintf(label, sizeof label, "  Process id (default P%d)", i + 1);
        for (;;) {
            char buf[INPUT_LINE_MAX];
            char err[ERR_MSG_MAX];
            ui_prompt("%s: ", label);
            r = read_line(buf, sizeof buf);
            if (r == PROMPT_EOF) goto abort;
            if (r == PROMPT_CANCEL) continue;
            trim(buf);
            if (buf[0] == '\0') {
                snprintf(idbuf, sizeof idbuf, "P%d", i + 1);
            } else if (strlen(buf) >= sizeof idbuf) {
                /* Reject rather than silently truncate to 15 characters. */
                ui_error("process id \"%s\" is longer than %d characters",
                         buf, (int)sizeof idbuf - 1);
                continue;
            } else {
                memcpy(idbuf, buf, strlen(buf) + 1);
            }
            if (strcmp(idbuf, "q") == 0) { r = PROMPT_CANCEL; goto abort; }
            if (!validate_id(idbuf, err, sizeof err)) {
                ui_error("%s", err);
                continue;
            }
            if (dataset_find_id(&fresh, idbuf) >= 0) {
                ui_error("process id \"%s\" is already used", idbuf);
                continue;
            }
            break;
        }
        snprintf(p.id, sizeof p.id, "%s", idbuf);

        r = prompt_int("  Arrival time", MIN_ARRIVAL, MAX_ARRIVAL,
                       &p.arrival);
        if (r != PROMPT_OK) goto abort;
        r = prompt_int("  Burst time", MIN_BURST, MAX_BURST, &p.burst);
        if (r != PROMPT_OK) goto abort;
        r = prompt_int_default("  Priority (lower = more urgent)",
                               MIN_PRIORITY, MAX_PRIORITY, 0, &p.priority);
        if (r != PROMPT_OK) goto abort;

        if (!dataset_add(&fresh, &p)) {
            ui_error("out of memory while adding %s", p.id);
            goto abort;
        }
    }

    r = prompt_text("\n  Name for this workload (q keeps \"custom\")",
                    name, sizeof name);
    if (r == PROMPT_EOF) goto abort;
    if (r == PROMPT_OK) dataset_set_name(&fresh, name);

    replace_dataset(app, &fresh);
    ui_success("workload \"%s\" with %d process(es) is ready",
               app->ds.name, app->ds.count);
    viz_dataset_table(&app->ds);
    return PROMPT_OK;

abort:
    dataset_free(&fresh);
    if (r == PROMPT_EOF) return PROMPT_EOF;
    ui_warn("cancelled; the previous workload is unchanged");
    return PROMPT_OK;
}

/* -------------------------------------------------------------- random -- */

static PromptResult menu_random(App *app)
{
    Dataset fresh;
    int n, max_arrival, max_burst, max_priority, seed;
    PromptResult r;
    bool use_clock;

    ui_section("Generate a random workload");
    ui_info("The generator is reproducible: the same seed always produces "
            "the same workload on any machine.");

    r = prompt_int_default("Number of processes", 1, MAX_PROCESSES, 5, &n);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Largest arrival time", MIN_ARRIVAL, 200, 10,
                           &max_arrival);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Largest burst time", MIN_BURST, 100, 10,
                           &max_burst);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Largest priority number", MIN_PRIORITY,
                           MAX_PRIORITY, 5, &max_priority);
    if (r != PROMPT_OK) return r;
    r = prompt_yes_no("Use a clock-based seed", false, &use_clock);
    if (r != PROMPT_OK) return r;

    if (use_clock) {
        seed = (int)(dataset_random_seed() % 1000000UL);
    } else {
        r = prompt_int_default("Seed", 0, 1000000, 42, &seed);
        if (r != PROMPT_OK) return r;
    }

    if (!dataset_random(&fresh, n, (unsigned long)seed, max_arrival,
                        max_burst, max_priority)) {
        ui_error("could not generate the workload");
        dataset_free(&fresh);
        return PROMPT_OK;
    }

    replace_dataset(app, &fresh);
    ui_success("generated %d process(es) with seed %d", app->ds.count, seed);
    viz_dataset_table(&app->ds);
    return PROMPT_OK;
}

/* -------------------------------------------------------------- import -- */

static PromptResult menu_import(App *app)
{
    char path[INPUT_LINE_MAX];
    char err[ERR_MSG_MAX];
    Dataset fresh;
    PromptResult r;

    ui_section("Import a workload from CSV");
    ui_info("Expected columns: id,arrival,burst[,priority].  A header row is "
            "optional; blank lines and lines starting with # are ignored.");
    ui_info("Try: data/sample_processes.csv");

    r = prompt_text("Path to the CSV file", path, sizeof path);
    if (r != PROMPT_OK) return r;

    if (!csv_import_dataset(path, &fresh, err, sizeof err)) {
        ui_error("%s", err);
        dataset_free(&fresh);
        return PROMPT_OK;
    }

    replace_dataset(app, &fresh);
    ui_success("imported %d process(es) from %s", app->ds.count, path);
    viz_dataset_table(&app->ds);
    return PROMPT_OK;
}

/* -------------------------------------------------------------- export -- */

static PromptResult ask_path(const char *label, const char *suggestion,
                            char *buf, size_t sz)
{
    for (;;) {
        PromptResult r;
        ui_prompt("%s [Enter for %s] (q to go back): ", label, suggestion);
        r = read_line(buf, sz);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL) continue;
        trim(buf);
        if (strcmp(buf, "q") == 0) return PROMPT_CANCEL;
        if (buf[0] == '\0') snprintf(buf, sz, "%s", suggestion);
        return PROMPT_OK;
    }
}

static PromptResult menu_export(App *app)
{
    char err[ERR_MSG_MAX];
    char path[INPUT_LINE_MAX];
    int choice;
    PromptResult r;

    if (!require_workload(app)) return PROMPT_OK;

    ui_section("Export to CSV");
    ui_menu_item("1", "The current workload");
    ui_menu_item("2", "Results of one algorithm (table + summary + segments)");
    ui_menu_item("3", "Comparison of all six algorithms");
    ui_menu_item("4", "Execution timeline of one algorithm");

    r = prompt_int("What to export", 1, 4, &choice);
    if (r != PROMPT_OK) return r;

    if (choice == 1) {
        r = ask_path("File name", "workload.csv", path, sizeof path);
        if (r != PROMPT_OK) return r;
        if (csv_export_dataset(path, &app->ds, err, sizeof err))
            ui_success("wrote %s", path);
        else
            ui_error("%s", err);
        return PROMPT_OK;
    }

    if (choice == 3) {
        Comparison cmp;
        r = prompt_int_default("Time quantum for Round Robin", MIN_QUANTUM,
                               MAX_QUANTUM, app->quantum, &app->quantum);
        if (r != PROMPT_OK) return r;
        r = ask_path("File name", "comparison.csv", path, sizeof path);
        if (r != PROMPT_OK) return r;
        if (!compare_run(&cmp, &app->ds, app->quantum, err, sizeof err)) {
            ui_error("%s", err);
            compare_free(&cmp);
            return PROMPT_OK;
        }
        if (csv_export_comparison(path, &cmp, err, sizeof err))
            ui_success("wrote %s", path);
        else
            ui_error("%s", err);
        compare_free(&cmp);
        return PROMPT_OK;
    }

    /* choice 2 and 4 both need one algorithm. */
    {
        AlgorithmId algo;
        Schedule sch;
        char suggestion[64];

        r = pick_algorithm(&algo);
        if (r != PROMPT_OK) return r;
        r = ensure_quantum(app, algo);
        if (r != PROMPT_OK) return r;

        snprintf(suggestion, sizeof suggestion, "%s_%s.csv",
                 (choice == 2) ? "results" : "timeline",
                 algo_abbrev(algo));
        r = ask_path("File name", suggestion, path, sizeof path);
        if (r != PROMPT_OK) return r;

        if (!algo_run(&sch, &app->ds, algo, app->quantum, false, err,
                      sizeof err)) {
            ui_error("%s", err);
            schedule_free(&sch);
            return PROMPT_OK;
        }
        if (choice == 2) {
            if (csv_export_schedule(path, &sch, err, sizeof err))
                ui_success("wrote %s", path);
            else
                ui_error("%s", err);
        } else {
            if (csv_export_timeline(path, &sch, err, sizeof err))
                ui_success("wrote %s", path);
            else
                ui_error("%s", err);
        }
        schedule_free(&sch);
    }
    return PROMPT_OK;
}

/* ---------------------------------------------------------- edit workload */

static PromptResult edit_add(App *app)
{
    Process p;
    char idbuf[PROC_ID_MAX];
    PromptResult r;

    memset(&p, 0, sizeof p);
    if (app->ds.count >= MAX_PROCESSES) {
        ui_error("the workload already holds the maximum of %d processes",
                 MAX_PROCESSES);
        return PROMPT_OK;
    }

    r = prompt_process_id("New process id", &app->ds, idbuf, sizeof idbuf);
    if (r != PROMPT_OK) return r;
    snprintf(p.id, sizeof p.id, "%s", idbuf);

    r = prompt_int("Arrival time", MIN_ARRIVAL, MAX_ARRIVAL, &p.arrival);
    if (r != PROMPT_OK) return r;
    r = prompt_int("Burst time", MIN_BURST, MAX_BURST, &p.burst);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Priority (lower = more urgent)", MIN_PRIORITY,
                           MAX_PRIORITY, 0, &p.priority);
    if (r != PROMPT_OK) return r;

    if (!dataset_add(&app->ds, &p)) {
        ui_error("could not add the process");
        return PROMPT_OK;
    }
    ui_success("added %s", p.id);
    return PROMPT_OK;
}

static PromptResult edit_remove(App *app)
{
    int row;
    PromptResult r;
    char id[PROC_ID_MAX];

    if (app->ds.count == 0) {
        ui_warn("the workload is already empty");
        return PROMPT_OK;
    }
    viz_dataset_table(&app->ds);
    r = prompt_int("Row number to remove", 1, app->ds.count, &row);
    if (r != PROMPT_OK) return r;

    snprintf(id, sizeof id, "%s", app->ds.items[row - 1].id);
    if (dataset_remove(&app->ds, row - 1)) ui_success("removed %s", id);
    else                                   ui_error("could not remove row %d",
                                                    row);
    return PROMPT_OK;
}

static PromptResult edit_modify(App *app)
{
    int row;
    PromptResult r;
    Process *p;

    if (app->ds.count == 0) {
        ui_warn("the workload is empty");
        return PROMPT_OK;
    }
    viz_dataset_table(&app->ds);
    r = prompt_int("Row number to edit", 1, app->ds.count, &row);
    if (r != PROMPT_OK) return r;

    p = &app->ds.items[row - 1];
    ui_info("editing %s (arrival %d, burst %d, priority %d); Enter keeps the "
            "current value", p->id, p->arrival, p->burst, p->priority);

    r = prompt_int_default("Arrival time", MIN_ARRIVAL, MAX_ARRIVAL,
                           p->arrival, &p->arrival);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Burst time", MIN_BURST, MAX_BURST, p->burst,
                           &p->burst);
    if (r != PROMPT_OK) return r;
    r = prompt_int_default("Priority", MIN_PRIORITY, MAX_PRIORITY,
                           p->priority, &p->priority);
    if (r != PROMPT_OK) return r;

    ui_success("%s is now arrival %d, burst %d, priority %d", p->id,
               p->arrival, p->burst, p->priority);
    return PROMPT_OK;
}

static PromptResult menu_edit(App *app)
{
    for (;;) {
        int choice;
        PromptResult r;

        ui_section("View or edit the workload");
        ui_menu_item("1", "Show the workload table");
        ui_menu_item("2", "Add a process");
        ui_menu_item("3", "Remove a process");
        ui_menu_item("4", "Edit a process");
        ui_menu_item("5", "Rename the workload");
        ui_menu_item("6", "Sort the rows by arrival time");
        ui_menu_item("7", "Clear the workload");
        ui_menu_item("0", "Back to the main menu");

        r = prompt_int("Choice", 0, 7, &choice);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL || choice == 0) return PROMPT_OK;

        switch (choice) {
        case 1:
            show_workload_or_warn(app);
            break;
        case 2:
            r = edit_add(app);
            break;
        case 3:
            r = edit_remove(app);
            break;
        case 4:
            r = edit_modify(app);
            break;
        case 5: {
            char name[DATASET_NAME_MAX];
            r = prompt_text("New name", name, sizeof name);
            if (r == PROMPT_OK) {
                dataset_set_name(&app->ds, name);
                ui_success("renamed to \"%s\"", app->ds.name);
            }
            break;
        }
        case 6:
            dataset_sort_by_arrival(&app->ds);
            ui_success("rows sorted by arrival time");
            viz_dataset_table(&app->ds);
            break;
        case 7: {
            bool yes = false;
            r = prompt_yes_no("Really discard the whole workload", false,
                              &yes);
            if (r == PROMPT_OK && yes) {
                dataset_clear(&app->ds);
                dataset_set_name(&app->ds, "empty");
                ui_success("workload cleared");
            }
            break;
        }
        default:
            break;
        }
        if (r == PROMPT_EOF) return PROMPT_EOF;
    }
}

/* ------------------------------------------------------------ settings -- */

static PromptResult menu_settings(App *app)
{
    for (;;) {
        int choice;
        PromptResult r;
        char line[96];

        ui_section("Settings");
        snprintf(line, sizeof line, "Time quantum for Round Robin (now %d)",
                 app->quantum);
        ui_menu_item("1", line);
        snprintf(line, sizeof line, "Colors (now %s)",
                 ui_color_enabled() ? "on" : "off");
        ui_menu_item("2", line);
        snprintf(line, sizeof line, "Box drawing (now %s)",
                 ui_unicode_enabled() ? "Unicode" : "plain ASCII");
        ui_menu_item("3", line);
        snprintf(line, sizeof line, "Output width (now %d columns)",
                 ui_term_width());
        ui_menu_item("4", line);
        ui_menu_item("0", "Back to the main menu");

        r = prompt_int("Choice", 0, 4, &choice);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL || choice == 0) return PROMPT_OK;

        switch (choice) {
        case 1:
            r = prompt_int_default("Time quantum", MIN_QUANTUM, MAX_QUANTUM,
                                   app->quantum, &app->quantum);
            if (r == PROMPT_OK) ui_success("quantum set to %d", app->quantum);
            break;
        case 2:
            ui_set_color_enabled(!ui_color_enabled());
            ui_success("colors are now %s",
                       ui_color_enabled() ? "on" : "off");
            break;
        case 3:
            ui_set_unicode_enabled(!ui_unicode_enabled());
            ui_success("box drawing is now %s",
                       ui_unicode_enabled() ? "Unicode" : "plain ASCII");
            break;
        case 4: {
            int w;
            r = prompt_int_default("Output width in columns", 60, 200,
                                   ui_term_width(), &w);
            if (r == PROMPT_OK) {
                ui_set_term_width(w);
                ui_success("output width set to %d", ui_term_width());
            }
            break;
        }
        default:
            break;
        }
        if (r == PROMPT_EOF) return PROMPT_EOF;
    }
}

/* ---------------------------------------------------------------- help -- */

static PromptResult menu_help(void)
{
    for (;;) {
        int choice;
        PromptResult r;

        ui_section("Help");
        ui_menu_item("1", "Scheduling assumptions and metric definitions");
        ui_menu_item("2", "What each of the six algorithms does");
        ui_menu_item("3", "About Schedulify");
        ui_menu_item("0", "Back to the main menu");

        r = prompt_int("Choice", 0, 3, &choice);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL || choice == 0) return PROMPT_OK;

        switch (choice) {
        case 1: viz_assumptions(); break;
        case 2: viz_algorithm_rules(); break;
        case 3:
            ui_title("Schedulify %s", SCHEDULIFY_VERSION);
            ui_info("A CPU scheduling simulator written entirely in C11.");
            ui_info("Every schedule, metric and chart on screen is computed "
                    "by the C code in src/ -- nothing is hard-coded.");
            ui_info("Colors: %s   Box drawing: %s   Width: %d columns",
                    ui_color_enabled() ? "on" : "off",
                    ui_unicode_enabled() ? "Unicode" : "plain ASCII",
                    ui_term_width());
            break;
        default:
            break;
        }
    }
}

/* ----------------------------------------------------------- main menu -- */

static void main_menu_draw(const App *app)
{
    char line[128];

    ui_banner();
    if (app->ds.count > 0) {
        printf("  %scurrent workload:%s %s%s%s  %s(%d process%s, quantum "
               "%d)%s\n",
               ui_c(C_DIM), ui_c(C_RESET), ui_c(C_OK), app->ds.name,
               ui_c(C_RESET), ui_c(C_DIM), app->ds.count,
               (app->ds.count == 1) ? "" : "es", app->quantum,
               ui_c(C_RESET));
    } else {
        printf("  %sno workload loaded yet%s\n", ui_c(C_DIM), ui_c(C_RESET));
    }

    ui_section("Main menu");
    ui_menu_item("1", "Run one algorithm");
    ui_menu_item("2", "Compare all six algorithms on this workload");
    ui_menu_item("3", "Step-by-step educational mode");
    ui_menu_item("4", "Load a predefined example");
    ui_menu_item("5", "Enter a custom workload");
    ui_menu_item("6", "Generate a random workload");
    ui_menu_item("7", "Import a workload from CSV");
    ui_menu_item("8", "Export to CSV");
    ui_menu_item("9", "View or edit the current workload");
    snprintf(line, sizeof line, "Settings (quantum %d, colors %s, %s)",
             app->quantum, ui_color_enabled() ? "on" : "off",
             ui_unicode_enabled() ? "Unicode" : "ASCII");
    ui_menu_item("10", line);
    ui_menu_item("11", "Help: assumptions, metrics and algorithm rules");
    ui_menu_item("0", "Quit");
}

static void run_menu(App *app)
{
    for (;;) {
        int choice;
        PromptResult r;

        main_menu_draw(app);
        r = prompt_int("Choice", 0, 11, &choice);
        if (r == PROMPT_EOF) {
            printf("\n");
            ui_note("end of input -- exiting");
            return;
        }
        if (r == PROMPT_CANCEL) continue;
        if (choice == 0) {
            ui_success("goodbye");
            return;
        }

        switch (choice) {
        case 1:  r = menu_run_single(app); break;
        case 2:  r = menu_compare(app);    break;
        case 3:  r = menu_step(app);       break;
        case 4:  r = menu_examples(app);   break;
        case 5:  r = menu_custom(app);     break;
        case 6:  r = menu_random(app);     break;
        case 7:  r = menu_import(app);     break;
        case 8:  r = menu_export(app);     break;
        case 9:  r = menu_edit(app);       break;
        case 10: r = menu_settings(app);   break;
        case 11: r = menu_help();          break;
        default: r = PROMPT_OK;            break;
        }
        if (r == PROMPT_EOF) {
            printf("\n");
            ui_note("end of input -- exiting");
            return;
        }
        if (prompt_pause("\n  Press Enter to return to the main menu... ")
                == PROMPT_EOF) {
            printf("\n");
            return;
        }
    }
}

/* ------------------------------------------------------------------ CLI -- */

static void print_usage(void)
{
    printf("Schedulify %s -- CPU scheduling simulator (C11)\n\n",
           SCHEDULIFY_VERSION);
    printf("Usage: schedulify [options]\n\n");
    printf("Workload selection\n");
    printf("  --csv FILE            load a workload from a CSV file\n");
    printf("  --example N           load predefined example N (1..%d)\n",
           examples_count());
    printf("  --random N[:SEED]     generate N random processes\n");
    printf("\nActions (any of these skips the interactive menu)\n");
    printf("  --algo NAME           run one algorithm: fcfs, sjf, srtf,\n");
    printf("                        prio-np, prio-p, rr\n");
    printf("  --compare             run and compare all six algorithms\n");
    printf("  --step                step-by-step mode (needs --algo)\n");
    printf("  --assumptions         print the documented assumptions\n");
    printf("  --rules               print what each algorithm does\n");
    printf("  --list-examples       list the predefined examples\n");
    printf("  --export-results F    write one algorithm's results to F\n");
    printf("  --export-comparison F write the comparison table to F\n");
    printf("  --export-workload F   write the workload to F\n");
    printf("  --menu                start the menu even after an action\n");
    printf("\nOptions\n");
    printf("  --quantum N           Round Robin time quantum (default %d)\n",
           DEFAULT_QUANTUM);
    printf("  --color | --no-color  force ANSI colors on or off\n");
    printf("  --unicode | --ascii   force box-drawing glyphs or plain text\n");
    printf("  --width N             output width in columns (60..200)\n");
    printf("  -h, --help            this message\n");
    printf("  -V, --version         version only\n");
    printf("\nExamples\n");
    printf("  schedulify --example 2 --algo srtf\n");
    printf("  schedulify --csv data/sample_processes.csv --compare "
           "--quantum 3\n");
    printf("  schedulify --random 8:7 --algo rr --quantum 4 --ascii\n");
}

static bool parse_algo_name(const char *s, AlgorithmId *out)
{
    struct { const char *name; AlgorithmId id; } map[] = {
        { "fcfs",      ALGO_FCFS },
        { "fifo",      ALGO_FCFS },
        { "sjf",       ALGO_SJF },
        { "sjn",       ALGO_SJF },
        { "srtf",      ALGO_SRTF },
        { "srt",       ALGO_SRTF },
        { "prio-np",   ALGO_PRIORITY_NP },
        { "priority",  ALGO_PRIORITY_NP },
        { "prio",      ALGO_PRIORITY_NP },
        { "prio-p",    ALGO_PRIORITY_P },
        { "priority-p", ALGO_PRIORITY_P },
        { "rr",        ALGO_RR },
        { "roundrobin", ALGO_RR }
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(s, map[i].name) == 0) { *out = map[i].id; return true; }
    }
    return false;
}

int main(int argc, char **argv)
{
    App app;
    int i;
    bool did_action = false;
    bool want_menu = false;
    bool force_menu = false;
    bool want_compare = false;
    bool want_step = false;
    bool have_algo = false;
    AlgorithmId algo = ALGO_FCFS;
    const char *export_results = NULL;
    const char *export_comparison = NULL;
    const char *export_workload = NULL;
    char err[ERR_MSG_MAX];

    ui_init();
    dataset_init(&app.ds, "empty");
    app.quantum = DEFAULT_QUANTUM;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage();
            dataset_free(&app.ds);
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("Schedulify %s\n", SCHEDULIFY_VERSION);
            dataset_free(&app.ds);
            return 0;
        }
        if (strcmp(a, "--no-color") == 0) { ui_set_color_enabled(false);
                                            continue; }
        if (strcmp(a, "--color") == 0)    { ui_set_color_enabled(true);
                                            continue; }
        if (strcmp(a, "--ascii") == 0)    { ui_set_unicode_enabled(false);
                                            continue; }
        if (strcmp(a, "--unicode") == 0)  { ui_set_unicode_enabled(true);
                                            continue; }
        if (strcmp(a, "--menu") == 0)     { force_menu = true; continue; }
        if (strcmp(a, "--compare") == 0)  { want_compare = true; continue; }
        if (strcmp(a, "--step") == 0)     { want_step = true; continue; }

        if (strcmp(a, "--assumptions") == 0) {
            viz_assumptions();
            did_action = true;
            continue;
        }
        if (strcmp(a, "--rules") == 0) {
            viz_algorithm_rules();
            did_action = true;
            continue;
        }
        if (strcmp(a, "--list-examples") == 0) {
            int k;
            printf("Predefined examples:\n");
            for (k = 0; k < examples_count(); k++) {
                printf("  %2d  %-34s (suggested quantum %d)\n", k + 1,
                       example_name(k), example_quantum(k));
            }
            did_action = true;
            continue;
        }

        /* options that take a value */
        if (i + 1 >= argc) {
            fprintf(stderr, "schedulify: option %s needs a value\n", a);
            dataset_free(&app.ds);
            return 2;
        }

        if (strcmp(a, "--width") == 0) {
            int w;
            if (!parse_int_range(argv[++i], 60, 200, &w)) {
                fprintf(stderr, "schedulify: --width needs 60..200\n");
                dataset_free(&app.ds);
                return 2;
            }
            ui_set_term_width(w);
            continue;
        }
        if (strcmp(a, "--quantum") == 0) {
            if (!parse_int_range(argv[++i], MIN_QUANTUM, MAX_QUANTUM,
                                 &app.quantum)) {
                fprintf(stderr, "schedulify: --quantum needs %d..%d\n",
                        MIN_QUANTUM, MAX_QUANTUM);
                dataset_free(&app.ds);
                return 2;
            }
            continue;
        }
        if (strcmp(a, "--algo") == 0) {
            if (!parse_algo_name(argv[++i], &algo)) {
                fprintf(stderr, "schedulify: unknown algorithm \"%s\"\n",
                        argv[i]);
                dataset_free(&app.ds);
                return 2;
            }
            have_algo = true;
            continue;
        }
        if (strcmp(a, "--csv") == 0) {
            Dataset fresh;
            if (!csv_import_dataset(argv[++i], &fresh, err, sizeof err)) {
                fprintf(stderr, "schedulify: %s\n", err);
                dataset_free(&fresh);
                dataset_free(&app.ds);
                return 1;
            }
            replace_dataset(&app, &fresh);
            continue;
        }
        if (strcmp(a, "--example") == 0) {
            Dataset fresh;
            int n;
            if (!parse_int_range(argv[++i], 1, examples_count(), &n)) {
                fprintf(stderr, "schedulify: --example needs 1..%d\n",
                        examples_count());
                dataset_free(&app.ds);
                return 2;
            }
            if (!example_load(n - 1, &fresh)) {
                fprintf(stderr, "schedulify: could not load example %d\n", n);
                dataset_free(&app.ds);
                return 1;
            }
            replace_dataset(&app, &fresh);
            app.quantum = example_quantum(n - 1);
            continue;
        }
        if (strcmp(a, "--random") == 0) {
            Dataset fresh;
            char spec[64];
            char *colon;
            int n = 5;
            int seed = 42;
            snprintf(spec, sizeof spec, "%s", argv[++i]);
            colon = strchr(spec, ':');
            if (colon != NULL) {
                *colon = '\0';
                if (!parse_int_range(colon + 1, 0, 1000000, &seed)) {
                    fprintf(stderr, "schedulify: bad seed in --random\n");
                    dataset_free(&app.ds);
                    return 2;
                }
            }
            if (!parse_int_range(spec, 1, MAX_PROCESSES, &n)) {
                fprintf(stderr, "schedulify: --random needs N[:SEED]\n");
                dataset_free(&app.ds);
                return 2;
            }
            if (!dataset_random(&fresh, n, (unsigned long)seed, 10, 10, 5)) {
                fprintf(stderr, "schedulify: could not generate workload\n");
                dataset_free(&fresh);
                dataset_free(&app.ds);
                return 1;
            }
            replace_dataset(&app, &fresh);
            continue;
        }
        if (strcmp(a, "--export-results") == 0) {
            export_results = argv[++i];
            continue;
        }
        if (strcmp(a, "--export-comparison") == 0) {
            export_comparison = argv[++i];
            continue;
        }
        if (strcmp(a, "--export-workload") == 0) {
            export_workload = argv[++i];
            continue;
        }

        fprintf(stderr, "schedulify: unknown option \"%s\" "
                        "(try --help)\n", a);
        dataset_free(&app.ds);
        return 2;
    }

    /* ---- batch actions ---- */

    if (export_workload != NULL) {
        if (app.ds.count == 0) {
            fprintf(stderr, "schedulify: --export-workload needs a workload "
                            "(--csv, --example or --random)\n");
            dataset_free(&app.ds);
            return 2;
        }
        if (!csv_export_dataset(export_workload, &app.ds, err, sizeof err)) {
            fprintf(stderr, "schedulify: %s\n", err);
            dataset_free(&app.ds);
            return 1;
        }
        printf("wrote %s\n", export_workload);
        did_action = true;
    }

    if (have_algo || want_step) {
        Schedule sch;
        if (app.ds.count == 0) {
            fprintf(stderr, "schedulify: --algo needs a workload "
                            "(--csv, --example or --random)\n");
            dataset_free(&app.ds);
            return 2;
        }
        if (want_step) {
            /* step_mode_run prints the workload table itself */
            step_mode_run(&app.ds, algo, app.quantum);
            did_action = true;
        } else {
            if (!algo_run(&sch, &app.ds, algo, app.quantum, false, err,
                          sizeof err)) {
                fprintf(stderr, "schedulify: %s\n", err);
                schedule_free(&sch);
                dataset_free(&app.ds);
                return 1;
            }
            viz_dataset_table(&app.ds);
            viz_report(&sch);
            if (export_results != NULL) {
                if (csv_export_schedule(export_results, &sch, err,
                                        sizeof err))
                    printf("\nwrote %s\n", export_results);
                else
                    fprintf(stderr, "schedulify: %s\n", err);
            }
            schedule_free(&sch);
            did_action = true;
        }
    } else if (export_results != NULL) {
        fprintf(stderr, "schedulify: --export-results needs --algo\n");
        dataset_free(&app.ds);
        return 2;
    }

    if (want_compare || export_comparison != NULL) {
        Comparison cmp;
        if (app.ds.count == 0) {
            fprintf(stderr, "schedulify: --compare needs a workload "
                            "(--csv, --example or --random)\n");
            dataset_free(&app.ds);
            return 2;
        }
        if (!compare_run(&cmp, &app.ds, app.quantum, err, sizeof err)) {
            fprintf(stderr, "schedulify: %s\n", err);
            compare_free(&cmp);
            dataset_free(&app.ds);
            return 1;
        }
        if (want_compare) {
            if (!have_algo) viz_dataset_table(&app.ds);
            compare_report(&cmp);
        }
        if (export_comparison != NULL) {
            if (csv_export_comparison(export_comparison, &cmp, err,
                                      sizeof err))
                printf("\nwrote %s\n", export_comparison);
            else
                fprintf(stderr, "schedulify: %s\n", err);
        }
        compare_free(&cmp);
        did_action = true;
    }

    want_menu = force_menu || !did_action;
    if (want_menu) run_menu(&app);

    dataset_free(&app.ds);
    return 0;
}
