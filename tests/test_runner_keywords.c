#include <stdio.h>
#include <string.h>

#include "../src/runner_desktop.c"
#include "../src/runner_key_state.h"

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
    RunnerKeyState key_state = {{0}};
    char keymap[RUNNER_KEYMAP_SIZE] = {0};
    const unsigned int shortcut_keycode = 38;
    const unsigned int modifier_keycode = 64;
    keymap[shortcut_keycode / 8] |=
        (char)(1u << (shortcut_keycode % 8));
    keymap[modifier_keycode / 8] |=
        (char)(1u << (modifier_keycode % 8));
    runner_key_state_capture(&key_state, keymap);
    if (!check(runner_key_state_ignores_press(&key_state, shortcut_keycode),
               "ignores the shortcut key held when the launcher opens") ||
        !check(runner_key_state_ignores_press(&key_state, modifier_keycode),
               "ignores the held Alt modifier") ||
        !check(!runner_key_state_ignores_press(&key_state, 65),
               "does not ignore keys that were not held at launch")) {
        return 1;
    }
    runner_key_state_release(&key_state, shortcut_keycode, 1);
    if (!check(runner_key_state_ignores_press(&key_state, shortcut_keycode),
               "keeps ignoring a key across an auto-repeat release")) {
        return 1;
    }
    runner_key_state_release(&key_state, shortcut_keycode, 0);
    if (!check(!runner_key_state_ignores_press(&key_state, shortcut_keycode),
               "accepts a key again after its real release")) {
        return 1;
    }

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
