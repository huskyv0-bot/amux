# amux — tmux with a sidebar for agent work

amux is a fork of [tmux](https://github.com/tmux/tmux) that adds a vertical
**sidebar** beside the panes. It is built for running several coding agents
(Claude Code, Codex, pi, builds, …) in one session and seeing at a glance
which one needs you.

```
┌──────────────────────────────────────────┬──────────────────────────────┐
│ 0:claude              │ 1:codex          │ ▌sfx               14:02:31  │
│                       │                  │ 4 windows · 6 panes          │
│ > Bau die Sidebar …   │ Apply? [y/N] █   │                              │
│                       ├──────────────────┤ WINDOWS ──────────────────── │
│                       │ 2:build          │ ▶ 0 claude           busy ◉  │
│                       │ ✓ build ok       │   1 codex         waiting ◉  │
│                       │                  │   2 build            done ◎  │
│                       │                  │                              │
│                       │                  │ AGENTS ───────────────────── │
│                       │                  │ claude   ⠋ busy 00:42 0:cla  │
│                       │                  │ codex    ! waiting 01:10 1:  │
│                       │                  │                              │
│                       │                  │ ALERTS 2 ─────────────────── │
│                       │                  │ 14:01 ! codex   Confirm [y/N]│
│                       │                  │ 13:58 ✓ build   make ok 12s  │
├───────────────────────┴──────────────────┴──────────────────────────────┤
│ [sfx] 0:claude* 1:codex! 2:build                          30 Aug 14:02  │
└─────────────────────────────────────────────────────────────────────────┘
```

Everything else is unchanged tmux (`next-3.8`), so your existing config,
plugins and muscle memory keep working.

## What the sidebar shows

| Section   | Content |
|-----------|---------|
| `windows` | Every window with its state: `waiting`, `bell`, `busy`, `activity`, `running`, `done`, `silent` or the pane count. Click a row to select the window. |
| `agents`  | Every pane whose foreground process is not the shell, with its command, state, time in that state and location. Click a row to jump there. |
| `alerts`  | A history (newest first) of bells, activity/silence alerts, agents that started waiting for input and anything sent with `tmux notify`. Unread alerts are bright, read ones dimmed. Click to jump. |
| `custom`  | Your own rows from `sidebar-custom[]` (formats, `#()` allowed) under `sidebar-custom-title` — by default git branch, load and host. |
| `buffers` | The most recent paste buffers. |
| `keys`    | The sidebar key bindings, using your real prefix. |

Agent states are detected automatically: a pane running something other
than its shell is `running`; if the last visible lines match
`sidebar-busy-pattern` it is `busy`, if they match `sidebar-wait-pattern`
it is `waiting` (which also raises an alert). Tools can set the state
explicitly with `tmux notify -s …`, which is more reliable than pattern
matching.

## Keys (with the default prefix `C-b`)

| Key          | Action |
|--------------|--------|
| `prefix b`   | Toggle the sidebar |
| `prefix B`   | Move the sidebar to the other side |
| `prefix N`   | Jump to the oldest unread alert (window + pane) and mark it read |
| `prefix C-n` | Clear all alerts |
| `prefix S`   | Agent mode: `j`/`k` select, `Enter` jump, `p` peek at the scrollback (popup), `K` kill (with confirm), `i` send a line, `n` next alert, `q` back |
| `prefix <` / `>` | Narrower / wider sidebar |
| `prefix O`   | Overview: every session, window and pane as a tree with agent state, activity and time, plus a live preview of the selected pane (`Enter` jumps, `q` closes). The big version of the sidebar. |
| `prefix T`   | Palette menu (theme only, see below) |

## Options (session options, `set -g …`)

| Option | Default | Meaning |
|--------|---------|---------|
| `sidebar` | `on` | Show the sidebar |
| `sidebar-position` | `left` | `left` or `right` |
| `sidebar-mode` | `full` | `full`, or `mini` for a 6-column rail |
| `sidebar-width` | `0` = auto | Columns; `0` picks 22% of the terminal (30–60). Hidden automatically if the terminal is narrower than width + 40 |
| `sidebar-sections` | `windows,agents,alerts,custom,buffers,keys` | Sections and their order |
| `sidebar-custom[]` | git branch, load, host | Rows of the custom section, one format per array member |
| `sidebar-custom-title` | `HOST` | Title of the custom section |
| `sidebar-interval` | `1` | Refresh every N seconds (`0` = only on events) |
| `sidebar-style` | `bg=#0a0c16,fg=#d9dcea` | Base style |
| `sidebar-accent` | `#22f3ff` | Border, title, current window |
| `sidebar-accent2` | `#ff3ec9` | Section headers, bells |
| `sidebar-dim` | `#555b7a` | Secondary text |
| `sidebar-ok` / `sidebar-warn` / `sidebar-error` | `#7dff5c` / `#ffc63a` / `#ff4f6d` | Semantic colours |
| `sidebar-busy-pattern` | `esc to interrupt\|…` | Regex for a busy agent |
| `sidebar-wait-pattern` | `\[[yY]/[nN]\]\|Do you want\|…` | Regex for an agent waiting for input |
| `alert-history` | `50` | Alerts kept in the history |

## Theme: the look of the mockup

The sidebar alone does not restyle the rest of tmux. `amux.conf` is the full
neon theme from the mockup — dark pane background, thin borders with an
accent-coloured active pane, `┤ 0 claude ├` pane titles, a status line with
the session in an accent block and `⚡ alerts │ path │ time` on the right.
amux loads **`~/.config/amux/amux.conf`** after `~/.tmux.conf`, so the theme
only affects amux, never a plain tmux:

```sh
mkdir -p ~/.config/amux
printf 'source-file ~/amux/amux.conf\n' >> ~/.config/amux/amux.conf
```

Put your own overrides below that line. Panes tagged with `@area_color` keep
their own border colour. Your terminal's colour scheme (iTerm2, Ghostty, …)
only supplies the default colours; the theme sets its own background, so the
look is the same everywhere as long as the terminal supports RGB.

### Palettes

The colours live in `themes/<palette>.conf`:

| Palette  | Accents               | Ground        |
|----------|-----------------------|---------------|
| `synth`  | `#22f3ff` / `#ff3ec9` | deep navy     |
| `acid`   | `#b9ff3c` / `#3cffd9` | black-green   |
| `sunset` | `#ff8f3c` / `#ff3c7e` | plum          |
| `ultra`  | `#b07cff` / `#3ce9ff` | indigo        |

`prefix T` opens a menu to switch the palette live; the choice is stored as
`~/.config/amux/palette.conf` (a symlink) and restored on the next start.
From a script: `tmux source-file ~/amux/themes/acid.conf`.

Every palette also sets `@amux_a1`, `@amux_a2`, `@amux_dim`, `@amux_dim2`,
`@amux_fg`, `@amux_fg2`, `@amux_panel` and `@amux_bg`, so your own formats
follow the palette:

```tmux
set -g status-right '#[fg=#{@amux_a2},bold]⚡ #{alert_count} #[fg=#{@amux_fg}]%H:%M '
```

## The `notify` command

```
notify [-Cnr] [-c target-client] [-l level] [-s state] [-S source] [-t target-pane] [message]
```

- `tmux notify -l ok -S build "make ok 12.4s"` — add an alert (`-l` is
  `info`, `ok`, `warn`, `error` or `bell`; `message` is a format).
- `tmux notify -s waiting "needs approval"` — set the pane state
  (`busy`, `waiting`, `done`, `running`, `idle`, `auto`) and add an alert.
- `tmux notify -a "Bash: make -j8"` — set the activity text shown beside the
  agent (`-a ""` clears it).
- `tmux notify -T "Sidebar bauen"` — set what the agent is working on; shown
  as a second line under the agent and in the overview. Without it the pane
  title is used (Claude Code sets it to a summary of the conversation).
- `tmux notify -n` — jump to the oldest unread alert.
- `tmux notify -r` — mark all read; `tmux notify -C` — clear.

Without `-t` the command applies to the pane it is run from, so agents and
scripts can report on themselves.

### Claude Code hooks

`bin/amux-hook` turns Claude Code hook events into sidebar state — busy on a
prompt, the current tool as activity (`Bash: make -j8`), waiting + alert on a
notification, done + alert on stop. It finds the right pane and server from
`$TMUX_PANE`/`$TMUX`, so it works with any number of agents and does nothing
under a plain tmux. Register it in `~/.claude/settings.json`:

```json
{
  "hooks": {
    "SessionStart":     [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "PreToolUse":       [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "PreCompact":       [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "~/amux/bin/amux-hook" }] }]
  }
}
```

Inside the pane `tmux` must resolve to the amux binary (put it first in
`PATH` or use the full path).

### Shell one-liners

```sh
make -j8 && tmux notify -l ok -S build "make ok" || tmux notify -l error -S build "make failed"
```

## Formats

`#{sidebar_width}` (0 when hidden), `#{alert_count}` (unread alerts),
`#{pane_agent_state}` (`shell`, `running`, `busy`, `waiting`, `done`),
`#{pane_agent_activity}`, `#{pane_agent_task}`, `#{pane_agent_since}`
(seconds in the current state), `#{sidebar_selected_pane}` and `#{sidebar_selected_cmd}` are available
in status lines, prompts, `choose-tree -F` and `if -F`.

## Building and the `amux` command

Same as tmux:

```sh
sh autogen.sh && ./configure && make
```

`bin/amux` runs the built binary on its own socket, so a plain tmux and its
sessions are never touched. Put it on your `PATH`:

```sh
ln -s ~/amux/bin/amux ~/.local/bin/amux
amux                 # attach to the most recent session, or create "main"
amux attach sfx      # attach to a session by name (amux a sfx)
amux new work        # create a session and attach (amux n work)
amux ls              # any other tmux command is passed through
amux notify -l ok "done"
```

Inside amux panes `tmux` should resolve to the amux binary (`bin/tmux` is a
symlink to it); add to your shell rc:

```sh
# inside an amux server, make `tmux` talk to it
if [ -n "$TMUX" ]; then
  _p=${TMUX#*,}; _p=${_p%%,*}
  case "$(readlink "/proc/$_p/exe" 2>/dev/null)" in */amux/tmux) PATH="$HOME/amux/bin:$PATH";; esac
  unset _p
fi
```

## Where the code lives

- `sidebar.c` — layout, drawing, agent detection, alert history, mouse.
- `cmd-notify.c` — the `notify` command.
- `resize.c`, `tty.c`, `screen-redraw.c`, `screen-write.c`,
  `server-client.c`, `cmd.c` — the window area is narrowed by the sidebar
  width and, for a left sidebar, shifted right (the same way the status line
  takes lines and shifts panes down).
- `alerts.c` — bell/activity/silence feed the alert history.
- `options-table.c`, `format.c`, `key-bindings.c`, `tmux.1` — options,
  formats, default keys, documentation.
