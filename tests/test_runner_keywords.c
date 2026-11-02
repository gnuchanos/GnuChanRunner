#include <stdio.h>
#include <string.h>

#include "../src/runner_desktop.c"

int runner_apps_split_command(RunnerProgram *program, const char *command) {
    (void)program;
    (void)command;
    return -1;
}

static int check(int condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "failed: %s\n", description);
        return 0;
    }
    return 1;
}

int main(void) {
    char generic[RUNNER_TEXT_LENGTH];
    memset(generic, 'g', sizeof(generic) - 1);
    generic[sizeof(generic) - 1] = '\0';

    char combined[RUNNER_TEXT_LENGTH];
    set_keywords(combined, sizeof(combined), generic, "searchneedle");
    if (!check(strlen(combined) < sizeof(combined),
               "keeps the combined field terminated") ||
        !check(strstr(combined, "searchneedle") != NULL,
               "retains keywords when generic name fills the field")) {
        return 1;
    }

    set_keywords(combined, sizeof(combined), "Utilities", "search");
    if (!check(strcmp(combined, "Utilities search") == 0,
               "preserves ordinary generic names and keywords")) {
        return 1;
    }

    char tiny[3];
    set_keywords(tiny, sizeof(tiny), "Utilities", "search");
    if (!check(strcmp(tiny, "s") == 0,
               "does not spend a tiny buffer on a dangling separator")) {
        return 1;
    }

    puts("runner keyword tests passed");
    return 0;
}
