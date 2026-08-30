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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Add an alert to the sidebar history, set the state of an agent pane, or
 * jump to / clear alerts.
 */

static enum cmd_retval	cmd_notify_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_notify_entry = {
	.name = "notify",
	.alias = NULL,

	.args = { "a:Cc:l:nrs:S:t:", 0, 1, NULL },
	.usage = "[-Cnr] [-a activity] [-c target-client] [-l level] "
		 "[-s state] [-S source] " CMD_TARGET_PANE_USAGE " [message]",

	.target = { 't', CMD_FIND_PANE, CMD_FIND_CANFAIL },

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG|CMD_CLIENT_CANFAIL,
	.exec = cmd_notify_exec
};

static int
cmd_notify_level(const char *name, enum alert_type *type)
{
	if (strcmp(name, "info") == 0)
		*type = ALERT_INFO;
	else if (strcmp(name, "ok") == 0 || strcmp(name, "success") == 0)
		*type = ALERT_OK;
	else if (strcmp(name, "warn") == 0 || strcmp(name, "warning") == 0)
		*type = ALERT_WARN;
	else if (strcmp(name, "error") == 0 || strcmp(name, "err") == 0)
		*type = ALERT_ERROR;
	else if (strcmp(name, "bell") == 0)
		*type = ALERT_BELL;
	else
		return (-1);
	return (0);
}

static enum cmd_retval
cmd_notify_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct client		*tc = cmdq_get_target_client(item);
	struct window		*w = NULL;
	struct window_pane	*wp = target->wp;
	struct alert_entry	*ae;
	enum alert_type		 type = ALERT_INFO;
	const char		*level, *source, *statename;
	char			*msg;
	int			 state = -2, did = 0;

	if (args_has(args, 'C')) {
		alert_clear_all();
		did = 1;
	}
	if (args_has(args, 'r')) {
		alert_mark_all_read();
		did = 1;
	}

	if (args_has(args, 'n')) {
		if (tc == NULL || tc->session == NULL) {
			cmdq_error(item, "no client");
			return (CMD_RETURN_ERROR);
		}
		ae = alert_next_unread();
		if (ae == NULL) {
			status_message_set(tc, -1, 1, 0, 0, "no unread alerts");
			return (CMD_RETURN_NORMAL);
		}
		if (alert_jump(tc, ae) != 0) {
			status_message_set(tc, -1, 1, 0, 0,
			    "alert from %s is not in this session", ae->source);
		}
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 's')) {
		statename = args_get(args, 's');
		state = sidebar_state_from_name(statename);
		if (state == -2) {
			cmdq_error(item, "unknown state: %s", statename);
			return (CMD_RETURN_ERROR);
		}
		if (wp == NULL) {
			cmdq_error(item, "no target pane");
			return (CMD_RETURN_ERROR);
		}
		sidebar_pane_set_state(wp, state);
		did = 1;
	}

	if (args_has(args, 'a')) {
		if (wp == NULL) {
			cmdq_error(item, "no target pane");
			return (CMD_RETURN_ERROR);
		}
		sidebar_pane_set_activity(wp, args_get(args, 'a'));
		did = 1;
	}

	if (args_count(args) == 1) {
		level = args_get(args, 'l');
		if (level == NULL) {
			if (state == SIDEBAR_STATE_WAITING)
				type = ALERT_WARN;
			else if (state == SIDEBAR_STATE_DONE)
				type = ALERT_OK;
			else
				type = ALERT_INFO;
		} else if (cmd_notify_level(level, &type) != 0) {
			cmdq_error(item, "unknown level: %s", level);
			return (CMD_RETURN_ERROR);
		}

		if (wp != NULL)
			w = wp->window;
		else if (target->wl != NULL)
			w = target->wl->window;

		source = args_get(args, 'S');
		if (source == NULL) {
			if (wp != NULL && wp->sb_cmd != NULL &&
			    *wp->sb_cmd != '\0')
				source = wp->sb_cmd;
			else if (w != NULL)
				source = w->name;
			else
				source = "notify";
		}

		msg = format_single_from_target(item, args_string(args, 0));
		alert_push(type, w, wp, source, msg);
		free(msg);
		did = 1;
	}

	if (!did) {
		cmdq_error(item, "nothing to do: give a message, -a, -s, -n, "
		    "-r or -C");
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
}
