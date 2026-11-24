/*
 * runner_draw.c — the whole window, painted in one pass.
 *
 * Layout:
 *   - top input bar with prompt, query/placeholder, and caret
 *   - separator line
 *   - scrollable result rows with a clear selected state
 *
 * The launcher redraws the full window on interaction. That keeps the rendering
 * logic simple and avoids stale pixels when the result list changes size.
 */
#include <stdio.h>
#include <string.h>

#include "runner_draw.h"

/* Space between the input bar and the first row. */
#define DRAW_GAP 6

static int total_rows(const RunnerUi *ui) {
    return ui->matches.count + (ui->command_row >= 0 ? 1 : 0);
}

static int prompt_width(const RunnerUi *ui) {
    return runner_style_text_width(ui->display, ui->style.font,
                                   ui->config.prompt) +
           ui->style.padding / 2;
}

static Drawable draw_target(const RunnerUi *ui) {
    return ui->buffer != None ? (Drawable)ui->buffer : (Drawable)ui->window;
}

static void ensure_buffer(RunnerUi *ui) {
    if (!ui->display || ui->window == None) {
        return;
    }
    if (ui->width <= 0 || ui->height <= 0) {
        return;
    }
    if (ui->buffer != None && ui->buffer_width == ui->width &&
        ui->buffer_height == ui->height) {
        return;
    }
    if (ui->buffer != None) {
        XFreePixmap(ui->display, ui->buffer);
        ui->buffer = None;
    }
    ui->buffer = XCreatePixmap(ui->display, ui->window,
                               (unsigned int)ui->width,
                               (unsigned int)ui->height,
                               (unsigned int)DefaultDepth(ui->display, ui->screen));
    ui->buffer_width = ui->buffer ? ui->width : 0;
    ui->buffer_height = ui->buffer ? ui->height : 0;
}

static void fit_text(const RunnerUi *ui, const char *text, int max_width,
                     char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || !text[0] || max_width <= 0) {
        return;
    }

    if (runner_style_text_width(ui->display, ui->style.font, text) <= max_width) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    const char *ellipsis = "...";
    int ellipsis_w = runner_style_text_width(ui->display, ui->style.font, ellipsis);
    if (ellipsis_w > max_width) {
        ellipsis = "";
        ellipsis_w = 0;
    }

    /* Walk UTF-8 code points by bytes conservatively; clipping by bytes is fine
       here because we re-measure width each step. */
    size_t len = strlen(text);
    size_t cut = len;
    while (cut > 0) {
        cut--;
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
            cut--;
        }
        char tmp[RUNNER_MAX_QUERY * 2];
        if (cut + strlen(ellipsis) + 1 > sizeof(tmp)) {
            continue;
        }
        memcpy(tmp, text, cut);
        tmp[cut] = '\0';
        strncat(tmp, ellipsis, sizeof(tmp) - strlen(tmp) - 1);
        if (runner_style_text_width(ui->display, ui->style.font, tmp) <= max_width) {
            snprintf(out, out_size, "%s", tmp);
            return;
        }
    }

    snprintf(out, out_size, "%s", ellipsis);
}

static void draw_row_text(const RunnerUi *ui, Drawable target, int y, int left,
                          const char *name, const char *comment, int chosen) {
    int baseline = y + (ui->style.row_height +
                        (ui->style.font ? ui->style.font->ascent -
                                          ui->style.font->descent : 0)) / 2;

    (void)chosen;

    int right_limit = ui->width - ui->style.padding - 14; /* keep clear of scrollbar */
    if (right_limit <= left) {
        return;
    }

    char name_buf[RUNNER_MAX_QUERY * 2];
    char comment_buf[RUNNER_MAX_QUERY * 2];
    name_buf[0] = '\0';
    comment_buf[0] = '\0';

    if (comment && comment[0]) {
        /* Prefer showing the full name; only fall back to truncation if it still
           cannot fit. This avoids hiding important app names behind ellipsis. */
        int total_room = right_limit - left;
        int full_name_w = runner_style_text_width(ui->display, ui->style.font, name);
        int max_name = total_room;
        if (full_name_w < total_room - 60) {
            max_name = full_name_w;
        } else if (max_name > total_room - 30) {
            max_name = total_room - 30;
        }
        if (max_name < 40) max_name = total_room;

        fit_text(ui, name, max_name, name_buf, sizeof(name_buf));
        int name_w = runner_style_text_width(ui->display, ui->style.font, name_buf);
        runner_style_text(ui->display, ui->screen, target, ui->style.font,
                          left, baseline, name_buf, ui->style.text);

        int comment_x = left + name_w + ui->style.padding;
        int room = right_limit - comment_x;
        if (room > 30) {
            fit_text(ui, comment, room, comment_buf, sizeof(comment_buf));
            if (comment_buf[0]) {
                runner_style_text(ui->display, ui->screen, target,
                                  ui->style.font, comment_x, baseline,
                                  comment_buf, ui->style.text_muted);
            }
        }
    } else {
        fit_text(ui, name, right_limit - left, name_buf, sizeof(name_buf));
        runner_style_text(ui->display, ui->screen, target, ui->style.font,
                          left, baseline, name_buf, ui->style.text);
    }
}

static void draw_query(const RunnerUi *ui, Drawable target) {
    int baseline = (ui->style.row_height +
                    (ui->style.font ? ui->style.font->ascent -
                                      ui->style.font->descent : 0)) / 2;

    int prompt_x = ui->style.padding;
    int text_x = prompt_x + prompt_width(ui) + 4;
    int max_text_w = ui->width - text_x - ui->style.padding - 4;
    if (max_text_w < 20) max_text_w = 20;

    runner_style_text(ui->display, ui->screen, target, ui->style.font,
                      prompt_x, baseline, ui->config.prompt,
                      ui->style.accent);

    if (ui->query_length == 0) {
        char ph[RUNNER_MAX_QUERY * 2];
        fit_text(ui, ui->config.placeholder, max_text_w, ph, sizeof(ph));
        runner_style_text(ui->display, ui->screen, target, ui->style.font,
                          text_x, baseline, ph, ui->style.text_muted);
        return;
    }

    /* Show the tail of long queries so the caret stays visible. */
    const char *q = ui->query;
    char shown[RUNNER_MAX_QUERY + 4];
    shown[0] = '\0';
    size_t qlen = strlen(q);
    size_t start = 0;
    while (start < qlen) {
        snprintf(shown, sizeof(shown), "%s%s", start == 0 ? "" : "…", q + start);
        if (runner_style_text_width(ui->display, ui->style.font, shown) <= max_text_w - 4) {
            break;
        }
        start++;
        while (start < qlen && ((unsigned char)q[start] & 0xC0) == 0x80) {
            start++;
        }
    }
    if (shown[0] == '\0') {
        snprintf(shown, sizeof(shown), "%s", q);
    }

    runner_style_text(ui->display, ui->screen, target, ui->style.font,
                      text_x, baseline, shown, ui->style.text);

    int caret_x = text_x + runner_style_text_width(ui->display, ui->style.font,
                                                   shown) + 1;
    if (caret_x > ui->width - ui->style.padding - 2) {
        caret_x = ui->width - ui->style.padding - 2;
    }
    int caret_h = ui->style.font ? (ui->style.font->ascent + ui->style.font->descent) : 14;
    int caret_y = (ui->style.row_height - caret_h) / 2;
    if (caret_h < 14) caret_h = 14;
    XSetForeground(ui->display, ui->gc, ui->style.accent);
    XFillRectangle(ui->display, target, ui->gc, caret_x, caret_y, 2,
                   (unsigned int)caret_h);
}

static void draw_scrollbar(const RunnerUi *ui, Drawable target, int top,
                           int visible, int rows) {
    if (rows <= visible || visible <= 0) {
        return;
    }

    int track_x = ui->width - 10;
    int track_y = top + 4;
    int track_h = visible * ui->style.row_height - 8;
    if (track_h < 20) {
        return;
    }

    /* Track */
    XSetForeground(ui->display, ui->gc, ui->style.panel_edge);
    XFillRectangle(ui->display, target, ui->gc, track_x, track_y, 4,
                   (unsigned int)track_h);

    int thumb_h = (track_h * visible) / rows;
    if (thumb_h < 18) thumb_h = 18;
    if (thumb_h > track_h) thumb_h = track_h;

    int max_scroll = rows - visible;
    int thumb_y = track_y;
    if (max_scroll > 0) {
        thumb_y += (ui->scroll * (track_h - thumb_h)) / max_scroll;
    }

    /* Thumb */
    XSetForeground(ui->display, ui->gc, ui->style.accent);
    XFillRectangle(ui->display, target, ui->gc, track_x, thumb_y, 4,
                   (unsigned int)thumb_h);
}

static void draw_rows(const RunnerUi *ui, Drawable target) {
    int top = ui->style.row_height + DRAW_GAP;
    int visible = (ui->height - top - 2) / ui->style.row_height;
    if (visible < 1) {
        visible = 1;
    }

    int rows = total_rows(ui);
    if (rows == 0) {
        int baseline = top + (ui->style.row_height +
                              (ui->style.font ? ui->style.font->ascent -
                                                ui->style.font->descent : 0)) / 2;
        runner_style_text(ui->display, ui->screen, target, ui->style.font,
                          ui->style.padding, baseline, ui->config.empty_message,
                          ui->style.text_muted);
        return;
    }

    for (int row = 0; row < visible; row++) {
        int index = ui->scroll + row;
        if (index >= rows) {
            break;
        }
        int y = top + row * ui->style.row_height;
        int chosen = (index == ui->selected);
        int content_left = ui->style.padding;

        if (chosen) {
            XSetForeground(ui->display, ui->gc, ui->style.field);
            XFillRectangle(ui->display, target, ui->gc, 6, y + 2,
                           (unsigned int)(ui->width - 20),
                           (unsigned int)(ui->style.row_height - 4));
            XSetForeground(ui->display, ui->gc, ui->style.accent);
            XFillRectangle(ui->display, target, ui->gc, 6, y + 5, 3,
                           (unsigned int)(ui->style.row_height - 10));
            content_left = ui->style.padding + 10;
        }

        if (index == ui->command_row) {
            char line[RUNNER_MAX_QUERY + 32];
            if (ui->query[0]) {
                snprintf(line, sizeof(line), "Run command: %s", ui->query);
            } else {
                snprintf(line, sizeof(line), "Run command");
            }
            draw_row_text(ui, target, y, content_left, line,
                          "Execute in shell", chosen);
        } else {
            const RunnerProgram *program =
                &ui->programs.items[ui->matches.items[index].program_index];
            draw_row_text(ui, target, y, content_left, program->name,
                          program->comment, chosen);
        }
    }

    draw_scrollbar(ui, target, top, visible, rows);
}

void runner_draw(RunnerUi *ui) {
    if (!ui->display || ui->window == None || !ui->gc) {
        return;
    }

    ensure_buffer(ui);
    Drawable target = draw_target(ui);

    XSetForeground(ui->display, ui->gc, ui->style.background);
    XFillRectangle(ui->display, target, ui->gc, 0, 0,
                   (unsigned int)ui->width, (unsigned int)ui->height);

    /* Input bar */
    XSetForeground(ui->display, ui->gc, ui->style.panel);
    XFillRectangle(ui->display, target, ui->gc, 0, 0,
                   (unsigned int)ui->width,
                   (unsigned int)ui->style.row_height);

    /* Subtle top accent strip */
    XSetForeground(ui->display, ui->gc, ui->style.accent);
    XFillRectangle(ui->display, target, ui->gc, 0, 0,
                   (unsigned int)ui->width, 2);

    /* Separator */
    XSetForeground(ui->display, ui->gc, ui->style.panel_edge);
    XDrawLine(ui->display, target, ui->gc, 0, ui->style.row_height,
              ui->width, ui->style.row_height);

    draw_query(ui, target);
    draw_rows(ui, target);

    /* Border */
    XSetForeground(ui->display, ui->gc, ui->style.panel_edge);
    XDrawRectangle(ui->display, target, ui->gc, 0, 0,
                   (unsigned int)(ui->width - 1),
                   (unsigned int)(ui->height - 1));

    if (target != (Drawable)ui->window) {
        XCopyArea(ui->display, ui->buffer, ui->window, ui->gc, 0, 0,
                  (unsigned int)ui->width, (unsigned int)ui->height, 0, 0);
    }

    XFlush(ui->display);
}
