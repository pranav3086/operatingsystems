/*
 * test_main.c -- Test runner.
 *
 * Build and run with:   make test     (or ctest, under CMake)
 * Exit status is 0 only when every check passed.
 */
#include <stdio.h>

#include "suites.h"
#include "test_util.h"
#include "ui.h"

int main(void)
{
    /* Deterministic, pipe-safe output: no colors, plain ASCII, fixed width. */
    ui_init();
    ui_set_color_enabled(false);
    ui_set_unicode_enabled(false);
    ui_set_term_width(100);

    printf("Schedulify unit tests\n");
    printf("=====================\n");

    suite_validation();
    suite_metrics();
    suite_algorithms();
    suite_csvio();
    suite_web();

    return t_report();
}
