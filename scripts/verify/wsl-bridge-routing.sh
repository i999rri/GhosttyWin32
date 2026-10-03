#!/usr/bin/env bash
# Which path is this WSL tab on, the bridge or ConPTY?
#
# Run it inside the tab. Both answers it gives have to come from in here:
# the process tree above this shell is in the distribution, and the OSC 52
# round trip is a conversation with the terminal that only the program
# holding the pty can have.
#
#   bash wsl-bridge-routing.sh
#
# The two are asked separately on purpose. The ancestry says how the tab
# was started; the round trip says what the stream does. A tab that was
# started through the bridge but whose bytes do not survive the trip is a
# different bug from one that was never on the bridge at all.

set -u

# A reply has to arrive within this; the terminal answers at once or not
# at all.
kReplyTenths=10

printf '%s\n' "== how this tab was started =="

# comm is cut to 15 characters by the kernel, so ghostty-wsl-bridge never
# appears in full here.
bridge_found=no
pid=$$
for _ in 1 2 3 4 5 6 7 8; do
    [ "$pid" -gt 1 ] || break
    read -r ppid comm <<EOF
$(ps -o ppid=,comm= -p "$pid" 2>/dev/null)
EOF
    [ -n "${comm:-}" ] || break
    printf '  %s %s\n' "$pid" "$comm"
    case "$comm" in
    ghostty-wsl-bri*) bridge_found=yes ;;
    esac
    pid=$ppid
done

if [ "$bridge_found" = yes ]; then
    printf '  -> started through the bridge\n'
else
    printf '  -> no ghostty-wsl-bridge above this shell: ConPTY, or not a Ghostty tab\n'
fi

printf '\n%s\n' "== what the stream does =="

# The reply arrives on stdin, so the line discipline must not eat it: raw,
# no echo, and a read that gives up instead of waiting for a newline that
# never comes. ConPTY answers OSC 52 itself and forwards nothing, so the
# silence is the signal.
saved_tty=$(stty -g)
stty raw -echo min 0 time "$kReplyTenths"
printf '\033]52;c;?\a'
reply=$(cat)
stty "$saved_tty"

if [ -n "$reply" ]; then
    printed=$(printf '%s' "$reply" | tr -d '\r' | sed 's/\x1b/<ESC>/g; s/\x07/<BEL>/g')
    printf '  OSC 52 answered: %s\n' "$printed"
    printf '  -> the terminal sees what this shell writes\n'
    stream=bridge
else
    printf '  OSC 52 went unanswered\n'
    printf '  -> something between here and the terminal is holding it\n'
    stream=conpty
fi

printf '\n'
if [ "$bridge_found" = yes ] && [ "$stream" = bridge ]; then
    printf 'bridge: started through it, and the stream reaches the terminal.\n'
    exit 0
fi

if [ "$bridge_found" = no ] && [ "$stream" = conpty ]; then
    printf 'ConPTY: the tab was not started through the bridge, and the stream\n'
    printf 'is being answered before it gets out. This is the default, and what\n'
    printf 'a tab looks like with wsl-bridge off.\n'
    exit 0
fi

printf 'The two disagree, which neither path produces on its own.\n'
printf '  started through the bridge: %s\n' "$bridge_found"
printf '  stream reaches the terminal: %s\n' "$([ "$stream" = bridge ] && echo yes || echo no)"
exit 1
