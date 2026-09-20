#!/usr/bin/env bash
# Check for GhosttyWin32#224: can a program running in the terminal
# get the Windows clipboard with an OSC 52 read?
#
# Run this inside a pane whose pty is not ConPTY's — a WSL pane,
# or a shell reached over ssh.
#
# A pwsh pane cannot answer the question: conhost parses OSC 52 on the way out
# and, for a query, drops it without replying or forwarding it,
# so the terminal never sees the request. The case is in microsoft/terminal,
# src/terminal/parser/OutputStateMachineEngine.cpp, under OscActionCodes::SetClipboard.
#
#   bash clipboard-osc52.sh ask     # the default config: expect no data
#   bash clipboard-osc52.sh allow   # clipboard-read = allow: expect the marker
#
# The clipboard is seeded through the terminal as well, with an OSC 52 write,
# so the check needs nothing from the Windows side.

set -u

mode="${1:-ask}"
case "$mode" in
ask | allow) ;;
*)
    echo "usage: $0 [ask|allow]" >&2
    exit 2
    ;;
esac

marker="ghostty-osc52-$$"
printf '\033]52;c;%s\a' "$(printf '%s' "$marker" | base64 | tr -d '\n')"
# The write travels to the host and on to the Windows clipboard;
# the query below must not overtake it.
sleep 0.5

# The reply arrives on stdin, so the line discipline must not eat it:
# raw, no echo, and a read that gives up after a second of silence
# instead of waiting for a newline that never comes.
saved_tty=$(stty -g)
stty raw -echo min 0 time 10
printf '\033]52;c;?\a'
reply=$(cat)
stty "$saved_tty"

if [ -z "$reply" ]; then
    echo "FAIL ($mode): no reply at all."
    echo "  Either this pane's pty is ConPTY's after all,"
    echo "  or the request never reached the host."
    exit 1
fi

# Everything between the introducer and the terminator,
# which ghostty ends with ST rather than BEL.
rest=${reply#*]52;c;}
payload=${rest%%$'\033'*}
payload=${payload%%$'\a'*}

printed=$(printf '%s' "$reply" | tr -d '\r' | sed 's/\x1b/<ESC>/g; s/\x07/<BEL>/g')
echo "reply: $printed"

if [ "$mode" = ask ]; then
    if [ -z "$payload" ]; then
        echo "PASS (ask): answered with no data, so the clipboard stayed here."
        exit 0
    fi
    echo "FAIL (ask): the reply carries data."
    echo "  The host confirmed a request it cannot put to the user."
    echo "  Decoded: $(printf '%s' "$payload" | base64 -d)"
    exit 1
fi

decoded=$(printf '%s' "$payload" | base64 -d 2>/dev/null || true)
if [ "$decoded" = "$marker" ]; then
    echo "PASS (allow): the clipboard came back,"
    echo "  so a refusal under 'ask' is a refusal and not a broken path."
    exit 0
fi

echo "FAIL (allow): expected '$marker', got '$decoded'."
exit 1
