#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Builds and installs Network Health for the current user. No root required.
#
# Distribution packagers want the CMake build directly instead:
#   cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build
#   DESTDIR="$pkgdir" cmake --install build
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-$HOME/.local}
build=${BUILD_DIR:-$here/build}

if [ -x "$here/bin/plasma-network-healthd" ]; then
    # Release tarball: the backend is already built, so no compiler is needed.
    printf 'Installing the prebuilt backend into %s/bin...\n' "$prefix"
    mkdir -p "$prefix/bin"
    install -m 0755 "$here/bin/plasma-network-healthd" "$prefix/bin/plasma-network-healthd"
else
    printf 'Building the backend...\n'
    # The widget is installed with kpackagetool6 below so that Plasma picks it
    # up straight away, and the systemd unit stays opt-in for a home install.
    cmake -S "$here" -B "$build" \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DINSTALL_PLASMOID=OFF \
        -DINSTALL_SYSTEMD_UNIT=OFF
    cmake --build "$build" --parallel

    printf 'Installing the backend into %s/bin...\n' "$prefix"
    cmake --install "$build"
fi

printf 'Installing the widget...\n'
if kpackagetool6 --type Plasma/Applet --list 2>/dev/null | grep -q '^com.github.atmanactive.networkhealth$'; then
    kpackagetool6 --type Plasma/Applet --upgrade "$here/package"
else
    kpackagetool6 --type Plasma/Applet --install "$here/package"
fi

# An older backend would keep serving the previous state format after an upgrade.
runtime="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -un)}/plasma-network-health"
pid=$(cat "$runtime/daemon.lock" 2>/dev/null || true)
if [ -n "${pid:-}" ] && [ -e "/proc/$pid" ]; then
    kill "$pid" 2>/dev/null || true
fi

cat <<'MESSAGE'

Done.

  * Right-click the desktop or a panel -> Add Widgets -> "Network Health".
  * For the system tray: right-click the tray -> Configure System Tray ->
    Entries -> set "Network Health" to "Shown".

The backend starts by itself and stops again once no widget needs it.
MESSAGE

if [ "$(id -u)" -ne 0 ] && [ -r /proc/sys/net/ipv4/ping_group_range ]; then
    range=$(cat /proc/sys/net/ipv4/ping_group_range)
    low=${range%%[!0-9]*}
    high=${range##*[!0-9]}
    gid=$(id -g)
    if [ "$gid" -lt "$low" ] || [ "$gid" -gt "$high" ]; then
        cat <<MESSAGE

Warning: unprivileged ICMP is disabled on this system
(net.ipv4.ping_group_range is "$range", your group id is $gid).

Either allow it:
    echo 'net.ipv4.ping_group_range = 0 2147483647' | sudo tee /etc/sysctl.d/99-ping-group-range.conf
    sudo sysctl --system

or grant the backend the raw-socket capability:
    sudo setcap cap_net_raw+ep $prefix/bin/plasma-network-healthd
MESSAGE
    fi
fi
