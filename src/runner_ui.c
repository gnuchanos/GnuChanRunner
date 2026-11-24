/*
 * runner_ui.c — the window, the grab, and the keys.
 *
 * This is where the launcher meets the display. Everything it is made of has
 * already been read and resolved by the time this runs: the config, the
 * palette, the font, the list of programs. What is left is a window, a grab,
 * and a loop that turns key presses into a query, a query into matches, and a
 * press of Enter into a choice.
 *
 * The grab is the part worth stating. The launcher takes the keyboard and the
 * pointer for as long as it is up:
 *
 *   The keyboard, because the first letter typed has to reach the launcher and
 *   not whatever had the focus before it. Without it a person opens the
 *   launcher, types "fire", and watches a letter appear in the terminal
 *   underneath.
 *
 *   The pointer, because a click anywhere has to be the launcher's — either on
 *   a row, or somewhere that means "go away". Without it a click on the
 *   desktop would go to the desktop and the launcher would sit there.
 *
 * Escape dismisses it, Enter chooses, the arrows move, and a printable letter
 * is appended to the query. That is the whole of the input, and it is small
 * because a launcher's input should be: anything more elaborate is a program
 * with a window, not a thing that starts one.
 */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include "runner_draw.h"
#include "runner_ui.h"

static XErrorHandler runner_previous_x_error_handler;
static int runner_x_error_handler_installed;
static int runner_x_error_seen;

static int runner_x_error(Display *display, XErrorEvent *error) {
    char text[256];
    XGetErrorText(display, error->error_code, text, sizeof(text));
    fprintf(stderr, "gnuchanrunner: X error: %s (request %u.%u)\n", text,
            error->request_code, error->minor_code);
    runner_x_error_seen = 1;
    return 0;
}

static void begin_checked_x_request(Display *display) {
    XSync(display, False);
    runner_x_error_seen = 0;
}

static int finish_checked_x_request(Display *display) {
    XSync(display, False);
    int succeeded = !runner_x_error_seen;
    runner_x_error_seen = 0;
    return succeeded;
}

/* Where the window sits on the screen: centred, hugging the top, or hugging
   the bottom, each with a margin so it is not flush against the edge. The
   position names come from the config, and anything the launcher does not know
   is centred — a misspelled position should put the window somewhere, not
   nowhere. */
static void work_out_geometry(RunnerUi *ui) {
    int rows = ui->config.rows;
    if (rows < 1) rows = 1;

    int row_count = ui->matches.count + (ui->command_row >= 0 ? 1 : 0);
    int visible = row_count < rows ? row_count : rows;
    if (visible < 1) {
        visible = 1;
    }

    int row_height = ui->style.row_height;
    int top_gap = 6;
    int max_visible = (ui->screen_height - row_height - top_gap - 2) / row_height;
    if (max_visible < 1) {
        max_visible = 1;
    }
    if (visible > max_visible) {
        visible = max_visible;
    }

    /* Start from configured width, then grow to fit visible row content so long
       names/descriptions are readable instead of being cut with ellipsis. */
    ui->width = ui->config.width;
    int desired = ui->width;
    int right_pad = ui->style.padding + 24;
    int left_pad = ui->style.padding + 10;
    int limit = ui->screen_width - 80;
    if (limit < ui->config.width) {
        limit = ui->config.width;
    }
    if (limit > ui->screen_width - 20) {
        limit = ui->screen_width - 20;
    }

    for (int row = 0; row < visible; row++) {
        int index = ui->scroll + row;
        if (index < 0 || index >= row_count) {
            continue;
        }

        const char *name = NULL;
        const char *comment = NULL;
        char cmd[RUNNER_MAX_QUERY + 32];
        if (index == ui->command_row) {
            if (ui->query[0]) {
                snprintf(cmd, sizeof(cmd), "Run command: %s", ui->query);
            } else {
                snprintf(cmd, sizeof(cmd), "Run command");
            }
            name = cmd;
            comment = "Execute in shell";
        } else {
            const RunnerProgram *program =
                &ui->programs.items[ui->matches.items[index].program_index];
            name = program->name;
            comment = program->comment;
        }

        int w = left_pad + runner_style_text_width(ui->display, ui->style.font,
                                                    name ? name : "") +
                right_pad;
        if (comment && comment[0]) {
            w += ui->style.padding +
                 runner_style_text_width(ui->display, ui->style.font, comment);
        }
        if (w > desired) desired = w;
    }

    if (desired > limit) desired = limit;
    if (desired < 320) desired = 320;
    ui->width = desired;
    if (ui->width > ui->screen_width) {
        ui->width = ui->screen_width;
    }

    /* The height is the query line, a small gap, the visible rows, and border
       room. It grows with available results up to the configured row count so
       the launcher stays stable while typing. */
    ui->height = ui->style.row_height + top_gap
               + visible * ui->style.row_height + 2;
    if (ui->height > ui->screen_height) {
        ui->height = ui->screen_height;
    }

    int x = (ui->screen_width - ui->width) / 2;
    int y;

    if (strcmp(ui->config.position, "top") == 0) {
        y = 80;
    } else if (strcmp(ui->config.position, "bottom") == 0) {
        y = ui->screen_height - ui->height - 80;
        if (y < 0) {
            y = 0;
        }
    } else {
        y = (ui->screen_height - ui->height) / 3;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XMoveResizeWindow(ui->display, ui->window, x, y,
                      (unsigned int)ui->width, (unsigned int)ui->height);
}

/* Work out where the command line belongs in the matches, and whether it is
   offered at all.
 *
 * There is one command line and it is always either at the end of the list or
 * absent. It is absent when the config turned it off, and when the config
 * offers it only for a query that matched nothing and the query did match
 * something. It is present otherwise — after the last match, so choosing the
 * first program is still one press of Enter with nothing typed.
 */
static void work_out_command_row(RunnerUi *ui) {
    ui->command_row = -1;
    if (ui->config.command_mode == RUNNER_COMMAND_OFF) {
        return;
    }
    if (ui->query_length == 0) {
        /* With nothing typed there is no command line: a row that would run an
           empty command is a row that does nothing. */
        return;
    }
    if (ui->config.command_mode == RUNNER_COMMAND_TYPED &&
        ui->matches.count > 0) {
        /* Offered only once nothing matches — see the config's own note. */
        return;
    }
    ui->command_row = ui->matches.count;
}

/* Re-run the query and put the list back at a sane place.
 *
 * The chosen row and the scroll are set here rather than by each key: every
 * key that changes the query makes the old chosen index mean a different
 * program, so a list that kept it would jump to an unrelated entry on the
 * first letter typed. Choosing the first match is what a launcher does, and it
 * is what makes Enter work with nothing selected by hand.
 */
static void refresh(RunnerUi *ui) {
    runner_match_query(&ui->matches, &ui->programs, &ui->config, ui->query);
    work_out_command_row(ui);

    /* The first row is chosen: the best match, or — when nothing matched and a
       command line is offered — the command line itself, so typing a command
       and pressing Enter runs it without a second keystroke. */
    if (ui->matches.count > 0) {
        ui->selected = 0;
    } else if (ui->command_row >= 0) {
        ui->selected = ui->command_row;
    } else {
        ui->selected = -1;
    }
    ui->scroll = 0;
    work_out_geometry(ui);
}

/* How many rows fit in the window, which is what the scroll is kept inside. */
static int visible_rows(const RunnerUi *ui) {
    int rows = ui->config.rows;
    int room = (ui->height - ui->style.row_height - 8) / ui->style.row_height;
    if (room < rows) {
        rows = room;
    }
    return rows < 1 ? 1 : rows;
}

/* The number of rows the list has: the matches, plus the command line when it
   is offered. */
static int total_rows(const RunnerUi *ui) {
    return ui->matches.count + (ui->command_row >= 0 ? 1 : 0);
}

/* Keep the chosen row inside the visible window, moving the scroll under it
   rather than the row. This is what makes the arrows work at the end of a long
   list: the chosen row walks off the bottom and the list follows. */
static void keep_selected_visible(RunnerUi *ui) {
    int visible = visible_rows(ui);
    if (ui->selected < ui->scroll) {
        ui->scroll = ui->selected;
    }
    if (ui->selected >= ui->scroll + visible) {
        ui->scroll = ui->selected - visible + 1;
    }
    if (ui->scroll < 0) {
        ui->scroll = 0;
    }
}

/* Move the chosen row by `step`, wrapping round the ends. Wrapping rather than
   stopping is what a launcher does — the list is short and the two ends are
   next to each other in a person's mind — and it is one line of arithmetic. */
static void move_selection(RunnerUi *ui, int step) {
    int rows = total_rows(ui);
    if (rows <= 0) {
        ui->selected = -1;
        return;
    }
    if (ui->selected < 0) {
        ui->selected = 0;
    } else {
        ui->selected += step;
        while (ui->selected < 0) {
            ui->selected += rows;
        }
        while (ui->selected >= rows) {
            ui->selected -= rows;
        }
    }
    keep_selected_visible(ui);
}

/* Append one typed character to the query, if it is printable and there is
   room. A control character is not printable and has no place in a program's
   name; the ceiling is what keeps a key held down from growing the buffer. */
static int query_append(RunnerUi *ui, const char *text) {
    if (!text || !text[0]) {
        return 0;
    }
    size_t length = strlen(text);
    if (ui->query_length + (int)length >= RUNNER_MAX_QUERY) {
        return 0;
    }
    memcpy(ui->query + ui->query_length, text, length);
    ui->query_length += (int)length;
    ui->query[ui->query_length] = '\0';
    return 1;
}

static int query_backspace(RunnerUi *ui) {
    if (ui->query_length == 0) {
        return 0;
    }
    ui->query_length--;
    ui->query[ui->query_length] = '\0';
    return 1;
}

/* Choose whatever row is selected. A program row sets launch_selected to the
   program's index; the command row puts what was typed into launch_command.
   The two are told apart here and nowhere else, which is what keeps "what does
   Enter do" one question with one answer. */
static void choose_selected(RunnerUi *ui) {
    if (ui->selected < 0) {
        return;
    }
    if (ui->selected == ui->command_row) {
        snprintf(ui->launch_command, sizeof(ui->launch_command), "%s",
                 ui->query);
        ui->launch_selected = -1;
        ui->running = 0;
        return;
    }
    if (ui->selected < ui->matches.count) {
        ui->launch_selected = ui->matches.items[ui->selected].program_index;
        ui->launch_command[0] = '\0';
        ui->running = 0;
    }
}

/* Choose the nth row of the visible window, for a click: the row a click
   landed on, not the one the keyboard was on. */
static void choose_clicked_row(RunnerUi *ui, int y) {
    int top = ui->style.row_height + 6;
    if (y < top) {
        return;         /* the click was on the query line */
    }
    int row = (y - top) / ui->style.row_height;
    int index = ui->scroll + row;
    if (index < 0 || index >= total_rows(ui)) {
        return;
    }
    ui->selected = index;
    choose_selected(ui);
}

/* --- the keys ------------------------------------------------------------- */

static void handle_key(RunnerUi *ui, XKeyEvent *key) {
    /* The keysym is looked up with the shift and caps state the event carries,
       so a capital letter and the digit above it are told apart. A keysym of
       NoSymbol is a key with nothing on it, which is nothing to do. */
    KeySym symbol = XLookupKeysym(key, 0);
    if (symbol == NoSymbol) {
        return;
    }

    switch (symbol) {
    case XK_Escape:
        ui->running = 0;
        ui->launch_selected = -1;
        ui->launch_command[0] = '\0';
        return;

    case XK_Return:
    case XK_KP_Enter:
        choose_selected(ui);
        return;

    case XK_BackSpace:
        if (query_backspace(ui)) {
            refresh(ui);
            runner_draw(ui);
        }
        return;

    case XK_Up:
    case XK_KP_Up:
        move_selection(ui, -1);
        runner_draw(ui);
        return;

    case XK_Down:
    case XK_KP_Down:
        move_selection(ui, 1);
        runner_draw(ui);
        return;

    case XK_Page_Up:
        move_selection(ui, -visible_rows(ui));
        runner_draw(ui);
        return;

    case XK_Page_Down:
        move_selection(ui, visible_rows(ui));
        runner_draw(ui);
        return;

    case XK_Home:
        ui->selected = 0;
        keep_selected_visible(ui);
        runner_draw(ui);
        return;

    case XK_End:
        ui->selected = total_rows(ui) - 1;
        keep_selected_visible(ui);
        runner_draw(ui);
        return;

    default:
        break;
    }

    /* A letter, a digit, a space, a punctuation mark: whatever the key
       produced as text. XLookupString writes the character the key stands for
       in the current layout, which is why it is asked rather than the keysym
       being turned into a character by hand — a keyboard that is not the one
       the launcher was written on still types what it says it types. */
    char text[8];
    int length = XLookupString(key, text, sizeof(text) - 1, NULL, NULL);
    if (length <= 0) {
        return;
    }
    text[length] = '\0';

    /* Only characters that show on a line: a control character is not part of
       a program's name, and appending one would put an invisible byte in the
       query that a later backspace would have to remove one at a time. */
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f) {
            return;
        }
    }

    if (query_append(ui, text)) {
        refresh(ui);
        runner_draw(ui);
    }
}

/* X reports an auto-repeat as a release followed immediately by a press with
   the same timestamp. Do not treat that synthetic release as the end of a key
   that was already down when the launcher took the keyboard. */
static int key_release_is_repeat(Display *display, XKeyEvent *release) {
    if (XPending(display) == 0) {
        return 0;
    }
    XEvent next;
    XPeekEvent(display, &next);
    return next.type == KeyPress &&
           next.xkey.keycode == release->keycode &&
           next.xkey.time == release->time;
}

/* --- the window ----------------------------------------------------------- */

/* Take the keyboard and, when possible, the pointer for as long as the
 * launcher is up.
 *
 * The window is override-redirect, so the window manager may never focus it for
 * us. We ask for input focus and try to grab the keyboard so typed keys reach
 * the launcher instead of the window underneath. Pointer grabbing is useful for
 * click-to-launch/click-outside behavior, but some environments refuse it
 * transiently; the launcher can still work from the keyboard, so pointer grab is
 * treated as best-effort.
 *
 * Grabs are retried because GrabNotViewable and AlreadyGrabbed can be brief
 * states while the map/focus settles or a previous instance has just released.
 */
static int grab_input(RunnerUi *ui) {
    /* A previous GnuChanRunner process that crashed may have left grabs active.
       Clearing them first avoids a whole retry window spent on AlreadyGrabbed. */
    XUngrabKeyboard(ui->display, CurrentTime);
    XUngrabPointer(ui->display, CurrentTime);
    XSync(ui->display, False);

    begin_checked_x_request(ui->display);
    int focus_request = XSetInputFocus(ui->display, ui->window,
                                       RevertToPointerRoot, CurrentTime);
    int focus_request_succeeded = finish_checked_x_request(ui->display);
    if (!focus_request || !focus_request_succeeded) {
        fprintf(stderr, "gnuchanrunner: warning: could not set input focus\n");
    }

    int pointer = GrabNotViewable;
    int keyboard = GrabNotViewable;
    int pointer_grabbed = 0;
    int keyboard_grabbed = 0;

    for (int attempt = 0; attempt < 80; attempt++) {
        if (!keyboard_grabbed) {
            keyboard = XGrabKeyboard(ui->display, ui->window, False,
                                     GrabModeAsync, GrabModeAsync,
                                     CurrentTime);
            keyboard_grabbed = keyboard == GrabSuccess;
        }
        if (!pointer_grabbed) {
            pointer = XGrabPointer(
                ui->display, ui->window, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            pointer_grabbed = pointer == GrabSuccess;
        }
        if (keyboard_grabbed && pointer_grabbed) {
            return 1;
        }
        usleep(10000);
    }

    if (!keyboard_grabbed) {
        if (pointer_grabbed) {
            XUngrabPointer(ui->display, CurrentTime);
        }
        fprintf(stderr, "gnuchanrunner: could not grab keyboard (keyboard %d, "
                        "pointer %d)\n", keyboard, pointer);
        return 0;
    }

    if (!pointer_grabbed) {
        /* Keyboard-only mode: typing/Enter/Escape still work. Click-outside
           detection is less reliable without a pointer grab, but failing to open
           was worse. */
        fprintf(stderr, "gnuchanrunner: warning: running without pointer grab "
                        "(pointer %d)\n", pointer);
    }
    return 1;
}

static void ungrab_input(RunnerUi *ui) {
    XUngrabKeyboard(ui->display, CurrentTime);
    XUngrabPointer(ui->display, CurrentTime);
}

int runner_ui_open(RunnerUi *ui, const char *config_path) {
    memset(ui, 0, sizeof(*ui));
    ui->launch_selected = -1;
    ui->command_row = -1;
    ui->selected = -1;

    runner_apps_init(&ui->programs);
    runner_match_init(&ui->matches);

    /* The locale first, so XLookupString returns the characters the keyboard
       actually types rather than assuming Latin-1. */
    setlocale(LC_ALL, "");

    ui->display = XOpenDisplay(NULL);
    if (!ui->display) {
        fprintf(stderr, "gnuchanrunner: cannot open the X display\n");
        return -1;
    }
    runner_previous_x_error_handler =
        XSetErrorHandler(runner_x_error);
    runner_x_error_handler_installed = 1;
    ui->screen = DefaultScreen(ui->display);
    ui->root = RootWindow(ui->display, ui->screen);
    ui->screen_width = DisplayWidth(ui->display, ui->screen);
    ui->screen_height = DisplayHeight(ui->display, ui->screen);

    /* The settings, then the palette, then the programs. In that order: the
       palette is built from the config's colour names, and the scan is given
       the config's own list of directories. */
    char found_path[RUNNER_TEXT_LENGTH * 2];
    const char *path = config_path;
    if (!path || !path[0]) {
        path = runner_config_path(found_path, sizeof(found_path));
    }
    if (runner_config_load(&ui->config, path) != 0) {
        /* A settings file that could not be read is not fatal: the defaults
           are already in place — runner_config_load lays them down first — and
           the launcher starts with them rather than not starting. The reason
           is written down where someone can read it. */
        fprintf(stderr, "gnuchanrunner: %s; using the defaults\n",
                ui->config.error);
    }

    runner_style_load(&ui->style, ui->display, ui->screen, &ui->config);
    runner_apps_load(&ui->programs, &ui->config);

    ui->gc = XCreateGC(ui->display, ui->root, 0, NULL);
    if (!ui->gc) {
        fprintf(stderr, "gnuchanrunner: cannot make a graphics context\n");
        return -1;
    }
    ui->buffer = None;
    ui->buffer_width = 0;
    ui->buffer_height = 0;

    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    /* Override-redirect: the window manager never frames this window. A
       launcher with a title bar is not a launcher, and the window is up for a
       few seconds — there is nothing to manage. */
    attributes.override_redirect = True;
    attributes.background_pixel = ui->style.background;
    attributes.border_pixel = 0;
    attributes.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                            ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | StructureNotifyMask |
                            VisibilityChangeMask;

    ui->window = XCreateWindow(ui->display, ui->root, 0, 0,
                               (unsigned int)ui->config.width, 100, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel |
                               CWBorderPixel | CWEventMask, &attributes);
    if (ui->window == None) {
        fprintf(stderr, "gnuchanrunner: cannot make the window\n");
        return -1;
    }

    /* The launcher watches the root for new windows, so it can stay in front of
       them. It is override-redirect, so the manager frames nothing of it and
       never restacks it on its behalf — but the manager DOES map and raise the
       windows it manages, and a program that opens while the launcher is up is
       put above it and hides it. Selecting SubstructureNotifyMask here is what
       makes each such map or raise arrive as an event (see the root cases in
       runner_ui_run), and the answer to each is one XRaiseWindow putting the
       launcher back on top. */
    begin_checked_x_request(ui->display);
    int selected = XSelectInput(ui->display, ui->root,
                                SubstructureNotifyMask);
    if (!selected || !finish_checked_x_request(ui->display)) {
        fprintf(stderr, "gnuchanrunner: cannot watch root window events\n");
        return -1;
    }

    /* The query is run once before the window is placed, so the window opens
       at the size of what it is going to show rather than at the size of
       nothing and then growing. */
    refresh(ui);
    begin_checked_x_request(ui->display);
    int mapped = XMapRaised(ui->display, ui->window);
    if (!mapped || !finish_checked_x_request(ui->display)) {
        fprintf(stderr, "gnuchanrunner: cannot map the launcher window\n");
        return -1;
    }
    /* The window has to be on screen before the keyboard is grabbed. XFlush
       only sends the map request and returns; the window is still unmapped on
       the server when the grab arrives, and a grab on an unmapped window is
       refused with GrabNotViewable. XSync waits for the map to have happened,
       which is what makes the grab below succeed the first time rather than
       relying on its retries. */
    if (!grab_input(ui)) {
        return -1;
    }
    char keymap[RUNNER_KEYMAP_SIZE] = {0};
    if (XQueryKeymap(ui->display, keymap)) {
        runner_key_state_capture(&ui->keys_held_at_open, keymap);
    }
    runner_draw(ui);
    XSync(ui->display, False);
    ui->running = 1;
    return 0;
}

int runner_ui_run(RunnerUi *ui) {
    if (!ui->display || ui->window == None) {
        return -1;
    }

    while (ui->running) {
        XEvent event;
        XNextEvent(ui->display, &event);

        switch (event.type) {
        case KeyPress:
            if (runner_key_state_ignores_press(
                    &ui->keys_held_at_open, event.xkey.keycode)) {
                break;
            }
            handle_key(ui, &event.xkey);
            break;

        case KeyRelease:
            runner_key_state_release(
                &ui->keys_held_at_open, event.xkey.keycode,
                key_release_is_repeat(ui->display, &event.xkey));
            break;

        case ButtonPress:
            /* A click on a row chooses it and runs it; a click outside the
               window dismisses the launcher, which is what makes a click on
               the desktop mean "I did not mean it".

               A click INSIDE the window that is not on a row does nothing at
               all, and the query line is the case that matters: it is the line
               the typing goes to, and a person who clicks it is aiming at the
               launcher, not asking it to go away. Closing on that click was
               the fault — the one place in the window that most looks like it
               should be clicked was the one place that dismissed it.

               Only the left button is read. A right or middle click is not a
               choice and must not be read as one. */
            if (event.xbutton.button != Button1) {
                break;
            }
            if (event.xbutton.x >= 0 && event.xbutton.x < ui->width &&
                event.xbutton.y >= 0 && event.xbutton.y < ui->height) {
                if (event.xbutton.y >= ui->style.row_height) {
                    choose_clicked_row(ui, event.xbutton.y);
                }
            } else {
                ui->running = 0;
                ui->launch_selected = -1;
                ui->launch_command[0] = '\0';
            }
            break;

        case Expose:
            if (event.xexpose.window == ui->window) {
                runner_draw(ui);
            }
            break;

        case VisibilityNotify:
            /* If the launcher becomes fully hidden behind other windows, raise
               it again. Override-redirect windows are not restacked by the
               window manager, so a newly mapped client can otherwise leave the
               interface effectively gone while the launcher is still running. */
            if (event.xvisibility.window == ui->window &&
                event.xvisibility.state == VisibilityFullyObscured) {
                XRaiseWindow(ui->display, ui->window);
            }
            break;

        case UnmapNotify:
            /* Keep the launcher visible for as long as it is active. */
            if (event.xunmap.window == ui->window) {
                XMapRaised(ui->display, ui->window);
            }
            break;

        case ConfigureNotify:
        case MapNotify:
            /* A window was moved, resized or shown — a program that just
               started, most of the time — and if it was raised over the
               launcher, the launcher is now behind it. The launcher is put
               back on top. Also react to events for our own window so it stays
               mapped and raised if something disturbs its stacking. */
            if (event.xany.window == ui->window) {
                XRaiseWindow(ui->display, ui->window);
            } else {
                XRaiseWindow(ui->display, ui->window);
            }
            break;

        case DestroyNotify:
            if (event.xdestroywindow.window == ui->window) {
                ui->running = 0;
                ui->launch_selected = -1;
                ui->launch_command[0] = '\0';
            }
            break;

        default:
            break;
        }
    }

    ungrab_input(ui);
    XFlush(ui->display);

    /* What was chosen, kept across the close. The caller runs it — see
       runner_ui.h — because starting a program is not the window's job and a
       window that starts programs cannot be tried without starting them. */
    if (ui->launch_selected >= 0 || ui->launch_command[0]) {
        return 0;
    }
    return -1;
}

void runner_ui_close(RunnerUi *ui) {
    if (ui->display) {
        ungrab_input(ui);
        if (ui->buffer != None) {
            XFreePixmap(ui->display, ui->buffer);
            ui->buffer = None;
        }
        if (ui->gc) {
            XFreeGC(ui->display, ui->gc);
            ui->gc = NULL;
        }
        if (ui->window != None) {
            XDestroyWindow(ui->display, ui->window);
            ui->window = None;
        }
        runner_style_free(&ui->style, ui->display);
        if (runner_x_error_handler_installed) {
            XSync(ui->display, False);
            XSetErrorHandler(runner_previous_x_error_handler);
            runner_previous_x_error_handler = NULL;
            runner_x_error_handler_installed = 0;
            runner_x_error_seen = 0;
        }
        XCloseDisplay(ui->display);
        ui->display = NULL;
    }

    ui->buffer_width = 0;
    ui->buffer_height = 0;

    runner_apps_free(&ui->programs);
    runner_match_free(&ui->matches);
}
