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
 * Work with the agent selected in the sidebar: move the selection, jump to
 * it, peek at its scrollback, kill it or send it a line of input.
 */

static enum cmd_retval	cmd_sidebar_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_sidebar_entry = {
	.name = "sidebar",
	.alias = NULL,

	.args = { "c:i:jknPpt:w:", 0, 0, NULL },
	.usage = "[-jknPp] [-c target-client] [-i text] [-w adjustment] "
		 CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, CMD_FIND_CANFAIL },

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_sidebar_exec
};

static enum cmd_retval
cmd_sidebar_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct client		*tc = cmdq_get_target_client(item);
	struct window_pane	*wp;
	char			*cause;
	long long		 n;

	if (tc == NULL || tc->session == NULL) {
		cmdq_error(item, "no client");
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'w')) {
		n = args_strtonum(args, 'w', -200, 200, &cause);
		if (cause != NULL) {
			cmdq_error(item, "adjustment %s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
		sidebar_set_width(tc, n);
		if (!args_has(args, 'n') && !args_has(args, 'p') &&
		    !args_has(args, 'j') && !args_has(args, 'k') &&
		    !args_has(args, 'i') && !args_has(args, 'P'))
			return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 't') && target->wp != NULL)
		tc->sidebar.sel = target->wp->id;
	if (args_has(args, 'n'))
		sidebar_select_move(tc, 1);
	if (args_has(args, 'p'))
		sidebar_select_move(tc, -1);

	wp = sidebar_selected(tc);
	if (wp == NULL) {
		if (args_has(args, 'j') || args_has(args, 'k') ||
		    args_has(args, 'i') || args_has(args, 'P'))
			status_message_set(tc, -1, 1, 0, 0, "no agent selected");
		sidebar_redraw_all();
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'j'))
		sidebar_jump(tc, wp);
	if (args_has(args, 'i'))
		sidebar_send_input(tc, wp, args_get(args, 'i'));
	if (args_has(args, 'k')) {
		status_message_set(tc, -1, 1, 0, 0, "killed %s (%%%u)",
		    wp->sb_cmd != NULL ? wp->sb_cmd : "pane", wp->id);
		server_kill_pane(wp);
		tc->sidebar.sel = -1;
	}
	sidebar_redraw_all();

	if (args_has(args, 'P')) {
		if (sidebar_preview(item, tc, wp) != 0) {
			cmdq_error(item, "cannot open preview");
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_WAIT);
	}
	return (CMD_RETURN_NORMAL);
}
