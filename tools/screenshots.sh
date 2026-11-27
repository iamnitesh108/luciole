#!/usr/bin/env bash
# Regenerate the user-guide screenshots in docs/user/assets/.
#
# Usage: tools/screenshots.sh [luciole-binary]
#
# Runs the binary inside a nested Umbriel session with a throwaway home, config
# and D-Bus session, so nothing of the user's desktop or settings is touched.
# Shots use the Ayu dark palette (tools/screenshots/ayu-dark.json), rendered into
# luciole's colors.toml by `noctalia theme`, so they stand out from the docs site's
# own Noctalia palette. Needs: noctalia, umbriel, grim, wtype, wlrctl, magick, cwebp,
# and a running Wayland session to nest in. ICON_THEME overrides Rose-Pine-Plus-Dark.
# Timing based: raise PAUSE if a shot comes out half drawn.
set -euo pipefail

bin=${1:-build-debug/luciole}
PAUSE=${PAUSE:-2}
# Window corner radius in output pixels.
RADIUS=${RADIUS:-7}

if [[ ${SHOTS_INNER:-} ]]; then
  # Inside the nested compositor: WAYLAND_DISPLAY is the nested socket.
  # shellcheck disable=SC2329
  press() { wtype -d 80 "$@"; }
  shot() { # name; crop to the window's geometry (global layout) on the capture output
    local try geom size x y outx
    for try in 1 2 3 4 5; do
      sleep "$PAUSE"
      geom=$(umbriel windows | grep -o '[0-9]*x[0-9]*+[0-9]*+[0-9]*' | head -1)
      [[ ${geom%%x*} -gt 300 ]] 2>/dev/null && break # a window that is still opening reports a tiny size
      [[ $try -lt 5 ]] || { echo "$1: no window found, raise PAUSE" >&2; touch "$SHOTS_TMP/failed"; exit 1; }
    done
    outx=$(umbriel outputs | awk '/^shots/ {f = 1} f && /Position:/ {split($2, a, ","); print a[1]; exit}')
    size=${geom%%+*}
    IFS=+ read -r _ x y <<<"$geom"
    grim -o shots "$SHOTS_TMP/raw.png"
    # Round the corners with transparency so the nested wallpaper does not show behind them.
    magick "$SHOTS_TMP/raw.png" -crop "$size+$((x - outx))+$y" +repage -alpha set \
      \( +clone -channel A -evaluate set 0 +channel -fill white \
      -draw "roundrectangle 0,0 $((${size%%x*} - 1)),$((${size##*x} - 1)) $RADIUS,$RADIUS" \) \
      -compose DstIn -composite "$SHOTS_TMP/$1.png"
    cwebp -quiet -lossless -z 9 "$SHOTS_TMP/$1.png" -o "$SHOTS_OUT/$1.webp"
    echo "wrote $SHOTS_OUT/$1.webp"
  }
  start() { # folder
    pkill -x luciole 2>/dev/null || true
    sleep 1 # let the previous instance release its D-Bus name
    "$SHOTS_BIN" "$1" &
    for _ in $(seq 50); do
      umbriel windows | grep -q . && break
      sleep 0.2
    done
    sleep "$PAUSE"
  }

  # A fixed-size virtual output makes every shot the same size; windows open on the focused output.
  umbriel output-create shots "${OUTPUT:-1280x800@60}"
  sleep 1 # the new output must exist before it can take focus
  umbriel msg output-focus-right
  trap 'pkill -x luciole; kill "$PPID"' EXIT # always end the nested compositor, even on failure
  pictures=$HOME/Pictures

  start "$pictures"
  press -k Down -k Down -k Down -k Down
  press -M ctrl -k 1 -m ctrl
  shot views-grid
  shot index-hero
  press -M ctrl -k 2 -m ctrl
  shot views-list
  press -M ctrl -k 3 -m ctrl
  shot views-columns

  start "$HOME"
  press -M ctrl -k 2 -m ctrl
  shot getting-started-window

  start "$pictures"
  press -M ctrl -k 2 -m ctrl
  press -k F3
  shot tabs-split-view

  start "$pictures"
  press -k Down -k Down
  shot preview-side

  start "$HOME"
  press -k Escape # dismiss the tooltip under the resting pointer
  shot sidebar-places

  start "$HOME"
  press -M ctrl -k comma -m ctrl
  shot settings

  exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/docs/user/assets
bin=$(realpath "$bin")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out" "$tmp"/{home,config/luciole,cache,data,state,run}
chmod 700 "$tmp/run"
home=$tmp/home

# Fixture: public-domain paintings (see tools/screenshots/pictures/README.md), a few folders and files.
mkdir -p "$home"/{Desktop,Documents,Downloads,Music,Videos,Pictures/Holidays,Projects/{website,notes},Reports}
cp "$root"/tools/screenshots/pictures/*.jpg "$home/Pictures/"
for f in Documents/notes.txt Documents/budget.csv Downloads/archive.zip; do echo "$f" >"$home/$f"; done
cat >"$tmp/config/user-dirs.dirs" <<EOF
XDG_DESKTOP_DIR="\$HOME/Desktop"
XDG_DOCUMENTS_DIR="\$HOME/Documents"
XDG_DOWNLOAD_DIR="\$HOME/Downloads"
XDG_MUSIC_DIR="\$HOME/Music"
XDG_PICTURES_DIR="\$HOME/Pictures"
XDG_VIDEOS_DIR="\$HOME/Videos"
EOF
mkdir -p "$tmp/config/gtk-3.0"
printf 'file://%s/Projects Projects\nfile://%s/Reports Reports\n' "$home" "$home" >"$tmp/config/gtk-3.0/bookmarks"
printf '[browser]\nview_mode = "grid"\n' >"$tmp/config/luciole/config.toml"

# Render luciole/colors.toml from the Ayu dark palette into the throwaway config.
HOME=$home XDG_CONFIG_HOME="$tmp/config" XDG_DATA_HOME="$tmp/data" XDG_STATE_HOME="$tmp/state" \
  XDG_CACHE_HOME="$tmp/cache" noctalia theme --theme-json "$root/tools/screenshots/ayu-dark.json" \
  --builtin-config --default-mode dark >/dev/null 2>&1
[[ -f $tmp/config/luciole/colors.toml ]] || { echo "noctalia theme did not write luciole/colors.toml" >&2; exit 1; }

# The icon theme comes from the desktop settings (GSettings, here a plain key file); the theme
# itself is found through the real data dir.
real_data=${XDG_DATA_HOME:-$HOME/.local/share}
icons=${ICON_THEME:-Rose-Pine-Plus-Dark}
if [[ -d $real_data/icons/$icons || -d /usr/share/icons/$icons ]]; then
  mkdir -p "$tmp/config/glib-2.0/settings"
  printf "[org/gnome/desktop/interface]\nicon-theme='%s'\ncolor-scheme='prefer-dark'\n" "$icons" >"$tmp/config/glib-2.0/settings/keyfile"
fi

printf '[general]\nshow_cheatsheet = false\n' >"$tmp/umbriel.toml"

parent=${WAYLAND_DISPLAY:?run inside a Wayland session}
[[ $parent == /* ]] || parent=${XDG_RUNTIME_DIR:?}/$parent

env -i PATH="$PATH" WAYLAND_DISPLAY="$parent" HOME="$home" \
  XDG_CONFIG_HOME="$tmp/config" XDG_CACHE_HOME="$tmp/cache" XDG_DATA_HOME="$tmp/data" \
  XDG_STATE_HOME="$tmp/state" XDG_RUNTIME_DIR="$tmp/run" \
  XDG_DATA_DIRS="$real_data:/usr/local/share:/usr/share" GSETTINGS_BACKEND=keyfile GVFS_DISABLE_FUSE=1 \
  SHOTS_INNER=1 SHOTS_TMP="$tmp" SHOTS_OUT="$out" SHOTS_BIN="$bin" \
  PAUSE="$PAUSE" RADIUS="$RADIUS" \
  dbus-run-session -- bash -c '
    umbriel -c "'"$tmp"'/umbriel.toml" -s "'"$0"'"
  ' || true # killed by the driver on exit, so its status says nothing
# The nested compositor's exit status does not carry the driver's, so check the marker.
[[ ! -e $tmp/failed ]]
