/* $amux$ */

/*
 * Copyright (c) 2026 Simon Festl <simon@sfx-ecommerce.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <limits.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Sidebar: a vertical panel beside the panes. It shows the window list with
 * per-window state, the state of "agent" panes (anything that is not the
 * shell), an alert history and the paste buffers.
 *
 * The sidebar takes sidebar-width columns away from the window area, in the
 * same way the status line takes lines away. Everything that maps window
 * coordinates to terminal coordinates adds sidebar_x_offset() when the
 * sidebar is on the left.
 */

#define SIDEBAR_MINI_WIDTH 6
#define SIDEBAR_MIN_PANES 40
#define SIDEBAR_SCAN_LINES 8
#define SIDEBAR_KEYS_ROWS 4
#define SIDEBAR_AUTO_PERCENT 22
#define SIDEBAR_AUTO_MIN 30
#define SIDEBAR_AUTO_MAX 60
#define SIDEBAR_BUFFERS_MAX 3

/* Alert history, newest first. */
static struct alert_entries	alert_list = TAILQ_HEAD_INITIALIZER(alert_list);
static u_int			alert_total;
static u_int			alert_next_id;

/* Cached regular expression. */
struct sidebar_regex {
	char	*pattern;
	regex_t	 re;
	int	 ok;
};
static struct sidebar_regex	sidebar_wait_re;
static struct sidebar_regex	sidebar_busy_re;

/* Glyph set. */
struct sidebar_glyphs {
	const char	*current;
	const char	*on;
	const char	*alert;
	const char	*done;
	const char	*idle;
	const char	*ok;
	const char	*warn;
	const char	*error;
	const char	*info;
	const char	*bell;
	const char	*silence;
	const char	*title;
	const char	*hline;
	const char	*vline;
	const char	*dot;
	const char	*spinner[10];
};
static const struct sidebar_glyphs sidebar_glyphs_utf8 = {
	"\xe2\x96\xb6",	/* U+25B6 black right-pointing triangle */
	"\xe2\x97\x89",	/* U+25C9 fisheye */
	"\xe2\x97\x89",	/* U+25C9 fisheye */
	"\xe2\x97\x8e",	/* U+25CE bullseye */
	"\xe2\x97\x8b",	/* U+25CB white circle */
	"\xe2\x9c\x93",	/* U+2713 check mark */
	"!",
	"\xe2\x9c\x97",	/* U+2717 ballot x */
	"\xe2\x97\x86",	/* U+25C6 black diamond */
	"\xe2\x97\x8f",	/* U+25CF black circle */
	"\xe2\x97\x8c",	/* U+25CC dotted circle */
	"\xe2\x96\x8c",	/* U+258C left half block */
	"\xe2\x94\x80",	/* U+2500 box drawings light horizontal */
	"\xe2\x94\x82",	/* U+2502 box drawings light vertical */
	"\xc2\xb7",	/* U+00B7 middle dot */
	{ "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
	  "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
	  "\xe2\xa0\x87", "\xe2\xa0\x8f" }
};
static const struct sidebar_glyphs sidebar_glyphs_ascii = {
	">", "*", "*", "o", ".", "+", "!", "x", "#", "@", "-", "|", "-", "|",
	".", { "|", "/", "-", "\\", "|", "/", "-", "\\", "|", "/" }
};

/* Drawing context. */
struct sidebar_draw {
	struct client			*c;
	struct session			*s;
	struct screen_write_ctx		 ctx;
	const struct sidebar_glyphs	*g;

	u_int				 w;	/* total width */
	u_int				 h;	/* total height */
	u_int				 x0;	/* first content column */
	u_int				 iw;	/* content width */
	u_int				 y;	/* next free row */
	u_int				 bottom;/* rows below this are reserved */

	struct grid_cell		 base;
	struct grid_cell		 dim;
	struct grid_cell		 a1;
	struct grid_cell		 a2;
	struct grid_cell		 ok;
	struct grid_cell		 warn;
	struct grid_cell		 err;

	time_t				 now;
	u_int				 nwindows;
	u_int				 npanes;
	u_int				 nagents;
};

/* Window summary state. */
enum sidebar_window_state {
	SIDEBAR_WINDOW_IDLE,
	SIDEBAR_WINDOW_RUNNING,
	SIDEBAR_WINDOW_BUSY,
	SIDEBAR_WINDOW_DONE,
	SIDEBAR_WINDOW_ACTIVITY,
	SIDEBAR_WINDOW_SILENCE,
	SIDEBAR_WINDOW_BELL,
	SIDEBAR_WINDOW_WAITING
};

static void	sidebar_timer_callback(int, short, void *);
static void	sidebar_scan(struct sidebar_draw *);

/* Section names, in the order they are drawn by default. */
static const char *sidebar_section_names[] = {
	"windows", "agents", "alerts", "custom", "buffers", "keys", NULL
};
enum sidebar_section {
	SIDEBAR_SECTION_WINDOWS,
	SIDEBAR_SECTION_AGENTS,
	SIDEBAR_SECTION_ALERTS,
	SIDEBAR_SECTION_CUSTOM,
	SIDEBAR_SECTION_BUFFERS,
	SIDEBAR_SECTION_KEYS,
	SIDEBAR_SECTION_NONE
};

/* Update per-session cached sidebar values from options. */
void
sidebar_update_cache(struct session *s)
{
	if (!options_get_number(s->options, "sidebar"))
		s->sidebarat = -1;
	else if (options_get_number(s->options, "sidebar-position") == 0)
		s->sidebarat = 0;
	else
		s->sidebarat = 1;

	if (options_get_number(s->options, "sidebar-mode") == 1)
		s->sidebarwidth = SIDEBAR_MINI_WIDTH;
	else
		s->sidebarwidth = options_get_number(s->options, "sidebar-width");
}

/* Get sidebar width for a client. 0 means off. */
u_int
sidebar_size(struct client *c)
{
	struct session	*s = c->session;
	u_int		 w;

	if (s == NULL || (c->flags & CLIENT_CONTROL))
		return (0);
	if (s->sidebarat == -1)
		return (0);
	w = s->sidebarwidth;
	if (w == 0) {
		/* Automatic: a share of the terminal, within sane bounds. */
		w = c->tty.sx * SIDEBAR_AUTO_PERCENT / 100;
		if (w < SIDEBAR_AUTO_MIN)
			w = SIDEBAR_AUTO_MIN;
		if (w > SIDEBAR_AUTO_MAX)
			w = SIDEBAR_AUTO_MAX;
	}
	if (c->tty.sx < w + SIDEBAR_MIN_PANES)
		return (0);
	return (w);
}

/* Is the sidebar on the left? */
int
sidebar_at_left(struct client *c)
{
	if (sidebar_size(c) == 0)
		return (0);
	return (c->session->sidebarat == 0);
}

/* Columns the window area is shifted right by. */
u_int
sidebar_x_offset(struct client *c)
{
	if (sidebar_at_left(c))
		return (sidebar_size(c));
	return (0);
}

/* First terminal column of the sidebar. */
u_int
sidebar_x(struct client *c)
{
	u_int	w = sidebar_size(c);

	if (w == 0)
		return (0);
	if (c->session->sidebarat == 0)
		return (0);
	return (c->tty.sx - w);
}

/* First terminal line of the sidebar. */
u_int
sidebar_y(struct client *c)
{
	if (c->session != NULL && status_at_line(c) == 0)
		return (status_line_size(c));
	return (0);
}

/* Height of the sidebar. */
u_int
sidebar_height(struct client *c)
{
	u_int	lines;

	if (c->session == NULL)
		return (0);
	lines = status_line_size(c);
	if (c->tty.sy <= lines)
		return (0);
	return (c->tty.sy - lines);
}

/* Initialize sidebar for a client. */
void
sidebar_init(struct client *c)
{
	struct sidebar	*sb = &c->sidebar;

	screen_init(&sb->screen, 1, 1, 0);
	screen_init(&sb->old, 1, 1, 0);
	sb->rows = NULL;
	sb->nrows = 0;
	sb->force = 1;
	sb->sel = -1;
}

/* Free sidebar for a client. */
void
sidebar_free(struct client *c)
{
	struct sidebar	*sb = &c->sidebar;

	if (event_initialized(&sb->timer))
		evtimer_del(&sb->timer);
	screen_free(&sb->screen);
	screen_free(&sb->old);
	free(sb->rows);
	sb->rows = NULL;
	sb->nrows = 0;
}

/* Flag the sidebar for redraw on every client. */
void
sidebar_redraw_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL)
			c->flags |= (CLIENT_REDRAWSIDEBAR|CLIENT_REDRAWSTATUS);
	}
}

/* Sidebar timer callback. */
static void
sidebar_timer_callback(__unused int fd, __unused short events, void *arg)
{
	struct client	*c = arg;
	struct session	*s = c->session;
	struct timeval	 tv;

	evtimer_del(&c->sidebar.timer);
	if (s == NULL)
		return;

	if (sidebar_size(c) != 0)
		c->flags |= CLIENT_REDRAWSIDEBAR;

	timerclear(&tv);
	tv.tv_sec = options_get_number(s->options, "sidebar-interval");
	if (tv.tv_sec != 0)
		evtimer_add(&c->sidebar.timer, &tv);
}

/* Start sidebar timer for a client. */
void
sidebar_timer_start(struct client *c)
{
	struct session	*s = c->session;

	if (event_initialized(&c->sidebar.timer))
		evtimer_del(&c->sidebar.timer);
	else
		evtimer_set(&c->sidebar.timer, sidebar_timer_callback, c);

	if (s != NULL && options_get_number(s->options, "sidebar"))
		sidebar_timer_callback(-1, 0, c);
}

/* Start sidebar timer for all clients. */
void
sidebar_timer_start_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry)
		sidebar_timer_start(c);
}

/* Name of a pane state. */
const char *
sidebar_state_name(int state)
{
	switch (state) {
	case SIDEBAR_STATE_RUNNING:
		return ("running");
	case SIDEBAR_STATE_BUSY:
		return ("busy");
	case SIDEBAR_STATE_WAITING:
		return ("waiting");
	case SIDEBAR_STATE_DONE:
		return ("done");
	}
	return ("shell");
}

/* Parse a pane state name. Returns -1 for auto, -2 if unknown. */
int
sidebar_state_from_name(const char *name)
{
	if (strcmp(name, "auto") == 0)
		return (-1);
	if (strcmp(name, "shell") == 0 || strcmp(name, "idle") == 0)
		return (SIDEBAR_STATE_SHELL);
	if (strcmp(name, "running") == 0)
		return (SIDEBAR_STATE_RUNNING);
	if (strcmp(name, "busy") == 0 || strcmp(name, "working") == 0)
		return (SIDEBAR_STATE_BUSY);
	if (strcmp(name, "waiting") == 0 || strcmp(name, "input") == 0)
		return (SIDEBAR_STATE_WAITING);
	if (strcmp(name, "done") == 0)
		return (SIDEBAR_STATE_DONE);
	return (-2);
}

/* Match text against a cached regular expression option. */
static int
sidebar_regex_match(struct sidebar_regex *sr, const char *pattern,
    const char *text)
{
	if (pattern == NULL || *pattern == '\0')
		return (0);
	if (sr->pattern == NULL || strcmp(sr->pattern, pattern) != 0) {
		if (sr->pattern != NULL) {
			free(sr->pattern);
			if (sr->ok)
				regfree(&sr->re);
		}
		sr->pattern = xstrdup(pattern);
		sr->ok = (regcomp(&sr->re, pattern,
		    REG_EXTENDED|REG_NOSUB|REG_NEWLINE) == 0);
		if (!sr->ok)
			log_debug("%s: bad pattern: %s", __func__, pattern);
	}
	if (!sr->ok)
		return (0);
	return (regexec(&sr->re, text, 0, NULL, 0) == 0);
}

/* Get the foreground command name for a pane. */
static char *
sidebar_pane_command(struct window_pane *wp)
{
	char	*cmd, *name;

	if (wp->fd == -1 || wp->shell == NULL)
		return (xstrdup(""));

	cmd = osdep_get_name(wp->fd, wp->tty);
	if (cmd == NULL || *cmd == '\0') {
		free(cmd);
		cmd = cmd_stringify_argv(wp->argc, wp->argv);
		if (cmd == NULL || *cmd == '\0') {
			free(cmd);
			cmd = xstrdup(wp->shell);
		}
	}
	name = parse_window_name(cmd);
	free(cmd);
	return (name);
}

/* Get the last few non-empty visible lines of a pane, newest first. */
static char *
sidebar_pane_tail(struct window_pane *wp)
{
	struct grid	*gd = wp->base.grid;
	u_int		 y, len, n = 0;
	char		*line, *out = NULL;
	size_t		 size = 0, linelen;

	for (y = gd->hsize + gd->sy; y > gd->hsize; y--) {
		if (n >= SIDEBAR_SCAN_LINES)
			break;
		len = grid_line_length(gd, y - 1);
		if (len == 0)
			continue;
		line = grid_string_cells(gd, 0, y - 1, len, NULL,
		    GRID_STRING_TRIM_SPACES, &wp->base);
		linelen = strlen(line);
		out = xrealloc(out, size + linelen + 2);
		memcpy(out + size, line, linelen);
		size += linelen;
		out[size++] = '\n';
		out[size] = '\0';
		free(line);
		n++;
	}
	if (out == NULL)
		out = xstrdup("");
	return (out);
}

/* Work out (and cache) the state of a pane. */
int
sidebar_pane_state(struct window_pane *wp)
{
	const char	*shell, *pattern;
	char		*cmd, *tail;
	int		 state;

	cmd = sidebar_pane_command(wp);
	if (wp->sb_cmd == NULL || strcmp(cmd, wp->sb_cmd) != 0) {
		free(wp->sb_cmd);
		wp->sb_cmd = cmd;
		wp->sb_override = -1;
		free(wp->sb_activity);
		wp->sb_activity = NULL;
		free(wp->sb_task);
		wp->sb_task = NULL;
	} else
		free(cmd);

	shell = NULL;
	if (wp->shell != NULL) {
		shell = strrchr(wp->shell, '/');
		if (shell != NULL)
			shell++;
		else
			shell = wp->shell;
	}

	if (wp->fd == -1 || *wp->sb_cmd == '\0' ||
	    (shell != NULL && strcmp(wp->sb_cmd, shell) == 0))
		state = SIDEBAR_STATE_SHELL;
	else if (wp->sb_override != -1)
		state = wp->sb_override;
	else {
		tail = sidebar_pane_tail(wp);
		pattern = options_get_string(global_s_options,
		    "sidebar-busy-pattern");
		if (sidebar_regex_match(&sidebar_busy_re, pattern, tail))
			state = SIDEBAR_STATE_BUSY;
		else {
			pattern = options_get_string(global_s_options,
			    "sidebar-wait-pattern");
			if (sidebar_regex_match(&sidebar_wait_re, pattern,
			    tail))
				state = SIDEBAR_STATE_WAITING;
			else
				state = SIDEBAR_STATE_RUNNING;
		}
		free(tail);
	}

	if (state == SIDEBAR_STATE_SHELL) {
		free(wp->sb_activity);
		wp->sb_activity = NULL;
		free(wp->sb_task);
		wp->sb_task = NULL;
	}
	if (state != wp->sb_state) {
		if (state == SIDEBAR_STATE_WAITING &&
		    wp->sb_override == -1 &&
		    wp->sb_state != SIDEBAR_STATE_SHELL) {
			alert_push(ALERT_WARN, wp->window, wp, wp->sb_cmd,
			    "waiting for input");
		}
		wp->sb_state = state;
		wp->sb_since = time(NULL);
	}
	return (state);
}

/* Explicitly set the state of a pane (from the notify command). */
void
sidebar_pane_set_state(struct window_pane *wp, int state)
{
	if (wp->sb_cmd == NULL)
		wp->sb_cmd = sidebar_pane_command(wp);
	wp->sb_override = state;
	if (state == -1)
		return;
	if (state != wp->sb_state) {
		wp->sb_state = state;
		wp->sb_since = time(NULL);
	}
	sidebar_redraw_all();
}

/* Set (or clear, with NULL or an empty string) the activity text of a pane. */
void
sidebar_pane_set_activity(struct window_pane *wp, const char *text)
{
	free(wp->sb_activity);
	if (text == NULL || *text == '\0')
		wp->sb_activity = NULL;
	else
		wp->sb_activity = xstrdup(text);
	sidebar_redraw_all();
}

/* Set (or clear) what a pane is working on. */
void
sidebar_pane_set_task(struct window_pane *wp, const char *text)
{
	free(wp->sb_task);
	if (text == NULL || *text == '\0')
		wp->sb_task = NULL;
	else
		wp->sb_task = xstrdup(text);
	sidebar_redraw_all();
}

/*
 * What a pane is working on: the text set with notify -T, or the pane title
 * (Claude Code sets it to a summary of the conversation), unless that is
 * just the host name.
 */
const char *
sidebar_pane_task(struct window_pane *wp)
{
	static char	 host[HOST_NAME_MAX + 1];
	static int	 host_set;
	const char	*title;

	if (wp->sb_state == SIDEBAR_STATE_SHELL)
		return (NULL);
	if (wp->sb_task != NULL)
		return (wp->sb_task);
	if (!host_set) {
		if (gethostname(host, sizeof host) != 0)
			*host = '\0';
		host_set = 1;
	}
	title = wp->base.title;
	if (title == NULL || *title == '\0' || strcmp(title, host) == 0)
		return (NULL);
	/* Skip the sparkle Claude Code puts in front of its titles. */
	if (strncmp(title, "\xe2\x9c\xb3 ", 4) == 0 ||
	    strncmp(title, "\xe2\x9c\xbb ", 4) == 0)
		title += 4;
	if (*title == '\0')
		return (NULL);
	return (title);
}

/* Add an alert to the history. */
struct alert_entry *
alert_push(enum alert_type type, struct window *w, struct window_pane *wp,
    const char *source, const char *msg)
{
	struct alert_entry	*ae, *old;
	u_int			 limit;
	int			 wid = -1, wpid = -1;

	limit = options_get_number(global_s_options, "alert-history");
	if (limit == 0)
		return (NULL);
	if (w != NULL)
		wid = w->id;
	if (wp != NULL)
		wpid = wp->id;
	if (source == NULL)
		source = "";
	if (msg == NULL)
		msg = "";

	/* Collapse repeats of an unread alert. */
	ae = TAILQ_FIRST(&alert_list);
	if (ae != NULL &&
	    !ae->read &&
	    ae->type == type &&
	    ae->w == wid &&
	    ae->wp == wpid &&
	    strcmp(ae->source, source) == 0 &&
	    strcmp(ae->msg, msg) == 0) {
		ae->t = time(NULL);
		ae->count++;
		sidebar_redraw_all();
		return (ae);
	}

	ae = xcalloc(1, sizeof *ae);
	ae->id = ++alert_next_id;
	ae->t = time(NULL);
	ae->type = type;
	ae->w = wid;
	ae->wp = wpid;
	ae->source = xstrdup(source);
	ae->msg = xstrdup(msg);
	ae->count = 1;
	TAILQ_INSERT_HEAD(&alert_list, ae, entry);
	alert_total++;

	while (alert_total > limit) {
		old = TAILQ_LAST(&alert_list, alert_entries);
		TAILQ_REMOVE(&alert_list, old, entry);
		free(old->source);
		free(old->msg);
		free(old);
		alert_total--;
	}

	log_debug("%s: alert #%u (%d) @%d %%%d %s: %s", __func__, ae->id,
	    type, wid, wpid, source, msg);
	sidebar_redraw_all();
	return (ae);
}

/* Number of unread alerts. */
u_int
alert_unread_count(void)
{
	struct alert_entry	*ae;
	u_int			 n = 0;

	TAILQ_FOREACH(ae, &alert_list, entry) {
		if (!ae->read)
			n++;
	}
	return (n);
}

/* Oldest unread alert. */
struct alert_entry *
alert_next_unread(void)
{
	struct alert_entry	*ae;

	TAILQ_FOREACH_REVERSE(ae, &alert_list, alert_entries, entry) {
		if (!ae->read)
			return (ae);
	}
	return (NULL);
}

/* Find alert by id. */
struct alert_entry *
alert_find_by_id(u_int id)
{
	struct alert_entry	*ae;

	TAILQ_FOREACH(ae, &alert_list, entry) {
		if (ae->id == id)
			return (ae);
	}
	return (NULL);
}

/* Mark all alerts read. */
void
alert_mark_all_read(void)
{
	struct alert_entry	*ae;

	TAILQ_FOREACH(ae, &alert_list, entry)
		ae->read = 1;
	sidebar_redraw_all();
}

/* Remove all alerts. */
void
alert_clear_all(void)
{
	struct alert_entry	*ae, *ae1;

	TAILQ_FOREACH_SAFE(ae, &alert_list, entry, ae1) {
		TAILQ_REMOVE(&alert_list, ae, entry);
		free(ae->source);
		free(ae->msg);
		free(ae);
	}
	alert_total = 0;
	sidebar_redraw_all();
}

/* Select a window (and optionally pane) in the client's session. */
static int
sidebar_select(struct client *c, struct window *w, struct window_pane *wp)
{
	struct session	*s = c->session;
	struct winlink	*wl;

	if (s == NULL || w == NULL)
		return (-1);
	wl = winlink_find_by_window(&s->windows, w);
	if (wl == NULL)
		return (-1);
	if (wl != s->curw) {
		if (session_select(s, wl->idx) != 0)
			return (-1);
	}
	if (wp != NULL && wp->window == w && w->active != wp)
		window_set_active_pane(w, wp, 1);
	server_redraw_session(s);
	return (0);
}

/* Jump to the window/pane of an alert and mark it read. */
int
alert_jump(struct client *c, struct alert_entry *ae)
{
	struct window		*w = NULL;
	struct window_pane	*wp = NULL;

	ae->read = 1;
	sidebar_redraw_all();

	if (ae->w != -1)
		w = window_find_by_id(ae->w);
	if (ae->wp != -1)
		wp = window_pane_find_by_id(ae->wp);
	if (w == NULL && wp != NULL)
		w = wp->window;
	if (w == NULL)
		return (0);
	return (sidebar_select(c, w, wp));
}

/* Agent panes of a session in sidebar order. Caller frees. */
static struct window_pane **
sidebar_agents(struct session *s, u_int *n)
{
	struct winlink		 *wl;
	struct window_pane	 *wp, **list = NULL;
	u_int			  size = 0;

	*n = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			if (wp->sb_state == SIDEBAR_STATE_SHELL)
				continue;
			if (*n == size) {
				size = (size == 0) ? 8 : size * 2;
				list = xreallocarray(list, size, sizeof *list);
			}
			list[(*n)++] = wp;
		}
	}
	return (list);
}

/* The agent selected in the sidebar, if it still exists. */
struct window_pane *
sidebar_selected(struct client *c)
{
	struct session		*s = c->session;
	struct window_pane	*wp;

	if (s == NULL || c->sidebar.sel == -1)
		return (NULL);
	wp = window_pane_find_by_id(c->sidebar.sel);
	if (wp == NULL ||
	    wp->sb_state == SIDEBAR_STATE_SHELL ||
	    !session_has(s, wp->window)) {
		c->sidebar.sel = -1;
		return (NULL);
	}
	return (wp);
}

/* Move the selection to the next (dir > 0) or previous agent. */
void
sidebar_select_move(struct client *c, int dir)
{
	struct session		 *s = c->session;
	struct window_pane	**list, *cur;
	u_int			  n, i;

	if (s == NULL)
		return;
	list = sidebar_agents(s, &n);
	if (n == 0) {
		c->sidebar.sel = -1;
		status_message_set(c, -1, 1, 0, 0, "no agents running");
		free(list);
		return;
	}
	cur = sidebar_selected(c);
	for (i = 0; i < n; i++) {
		if (list[i] == cur)
			break;
	}
	if (i == n)
		i = (dir > 0) ? 0 : n - 1;
	else if (dir > 0)
		i = (i + 1) % n;
	else
		i = (i == 0) ? n - 1 : i - 1;
	c->sidebar.sel = list[i]->id;
	free(list);
	sidebar_redraw_all();
}

/* Select a pane in the sidebar and jump to it. */
int
sidebar_jump(struct client *c, struct window_pane *wp)
{
	c->sidebar.sel = wp->id;
	return (sidebar_select(c, wp->window, wp));
}

/* Send a line of text, followed by Enter, to a pane. */
void
sidebar_send_input(struct client *c, struct window_pane *wp, const char *text)
{
	struct session		*s = c->session;
	struct winlink		*wl;
	struct utf8_data	*ud, *loop;
	utf8_char		 uc;
	key_code		 key;

	wl = winlink_find_by_window(&s->windows, wp->window);
	ud = utf8_fromcstr(text);
	for (loop = ud; loop->size != 0; loop++) {
		if (loop->size == 1 && loop->data[0] <= 0x7f)
			key = loop->data[0];
		else {
			if (utf8_from_data(loop, &uc) != UTF8_DONE)
				continue;
			key = uc;
		}
		window_pane_key(wp, c, s, wl, key, NULL);
	}
	free(ud);
	window_pane_key(wp, c, s, wl, '\r', NULL);
}

/* Show the scrollback of a pane in a popup. */
int
sidebar_preview(struct cmdq_item *item, struct client *c,
    struct window_pane *wp)
{
	struct session	*s = c->session;
	struct environ	*env;
	char		 exe[PATH_MAX], *shellcmd, *title;
	ssize_t		 len;
	u_int		 w, h, px, py;
	int		 ret;

	len = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (len <= 0)
		strlcpy(exe, "tmux", sizeof exe);
	else
		exe[len] = '\0';

	/* Drop trailing blank lines so less +G lands on the last output. */
	xasprintf(&shellcmd, "'%s' -S '%s' capture-pane -peJ -t %%%u -S -3000 | "
	    "awk '{ l[NR] = $0 } END { n = NR; "
	    "while (n > 0 && l[n] ~ /^[[:space:]]*$/) n--; "
	    "for (i = 1; i <= n; i++) print l[i] }' | less -R +G",
	    exe, socket_path, wp->id);
	xasprintf(&title, " %s (%%%u) ", wp->sb_cmd != NULL ? wp->sb_cmd : "",
	    wp->id);

	w = c->tty.sx * 8 / 10;
	h = c->tty.sy * 8 / 10;
	if (w < 40)
		w = c->tty.sx;
	if (h < 10)
		h = c->tty.sy;
	px = (c->tty.sx - w) / 2;
	py = (c->tty.sy - h) / 2;

	env = environ_create();
	ret = popup_display(POPUP_CLOSEEXIT, BOX_LINES_DEFAULT, item, px, py, w,
	    h, env, shellcmd, 0, NULL, NULL, title, c, s, NULL, NULL, NULL,
	    NULL);
	environ_free(env);
	free(shellcmd);
	free(title);
	return (ret);
}

/* Change the sidebar width by delta columns (from the effective width). */
void
sidebar_set_width(struct client *c, int delta)
{
	struct session	*s = c->session;
	int		 w;

	if (s == NULL)
		return;
	w = (int)sidebar_size(c);
	if (w == 0)
		w = s->sidebarwidth;
	w += delta;
	if (w < 8)
		w = 8;
	if (w > 200)
		w = 200;
	options_set_number(s->options, "sidebar-width", w);
	options_push_changes("sidebar-width");
	status_message_set(c, -1, 1, 0, 0, "sidebar width %d", w);
}

/* Handle a mouse click on the sidebar. */
void
sidebar_click(struct client *c, __unused u_int x, u_int y)
{
	struct sidebar		*sb = &c->sidebar;
	struct sidebar_row	*row;
	struct session		*s = c->session;
	struct winlink		*wl;
	struct window_pane	*wp;
	struct alert_entry	*ae;

	if (s == NULL || y >= sb->nrows)
		return;
	row = &sb->rows[y];
	switch (row->type) {
	case SIDEBAR_ROW_WINDOW:
		wl = winlink_find_by_index(&s->windows, row->id);
		if (wl != NULL)
			sidebar_select(c, wl->window, NULL);
		break;
	case SIDEBAR_ROW_PANE:
		wp = window_pane_find_by_id(row->id);
		if (wp != NULL)
			sidebar_jump(c, wp);
		break;
	case SIDEBAR_ROW_ALERT:
		ae = alert_find_by_id(row->id);
		if (ae != NULL)
			alert_jump(c, ae);
		break;
	}
}

/* Set a colour on a copy of the base cell. */
static void
sidebar_colour(struct sidebar_draw *d, struct grid_cell *gc, const char *name)
{
	int	colour;

	memcpy(gc, &d->base, sizeof *gc);
	colour = options_get_number(d->s->options, name);
	if (!COLOUR_DEFAULT(colour))
		gc->fg = colour;
}

/* Write a string at a content position, clipped to maxw columns. */
static void printflike(6, 7)
sidebar_put(struct sidebar_draw *d, u_int x, u_int y,
    const struct grid_cell *gc, int maxw, const char *fmt, ...)
{
	va_list	 ap;
	char	*s;

	if (y >= d->h || x >= d->iw)
		return;
	if (maxw < 0 || x + (u_int)maxw > d->iw)
		maxw = d->iw - x;
	if (maxw == 0)
		return;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);

	screen_write_cursormove(&d->ctx, d->x0 + x, y, 0);
	screen_write_nputs(&d->ctx, (ssize_t)maxw, gc, "%s", s);
	free(s);
}

/* Write a string right-aligned on a row, clipped so it starts at minx. */
static void
sidebar_put_right(struct sidebar_draw *d, u_int y, const struct grid_cell *gc,
    u_int minx, const char *s)
{
	u_int	width = utf8_cstrwidth(s);

	if (minx >= d->iw)
		return;
	if (width > d->iw - minx)
		width = d->iw - minx;
	if (width == 0)
		return;
	sidebar_put(d, d->iw - width, y, gc, width, "%s", s);
}

/* Draw a horizontal rule from x to the end of the row. */
static void
sidebar_rule(struct sidebar_draw *d, u_int x, u_int y)
{
	for (; x < d->iw; x++)
		sidebar_put(d, x, y, &d->dim, 1, "%s", d->g->hline);
}

/* Record what a row points at, for the mouse. */
static void
sidebar_row_target(struct sidebar_draw *d, u_int y, int type, int id)
{
	struct sidebar	*sb = &d->c->sidebar;

	if (y < sb->nrows) {
		sb->rows[y].type = type;
		sb->rows[y].id = id;
	}
}

/* Draw a section header. */
static void
sidebar_header(struct sidebar_draw *d, u_int y, const char *name, u_int count)
{
	struct grid_cell	gc, badge;
	u_int			x;

	memcpy(&gc, &d->a2, sizeof gc);
	gc.attr |= GRID_ATTR_BRIGHT;
	sidebar_put(d, 0, y, &gc, -1, "%s", name);
	x = strlen(name);
	if (count != 0) {
		memcpy(&badge, &d->a2, sizeof badge);
		badge.attr |= GRID_ATTR_REVERSE;
		sidebar_put(d, x + 1, y, &badge, -1, "%u", count);
		x += 1 + (count < 10 ? 1 : (count < 100 ? 2 : 3));
	}
	sidebar_rule(d, x + 1, y);
}

/* Elapsed time as m:ss or h:mm. */
static void
sidebar_elapsed(char *buf, size_t len, time_t since, time_t now)
{
	time_t	secs = now - since;

	if (secs < 0)
		secs = 0;
	if (secs >= 3600)
		xsnprintf(buf, len, "%lldh%02lld", (long long)secs / 3600,
		    ((long long)secs % 3600) / 60);
	else
		xsnprintf(buf, len, "%02lld:%02lld", (long long)secs / 60,
		    (long long)secs % 60);
}

/* Summarize the state of a window. */
static enum sidebar_window_state
sidebar_window_state(struct winlink *wl, u_int *npanes)
{
	struct window			*w = wl->window;
	struct window_pane		*wp;
	enum sidebar_window_state	 state = SIDEBAR_WINDOW_IDLE;

	*npanes = 0;
	TAILQ_FOREACH(wp, &w->panes, entry) {
		(*npanes)++;
		switch (wp->sb_state) {
		case SIDEBAR_STATE_WAITING:
			state = SIDEBAR_WINDOW_WAITING;
			break;
		case SIDEBAR_STATE_BUSY:
			if (state < SIDEBAR_WINDOW_BUSY)
				state = SIDEBAR_WINDOW_BUSY;
			break;
		case SIDEBAR_STATE_DONE:
			if (state < SIDEBAR_WINDOW_DONE)
				state = SIDEBAR_WINDOW_DONE;
			break;
		case SIDEBAR_STATE_RUNNING:
			if (state < SIDEBAR_WINDOW_RUNNING)
				state = SIDEBAR_WINDOW_RUNNING;
			break;
		}
	}
	if (state == SIDEBAR_WINDOW_WAITING)
		return (state);
	if (wl->flags & WINLINK_BELL)
		return (SIDEBAR_WINDOW_BELL);
	if (state >= SIDEBAR_WINDOW_BUSY)
		return (state);
	if (wl->flags & WINLINK_ACTIVITY)
		return (SIDEBAR_WINDOW_ACTIVITY);
	if (wl->flags & WINLINK_SILENCE)
		return (SIDEBAR_WINDOW_SILENCE);
	return (state);
}

/* Update pane states and counts for the session. */
static void
sidebar_scan(struct sidebar_draw *d)
{
	struct winlink		*wl;
	struct window_pane	*wp;

	d->nwindows = d->npanes = d->nagents = 0;
	RB_FOREACH(wl, winlinks, &d->s->windows) {
		d->nwindows++;
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			d->npanes++;
			if (sidebar_pane_state(wp) != SIDEBAR_STATE_SHELL)
				d->nagents++;
		}
	}
}

/* Draw the title block. */
static void
sidebar_draw_head(struct sidebar_draw *d)
{
	struct grid_cell	gc;
	char			clock[16];
	struct tm		*tm;

	if (d->h < 2)
		return;

	memcpy(&gc, &d->a1, sizeof gc);
	gc.attr |= GRID_ATTR_BRIGHT;
	sidebar_put(d, 0, 0, &gc, -1, "%s%s", d->g->title, d->s->name);

	tm = localtime(&d->now);
	if (tm != NULL && strftime(clock, sizeof clock, "%H:%M:%S", tm) != 0)
		sidebar_put_right(d, 0, &d->base, strlen(d->s->name) + 2, clock);

	sidebar_put(d, 0, 1, &d->dim, -1, "%u window%s %s %u pane%s",
	    d->nwindows, d->nwindows == 1 ? "" : "s", d->g->dot, d->npanes,
	    d->npanes == 1 ? "" : "s");
	d->y = 3;
}

/* Draw the window list. Returns rows used. */
static void
sidebar_draw_windows(struct sidebar_draw *d, u_int avail)
{
	struct winlink			*wl;
	struct window			*w;
	enum sidebar_window_state	 state;
	const struct grid_cell		*dotgc, *namegc, *metagc;
	const char			*dot, *meta;
	char				 buf[32];
	u_int				 npanes, y, x, metaw;
	int				 current;

	if (avail < 2)
		return;
	y = d->y;
	sidebar_header(d, y++, "WINDOWS", 0);
	avail--;

	RB_FOREACH(wl, winlinks, &d->s->windows) {
		if (avail == 0)
			break;
		w = wl->window;
		current = (wl == d->s->curw);
		state = sidebar_window_state(wl, &npanes);

		switch (state) {
		case SIDEBAR_WINDOW_WAITING:
			dot = d->g->alert; dotgc = &d->warn;
			meta = "waiting"; metagc = &d->warn;
			break;
		case SIDEBAR_WINDOW_BELL:
			dot = d->g->bell; dotgc = &d->warn;
			meta = "bell"; metagc = &d->warn;
			break;
		case SIDEBAR_WINDOW_BUSY:
			dot = d->g->on; dotgc = &d->ok;
			meta = "busy"; metagc = &d->dim;
			break;
		case SIDEBAR_WINDOW_ACTIVITY:
			dot = d->g->on; dotgc = &d->ok;
			meta = "activity"; metagc = &d->dim;
			break;
		case SIDEBAR_WINDOW_RUNNING:
			dot = d->g->on; dotgc = &d->ok;
			meta = "running"; metagc = &d->dim;
			break;
		case SIDEBAR_WINDOW_DONE:
			dot = d->g->done; dotgc = &d->a1;
			meta = "done"; metagc = &d->a1;
			break;
		case SIDEBAR_WINDOW_SILENCE:
			dot = d->g->silence; dotgc = &d->dim;
			meta = "silent"; metagc = &d->dim;
			break;
		default:
			if (current) {
				dot = d->g->on; dotgc = &d->ok;
			} else {
				dot = d->g->idle; dotgc = &d->dim;
			}
			xsnprintf(buf, sizeof buf, "%u pane%s", npanes,
			    npanes == 1 ? "" : "s");
			meta = buf; metagc = &d->dim;
			break;
		}
		namegc = current ? &d->a1 : &d->base;

		if (current)
			sidebar_put(d, 0, y, &d->a1, 1, "%s", d->g->current);
		sidebar_put(d, 2, y, current ? &d->a1 : &d->dim, 2, "%d",
		    wl->idx);
		x = (wl->idx < 10) ? 4 : 5;
		metaw = strlen(meta);
		if (d->iw > x + metaw + 3)
			sidebar_put(d, x, y, namegc, d->iw - x - metaw - 3,
			    "%s", w->name);
		sidebar_put_right(d, y, dotgc, 0, dot);
		if (d->iw >= metaw + 2)
			sidebar_put(d, d->iw - metaw - 2, y, metagc, metaw,
			    "%s", meta);

		sidebar_row_target(d, y, SIDEBAR_ROW_WINDOW, wl->idx);
		y++;
		avail--;
	}
	d->y = y;
}

/* Draw the agents list. */
static void
sidebar_draw_agents(struct sidebar_draw *d, u_int avail)
{
	struct winlink		*wl;
	struct window_pane	*wp;
	const struct grid_cell	*icongc, *stategc;
	const char		*icon, *state, *task;
	char			 elapsed[16], where[32], text[48];
	u_int			 y, namew = 8, x0 = 2;
	int			 selected;

	if (avail < 2)
		return;
	y = d->y;
	sidebar_header(d, y++, "AGENTS", 0);
	avail--;
	sidebar_selected(d->c); /* drop a stale selection */

	if (d->nagents == 0) {
		sidebar_put(d, 0, y++, &d->dim, -1, "no agents running");
		d->y = y;
		return;
	}

	RB_FOREACH(wl, winlinks, &d->s->windows) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			if (avail == 0)
				break;
			if (wp->sb_state == SIDEBAR_STATE_SHELL)
				continue;
			sidebar_elapsed(elapsed, sizeof elapsed, wp->sb_since,
			    d->now);
			switch (wp->sb_state) {
			case SIDEBAR_STATE_BUSY:
				icon = d->g->spinner[(u_int)d->now % 10];
				icongc = &d->a1;
				xsnprintf(text, sizeof text, "busy %s",
				    elapsed);
				state = text; stategc = &d->base;
				break;
			case SIDEBAR_STATE_WAITING:
				icon = d->g->warn; icongc = &d->warn;
				xsnprintf(text, sizeof text, "waiting %s",
				    elapsed);
				state = text; stategc = &d->warn;
				break;
			case SIDEBAR_STATE_DONE:
				icon = d->g->ok; icongc = &d->a1;
				state = "done"; stategc = &d->a1;
				break;
			default:
				icon = d->g->dot; icongc = &d->dim;
				xsnprintf(text, sizeof text, "running %s",
				    elapsed);
				state = text; stategc = &d->dim;
				break;
			}
			if (wp->sb_activity != NULL &&
			    wp->sb_state != SIDEBAR_STATE_WAITING) {
				state = wp->sb_activity;
				stategc = (wp->sb_state == SIDEBAR_STATE_DONE) ?
				    &d->a1 : &d->base;
			}
			selected = ((int)wp->id == d->c->sidebar.sel);
			if (selected)
				sidebar_put(d, 0, y, &d->a1, 1, "%s",
				    d->g->current);
			sidebar_put(d, x0, y, selected ? &d->a1 : &d->base,
			    namew, "%s", wp->sb_cmd);
			sidebar_put(d, x0 + namew + 1, y, icongc, 1, "%s", icon);
			sidebar_put(d, x0 + namew + 3, y, stategc, -1, "%s",
			    state);
			xsnprintf(where, sizeof where, "%d:%s", wl->idx,
			    wl->window->name);
			sidebar_put_right(d, y, &d->dim,
			    x0 + namew + 4 + utf8_cstrwidth(state), where);

			sidebar_row_target(d, y, SIDEBAR_ROW_PANE, wp->id);
			y++;
			avail--;

			task = sidebar_pane_task(wp);
			if (task != NULL && avail != 0) {
				sidebar_put(d, x0 + 2, y, &d->dim, -1, "%s", task);
				sidebar_row_target(d, y, SIDEBAR_ROW_PANE, wp->id);
				y++;
				avail--;
			}
		}
	}
	d->y = y;
}

/* Draw the alert list. */
static void
sidebar_draw_alerts(struct sidebar_draw *d, u_int avail)
{
	struct alert_entry	*ae;
	const struct grid_cell	*icongc, *textgc, *srcgc;
	const char		*icon;
	struct tm		*tm;
	char			 clock[8], msg[64];
	u_int			 y, unread = alert_unread_count(), srcw = 7;

	if (avail < 2)
		return;
	y = d->y;
	sidebar_header(d, y++, "ALERTS", unread);
	avail--;

	if (TAILQ_EMPTY(&alert_list)) {
		sidebar_put(d, 0, y++, &d->dim, -1, "nothing yet");
		d->y = y;
		return;
	}

	TAILQ_FOREACH(ae, &alert_list, entry) {
		if (avail == 0)
			break;
		switch (ae->type) {
		case ALERT_OK:
			icon = d->g->ok; icongc = &d->ok;
			break;
		case ALERT_WARN:
			icon = d->g->warn; icongc = &d->warn;
			break;
		case ALERT_ERROR:
			icon = d->g->error; icongc = &d->err;
			break;
		case ALERT_BELL:
			icon = d->g->bell; icongc = &d->a2;
			break;
		case ALERT_ACTIVITY:
			icon = d->g->on; icongc = &d->ok;
			break;
		case ALERT_SILENCE:
			icon = d->g->silence; icongc = &d->dim;
			break;
		default:
			icon = d->g->info; icongc = &d->a1;
			break;
		}
		if (ae->read) {
			icongc = &d->dim;
			textgc = &d->dim;
			srcgc = &d->dim;
		} else {
			textgc = &d->base;
			srcgc = &d->base;
		}

		tm = localtime(&ae->t);
		if (tm == NULL ||
		    strftime(clock, sizeof clock, "%H:%M", tm) == 0)
			strlcpy(clock, "--:--", sizeof clock);
		sidebar_put(d, 0, y, &d->dim, 5, "%s", clock);
		sidebar_put(d, 6, y, icongc, 1, "%s", icon);
		sidebar_put(d, 8, y, srcgc, srcw, "%s", ae->source);
		if (ae->count > 1) {
			xsnprintf(msg, sizeof msg, "%s (x%u)", ae->msg,
			    ae->count);
			sidebar_put(d, 8 + srcw + 1, y, textgc, -1, "%s", msg);
		} else
			sidebar_put(d, 8 + srcw + 1, y, textgc, -1, "%s",
			    ae->msg);

		sidebar_row_target(d, y, SIDEBAR_ROW_ALERT, ae->id);
		y++;
		avail--;
	}
	d->y = y;
}

/* Number of rows in the custom section. */
static u_int
sidebar_custom_rows(struct session *s)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	u_int				 n = 0;

	o = options_get(s->options, "sidebar-custom");
	if (o == NULL)
		return (0);
	a = options_array_first(o);
	while (a != NULL) {
		n++;
		a = options_array_next(a);
	}
	return (n);
}

/* Draw the custom section: one format per row. */
static void
sidebar_draw_custom(struct sidebar_draw *d, u_int avail)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	union options_value		*ov;
	struct format_tree		*ft;
	struct window_pane		*wp = d->s->curw->window->active;
	char				*expanded;
	u_int				 y;

	o = options_get(d->s->options, "sidebar-custom");
	if (o == NULL || avail < 2)
		return;
	a = options_array_first(o);
	if (a == NULL)
		return;
	y = d->y;
	sidebar_header(d, y++, options_get_string(d->s->options,
	    "sidebar-custom-title"), 0);
	avail--;

	ft = format_create(d->c, NULL, FORMAT_NONE, FORMAT_STATUS);
	format_defaults(ft, d->c, d->s, d->s->curw, wp);
	while (a != NULL && avail != 0) {
		ov = options_array_item_value(a);
		expanded = format_expand_time(ft, ov->string);
		screen_write_cursormove(&d->ctx, d->x0, y, 0);
		format_draw(&d->ctx, &d->base, d->iw, expanded, NULL, 0);
		free(expanded);
		y++;
		avail--;
		a = options_array_next(a);
	}
	format_free(ft);
	d->y = y;
}

/* Draw the paste buffer list. */
static void
sidebar_draw_buffers(struct sidebar_draw *d, u_int avail)
{
	struct paste_buffer	*pb;
	char			*sample, sizebuf[16];
	size_t			 size;
	u_int			 y, n = 0;

	if (avail < 2 || paste_is_empty())
		return;
	y = d->y;
	sidebar_header(d, y++, "BUFFERS", 0);
	avail--;

	pb = NULL;
	while ((pb = paste_walk(pb)) != NULL) {
		if (avail == 0 || n == SIDEBAR_BUFFERS_MAX)
			break;
		paste_buffer_data(pb, &size);
		if (size >= 1024 * 1024)
			xsnprintf(sizebuf, sizeof sizebuf, "%.1fM",
			    (double)size / (1024 * 1024));
		else if (size >= 1024)
			xsnprintf(sizebuf, sizeof sizebuf, "%.1fK",
			    (double)size / 1024);
		else
			xsnprintf(sizebuf, sizeof sizebuf, "%zuB", size);
		sample = paste_make_sample(pb);

		sidebar_put(d, 0, y, &d->dim, 1, "%u", n);
		if (d->iw > strlen(sizebuf) + 3)
			sidebar_put(d, 2, y, &d->base,
			    d->iw - strlen(sizebuf) - 3, "\"%s\"", sample);
		sidebar_put_right(d, y, &d->dim, 0, sizebuf);
		free(sample);

		y++;
		avail--;
		n++;
	}
	d->y = y;
}

/* Draw the key hints at the bottom. */
static void
sidebar_draw_keys(struct sidebar_draw *d)
{
	const char	*prefix;
	char		 left[32], right[32];
	u_int		 y = d->h - SIDEBAR_KEYS_ROWS, col2;
	key_code	 key;

	key = options_get_number(d->s->options, "prefix");
	prefix = key_string_lookup_key(key, 0);
	if (prefix == NULL || strlen(prefix) > 6)
		prefix = "prefix";
	col2 = d->iw / 2;
	if (col2 < strlen(prefix) + 10)
		col2 = strlen(prefix) + 10;

	sidebar_rule(d, 0, y);
	if (d->c->keytable != NULL &&
	    strcmp(d->c->keytable->name, "sidebar") == 0) {
		/* Agent selection mode. */
		sidebar_put(d, 0, y + 1, &d->a2, -1, "AGENT MODE");
		sidebar_put(d, 0, y + 2, &d->base, -1, "j/k");
		sidebar_put(d, 4, y + 2, &d->dim, -1, "select");
		sidebar_put(d, col2, y + 2, &d->base, -1, "Enter");
		sidebar_put(d, col2 + 6, y + 2, &d->dim, -1, "jump");
		sidebar_put(d, 0, y + 3, &d->base, -1, "p");
		sidebar_put(d, 2, y + 3, &d->dim, -1, "peek");
		sidebar_put(d, 7, y + 3, &d->base, -1, "K");
		sidebar_put(d, 9, y + 3, &d->dim, -1, "kill");
		sidebar_put(d, col2, y + 3, &d->base, -1, "i");
		sidebar_put(d, col2 + 2, y + 3, &d->dim, -1, "input");
		sidebar_put(d, col2 + 8, y + 3, &d->base, -1, "q");
		sidebar_put(d, col2 + 10, y + 3, &d->dim, -1, "back");
		return;
	}

	xsnprintf(left, sizeof left, "%s b", prefix);
	xsnprintf(right, sizeof right, "%s N", prefix);
	sidebar_put(d, 0, y + 1, &d->base, -1, "%s", left);
	sidebar_put(d, strlen(left) + 1, y + 1, &d->dim, -1, "toggle");
	sidebar_put(d, col2, y + 1, &d->base, -1, "%s", right);
	sidebar_put(d, col2 + strlen(right) + 1, y + 1, &d->dim, -1,
	    "next alert");

	xsnprintf(left, sizeof left, "%s B", prefix);
	xsnprintf(right, sizeof right, "%s C-n", prefix);
	sidebar_put(d, 0, y + 2, &d->base, -1, "%s", left);
	sidebar_put(d, strlen(left) + 1, y + 2, &d->dim, -1, "side");
	sidebar_put(d, col2, y + 2, &d->base, -1, "%s", right);
	sidebar_put(d, col2 + strlen(right) + 1, y + 2, &d->dim, -1, "clear");

	xsnprintf(left, sizeof left, "%s S", prefix);
	xsnprintf(right, sizeof right, "%s < >", prefix);
	sidebar_put(d, 0, y + 3, &d->base, -1, "%s", left);
	sidebar_put(d, strlen(left) + 1, y + 3, &d->dim, -1, "agents");
	sidebar_put(d, col2, y + 3, &d->base, -1, "%s", right);
	sidebar_put(d, col2 + strlen(right) + 1, y + 3, &d->dim, -1, "width");
}

/* Parse the sidebar-sections option into an ordered list. */
static u_int
sidebar_parse_sections(struct session *s, enum sidebar_section *out,
    u_int max)
{
	const char	*value = options_get_string(s->options,
			    "sidebar-sections");
	char		*copy, *next, *tok;
	u_int		 n = 0, i, j;

	copy = xstrdup(value);
	next = copy;
	while ((tok = strsep(&next, ", ")) != NULL) {
		if (*tok == '\0' || n == max)
			continue;
		for (i = 0; sidebar_section_names[i] != NULL; i++) {
			if (strcmp(tok, sidebar_section_names[i]) != 0)
				continue;
			for (j = 0; j < n; j++) {
				if (out[j] == (enum sidebar_section)i)
					break;
			}
			if (j == n)
				out[n++] = i;
			break;
		}
	}
	free(copy);
	return (n);
}

/* Minimum rows a section needs (header, one row, separator). */
static u_int
sidebar_section_min(enum sidebar_section section)
{
	if (section == SIDEBAR_SECTION_KEYS)
		return (0);
	return (3);
}

/* Natural rows a section would like. */
static u_int
sidebar_section_natural(struct sidebar_draw *d, enum sidebar_section section)
{
	struct winlink		*wl;
	struct window_pane	*wp;
	u_int			 n = 0;

	switch (section) {
	case SIDEBAR_SECTION_WINDOWS:
		return (2 + d->nwindows);
	case SIDEBAR_SECTION_AGENTS:
		if (d->nagents == 0)
			return (3);
		n = 0;
		RB_FOREACH(wl, winlinks, &d->s->windows) {
			TAILQ_FOREACH(wp, &wl->window->panes, entry) {
				if (wp->sb_state == SIDEBAR_STATE_SHELL)
					continue;
				n += 1 + (sidebar_pane_task(wp) != NULL);
			}
		}
		return (2 + n);
	case SIDEBAR_SECTION_ALERTS:
		return (2 + (alert_total == 0 ? 1 : alert_total));
	case SIDEBAR_SECTION_CUSTOM:
		n = sidebar_custom_rows(d->s);
		return (n == 0 ? 0 : 2 + n);
	case SIDEBAR_SECTION_BUFFERS:
		if (paste_is_empty())
			return (0);
		n = 0;
		if (!paste_is_empty()) {
			struct paste_buffer	*pb = NULL;

			while ((pb = paste_walk(pb)) != NULL &&
			    n < SIDEBAR_BUFFERS_MAX)
				n++;
		}
		return (2 + n);
	default:
		return (0);
	}
}

/* Draw the full sidebar. */
static void
sidebar_draw_full(struct sidebar_draw *d)
{
	enum sidebar_section	sections[8];
	u_int			n, i, j, reserve, avail, rows, keys = 0;

	n = sidebar_parse_sections(d->s, sections, nitems(sections));
	for (i = 0; i < n; i++) {
		if (sections[i] == SIDEBAR_SECTION_KEYS)
			keys = 1;
	}
	d->bottom = d->h;
	if (keys && d->h >= 12)
		d->bottom = d->h - SIDEBAR_KEYS_ROWS;

	sidebar_draw_head(d);

	for (i = 0; i < n; i++) {
		if (d->y >= d->bottom)
			break;
		avail = d->bottom - d->y;

		/* Keep the minimum for the sections still to come. */
		reserve = 0;
		for (j = i + 1; j < n; j++)
			reserve += sidebar_section_min(sections[j]);
		if (avail <= reserve)
			avail = 0;
		else
			avail -= reserve;

		rows = sidebar_section_natural(d, sections[i]);
		if (sections[i] == SIDEBAR_SECTION_ALERTS)
			rows = avail;
		else if (rows > avail)
			rows = avail;
		if (rows == 0)
			continue;

		switch (sections[i]) {
		case SIDEBAR_SECTION_WINDOWS:
			sidebar_draw_windows(d, rows);
			break;
		case SIDEBAR_SECTION_AGENTS:
			sidebar_draw_agents(d, rows);
			break;
		case SIDEBAR_SECTION_ALERTS:
			sidebar_draw_alerts(d, rows);
			break;
		case SIDEBAR_SECTION_CUSTOM:
			sidebar_draw_custom(d, rows);
			break;
		case SIDEBAR_SECTION_BUFFERS:
			sidebar_draw_buffers(d, rows);
			break;
		default:
			break;
		}
		d->y++; /* separator */
	}

	if (keys && d->bottom != d->h)
		sidebar_draw_keys(d);
}

/* Draw the compact sidebar: window indexes and the alert count. */
static void
sidebar_draw_mini(struct sidebar_draw *d)
{
	struct winlink			*wl;
	enum sidebar_window_state	 state;
	const struct grid_cell		*dotgc, *idxgc;
	const char			*dot;
	u_int				 npanes, y = 0, unread;
	int				 current;

	RB_FOREACH(wl, winlinks, &d->s->windows) {
		if (y + 3 > d->h)
			break;
		current = (wl == d->s->curw);
		state = sidebar_window_state(wl, &npanes);
		switch (state) {
		case SIDEBAR_WINDOW_WAITING:
		case SIDEBAR_WINDOW_BELL:
			dot = d->g->alert; dotgc = &d->warn;
			break;
		case SIDEBAR_WINDOW_BUSY:
		case SIDEBAR_WINDOW_ACTIVITY:
		case SIDEBAR_WINDOW_RUNNING:
			dot = d->g->on; dotgc = &d->ok;
			break;
		case SIDEBAR_WINDOW_DONE:
			dot = d->g->done; dotgc = &d->a1;
			break;
		default:
			if (current) {
				dot = d->g->on; dotgc = &d->ok;
			} else {
				dot = d->g->idle; dotgc = &d->dim;
			}
			break;
		}
		idxgc = current ? &d->a1 : &d->dim;
		sidebar_put(d, 0, y, idxgc, 2, "%d", wl->idx);
		sidebar_put_right(d, y, dotgc, 0, dot);
		sidebar_row_target(d, y, SIDEBAR_ROW_WINDOW, wl->idx);
		y++;
	}

	if (d->h >= 3) {
		unread = alert_unread_count();
		if (unread != 0) {
			struct grid_cell	gc;

			memcpy(&gc, &d->a2, sizeof gc);
			gc.attr |= GRID_ATTR_BRIGHT;
			sidebar_put(d, 0, d->h - 2, &gc, -1, "%u", unread);
			sidebar_put(d, 0, d->h - 1, &d->dim, -1, "alrt");
		}
	}
}

/* Build the sidebar screen. Returns 0 if there is no sidebar. */
int
sidebar_redraw(struct client *c)
{
	struct sidebar		*sb = &c->sidebar;
	struct session		*s = c->session;
	struct sidebar_draw	 d;
	struct format_tree	*ft;
	struct grid_cell	 border;
	u_int			 w, h, y, bx;
	int			 mini;

	w = sidebar_size(c);
	h = sidebar_height(c);
	if (w == 0 || h == 0)
		return (0);

	if (screen_size_x(&sb->screen) != w || screen_size_y(&sb->screen) != h) {
		screen_resize(&sb->screen, w, h, 0);
		sb->force = 1;
	}
	if (sb->nrows != h) {
		free(sb->rows);
		sb->rows = xcalloc(h, sizeof *sb->rows);
		sb->nrows = h;
	} else
		memset(sb->rows, 0, h * sizeof *sb->rows);

	memset(&d, 0, sizeof d);
	d.c = c;
	d.s = s;
	d.w = w;
	d.h = h;
	d.now = time(NULL);
	if (c->flags & CLIENT_UTF8)
		d.g = &sidebar_glyphs_utf8;
	else
		d.g = &sidebar_glyphs_ascii;

	/*
	 * Border column faces the panes; content is padded by one column on
	 * each side (only on the pane side in mini mode).
	 */
	mini = (s->sidebarwidth == SIDEBAR_MINI_WIDTH &&
	    options_get_number(s->options, "sidebar-mode") == 1);
	if (s->sidebarat == 0) {
		bx = w - 1;
		d.x0 = mini ? 0 : 1;
	} else {
		bx = 0;
		d.x0 = mini ? 1 : 2;
	}
	if (mini)
		d.iw = (w >= 2) ? w - 2 : 0;
	else
		d.iw = (w >= 3) ? w - 3 : 0;

	/* Colours. */
	ft = format_create_defaults(NULL, c, s, s->curw, NULL);
	memcpy(&d.base, &grid_default_cell, sizeof d.base);
	style_apply(&d.base, s->options, "sidebar-style", ft);
	format_free(ft);
	sidebar_colour(&d, &d.dim, "sidebar-dim");
	sidebar_colour(&d, &d.a1, "sidebar-accent");
	sidebar_colour(&d, &d.a2, "sidebar-accent2");
	sidebar_colour(&d, &d.ok, "sidebar-ok");
	sidebar_colour(&d, &d.warn, "sidebar-warn");
	sidebar_colour(&d, &d.err, "sidebar-error");

	screen_write_start(&d.ctx, &sb->screen);
	screen_write_clearscreen(&d.ctx, d.base.bg);

	memcpy(&border, &d.a1, sizeof border);
	for (y = 0; y < h; y++) {
		screen_write_cursormove(&d.ctx, bx, y, 0);
		screen_write_nputs(&d.ctx, 1, &border, "%s", d.g->vline);
	}

	sidebar_scan(&d);
	if (mini)
		sidebar_draw_mini(&d);
	else
		sidebar_draw_full(&d);

	screen_write_stop(&d.ctx);
	return (1);
}

/* Has a line changed between two screens? */
static int
sidebar_line_changed(struct screen *a, struct screen *b, u_int y)
{
	struct grid_cell	gca, gcb;
	u_int			x;

	for (x = 0; x < screen_size_x(a); x++) {
		grid_get_cell(a->grid, x, y, &gca);
		grid_get_cell(b->grid, x, y, &gcb);
		if (!grid_cells_equal(&gca, &gcb))
			return (1);
		if (gca.data.size != gcb.data.size ||
		    memcmp(gca.data.data, gcb.data.data, gca.data.size) != 0)
			return (1);
	}
	return (0);
}

/* Draw the sidebar screen to the terminal. */
void
sidebar_draw(struct client *c, int force)
{
	struct sidebar		*sb = &c->sidebar;
	struct tty		*tty = &c->tty;
	struct visible_ranges	*r;
	struct visible_range	*rr;
	u_int			 w, h, x0, y0, i, j;

	w = sidebar_size(c);
	h = sidebar_height(c);
	if (w == 0 || h == 0)
		return;
	if (screen_size_x(&sb->screen) != w || screen_size_y(&sb->screen) != h)
		return;
	if (screen_size_x(&sb->old) != w || screen_size_y(&sb->old) != h) {
		screen_resize(&sb->old, w, h, 0);
		force = 1;
	}
	if (sb->force)
		force = 1;

	x0 = sidebar_x(c);
	y0 = sidebar_y(c);
	for (i = 0; i < h; i++) {
		if (!force && !sidebar_line_changed(&sb->screen, &sb->old, i))
			continue;
		r = tty_check_overlay_range(tty, x0, y0 + i, w);
		for (j = 0; j < r->used; j++) {
			rr = &r->ranges[j];
			if (rr->nx == 0)
				continue;
			tty_draw_line(tty, &sb->screen, rr->px - x0, i, rr->nx,
			    rr->px, y0 + i, NULL);
		}
		grid_duplicate_lines(sb->old.grid, i, sb->screen.grid, i, 1);
	}
	sb->force = 0;
}
