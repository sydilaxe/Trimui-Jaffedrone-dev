#!/bin/sh
# HELP: Jaffedrone: a five-string drone instrument. MENU quits.
# ICON: jaffedrone
# GRID: Jaffedrone

# MustardOS starts an app with this script, passing the app's folder. It sets up
# the app the way MustardOS's own apps do, then runs launch.sh, which is shared
# with the TrimUI OS package.

APP_DIR="${1:-$(dirname "$0")}"

if [ -f /opt/muos/script/var/retro.sh ]; then
    . /opt/muos/script/var/retro.sh
    command -v SETUP_APP >/dev/null 2>&1 && SETUP_APP jaffedrone ""
elif [ -f /opt/muos/script/var/func.sh ]; then
    # older muOS releases
    . /opt/muos/script/var/func.sh
    echo app >/tmp/act_go
    command -v SETUP_SDL_ENVIRONMENT >/dev/null 2>&1 && SETUP_SDL_ENVIRONMENT
    command -v SET_VAR >/dev/null 2>&1 && SET_VAR "system" "foreground_process" "jaffedrone"
fi

cd "$APP_DIR" || exit 1
# the screen can stay dark once the app's window opens: the app runs this then
export JD_SCREEN_WAKE="$PWD/wake_screen.sh"
./launch.sh
