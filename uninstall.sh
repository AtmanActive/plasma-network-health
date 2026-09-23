#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

prefix=${PREFIX:-$HOME/.local}

systemctl --user disable --now plasma-network-healthd.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/plasma-network-healthd.service"
runtime="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -un)}/plasma-network-health"
pid=$(cat "$runtime/daemon.lock" 2>/dev/null || true)
if [ -n "${pid:-}" ] && [ -e "/proc/$pid" ]; then
    kill "$pid" 2>/dev/null || true
fi

kpackagetool6 --type Plasma/Applet --remove com.github.atmanactive.networkhealth 2>/dev/null || true
rm -f "$prefix/bin/plasma-network-healthd"
rm -rf "${XDG_RUNTIME_DIR:-/tmp}/plasma-network-health"

printf 'Removed.\n'
printf 'Your destinations and appearance are kept in %s\n' "$HOME/.config/plasma-network-health"
printf 'Delete that directory too if you want nothing left behind.\n'
