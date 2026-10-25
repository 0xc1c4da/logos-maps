#!/usr/bin/env bash
# maps end-to-end snapshot test (docs/architecture.md §16.7, full run).
#
# The core half (core/tests/logosctl/smoke.sh) checks the §12.1 contract
# without a view. This script adds the view: maps_core runs in an isolated
# logosctl session, maps_view_dev renders headless (llvmpipe under Xvfb, the
# same recipe as the §16.1 suites), a raster basemap is served from local
# files, a scene is published through applyBatch, the camera is placed by
# intent, and `snapshot` produces a PNG that is compared against a golden
# image. Everything crosses the real maps_core API through logosctl; nothing
# is mocked. Fonts are avoided (no symbol layers) so the golden does not
# depend on the host's text stack.
#
# Usage:
#   view/tests/logosctl/e2e.sh                # builds everything it needs
#   MAPS_E2E_UPDATE_GOLDEN=1 view/tests/logosctl/e2e.sh   # (re)record the golden
#
# Environment:
#   MAPS_CORE_LGX     prebuilt maps_core package (default: nix build core#lgx)
#   MAPS_VIEW_DEV     maps-view-dev output dir (default: nix build view#dev)
#   LOGOSCTL          logosctl binary (default: nix build logos-logoscore-cli#ctl)
#   LOGOSCTL_FLAKE    flake ref for logosctl (default: github:logos-co/logos-logoscore-cli)
#   BUILDER_OVERRIDE  extra nix args for the core/view builds, e.g.
#                     "--override-input logos-module-builder path:../logos-module-builder"
#   MAPS_E2E_TOLERANCE  max fraction of pixels allowed to differ (default 0.005)
#   MAPS_E2E_FUZZ       per-channel fuzz for the pixel comparison (default 6%)
#   KEEP_SESSION=1    leave the session directory behind for inspection
#
# Needs: jq, and ImageMagick's `compare` (fetched through `nix shell` when
# absent).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
view="$(cd "$here/../.." && pwd)"
core="$(cd "$view/../core" && pwd)"
golden="$view/tests/fixtures/e2e/golden.png"

log()  { printf '\033[1;34m[e2e]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[e2e] FAIL\033[0m %s\n' "$*" >&2; dump_logs; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing tool: $1"; }

dump_logs() {
    [[ -n "${session:-}" ]] || return 0
    if [[ -s "$session/view.log" ]]; then
        echo "--- view.log (tail) ---" >&2
        tail -n 40 "$session/view.log" >&2
    fi
    if [[ -s "$session/events.jsonl" ]]; then
        echo "--- events (tail) ---" >&2
        tail -n 20 "$session/events.jsonl" >&2
    fi
}
need jq

# ── artefacts ─────────────────────────────────────────────────────────────
if [[ -z "${MAPS_CORE_LGX:-}" ]]; then
    log "building maps_core .lgx"
    # shellcheck disable=SC2086
    nix build "path:$core#lgx" --out-link "$here/.result-lgx" ${BUILDER_OVERRIDE:-}
    MAPS_CORE_LGX="$(ls "$here"/.result-lgx/*.lgx | head -1)"
fi
[[ -f "$MAPS_CORE_LGX" ]] || fail "no .lgx at $MAPS_CORE_LGX"

if [[ -z "${MAPS_VIEW_DEV:-}" ]]; then
    log "building maps_view_dev"
    # shellcheck disable=SC2086
    nix build "path:$view#dev" --out-link "$here/.result-view-dev" ${BUILDER_OVERRIDE:-}
    MAPS_VIEW_DEV="$here/.result-view-dev"
fi
view_dev="$MAPS_VIEW_DEV/bin/maps_view_dev_headless"
[[ -x "$view_dev" ]] || fail "no maps_view_dev_headless under $MAPS_VIEW_DEV"

if [[ -z "${LOGOSCTL:-}" ]]; then
    log "building logosctl"
    nix build "${LOGOSCTL_FLAKE:-github:logos-co/logos-logoscore-cli}#ctl" --out-link "$here/.result-logosctl"
    LOGOSCTL="$here/.result-logosctl/bin/logosctl"
fi
[[ -x "$LOGOSCTL" ]] || fail "no logosctl at $LOGOSCTL"

if command -v compare >/dev/null 2>&1; then
    compare_png() { compare "$@"; }
else
    compare_png() { nix shell nixpkgs#imagemagick -c compare "$@"; }
fi

# ── isolated session (absolute paths; never rm -rf through $HOME) ─────────
session="$(mktemp -d /tmp/maps-e2e.XXXXXX)"
mkdir -p "$session/home" "$session/cfg" "$session/tiles"
export HOME="$session/home" LOGOSCTL_CONFIG_DIR="$session/cfg" QT_QPA_PLATFORM=offscreen
ctl() { timeout 60 "$LOGOSCTL" "$@"; }
view_pid=""
watch_pid=""
cleanup() {
    # The headless wrapper is xvfb-run -> Xvfb + maps_view_dev; it runs in its
    # own process group (setsid) so the whole tree can be stopped at once.
    [[ -n "$view_pid" ]] && { kill -- "-$view_pid" 2>/dev/null || kill "$view_pid" 2>/dev/null; wait "$view_pid" 2>/dev/null; } || true
    [[ -n "$watch_pid" ]] && kill "$watch_pid" 2>/dev/null || true
    ctl daemon stop >/dev/null 2>&1 || true
    if [[ "${KEEP_SESSION:-0}" == 1 ]]; then log "session kept at $session"; else rm -rf "/tmp/${session#/tmp/}"; fi
}
trap cleanup EXIT

log "session $session"
ctl daemon start --detach >/dev/null
ctl install "$MAPS_CORE_LGX" -y >/dev/null
ctl module load maps_core >/dev/null
sleep 1

# Every event maps_core emits, one JSON object per line.
"$LOGOSCTL" watch maps_core >"$session/events.jsonl" 2>/dev/null &
watch_pid=$!

call() {
    local out; out="$(ctl call maps_core "$@" | tail -n 1)"
    jq -e '.status == "ok"' <<<"$out" >/dev/null || fail "$1: $(jq -r '.message // .' <<<"$out")"
    jq -c '.result' <<<"$out"
}
text() { call "$@" | jq -c 'fromjson'; }
value() {
    local out; out="$(call "$@")"
    jq -e '.success == true' <<<"$out" >/dev/null || fail "$1 failed: $(jq -r '.error' <<<"$out")"
    jq -c '.value' <<<"$out"
}
# wait_for <seconds> <description> <command...>: poll until the command succeeds.
wait_for() {
    local secs="$1" what="$2"; shift 2
    local i
    for ((i = 0; i < secs * 4; i++)); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.25
    done
    fail "timed out waiting for $what"
}
# event_seen <topic> [<viewEvent topic>]: has the watch log recorded it?
event_seen() {
    local topic="$1" inner="${2:-}"
    if [[ -z "$inner" ]]; then
        grep -q "\"event\":\"$topic\"" "$session/events.jsonl"
    else
        jq -e --arg t "$topic" --arg i "$inner" \
            'select(.event == $t) | (.data.arg0 | if type == "string" then fromjson else . end) | select(.topic == $i)' \
            "$session/events.jsonl" 2>/dev/null | grep -q .
    fi
}

# ── 1. a basemap served from local files ──────────────────────────────────
# A checkerboard of two 512 px fixture tiles for zoom 0..3 (the golden is
# taken at zoom 1: two by two tiles meeting at the origin), so misplaced or
# missing tiles show up in the comparison.
for z in 0 1 2 3; do
    n=$((1 << z))
    for ((x = 0; x < n; x++)); do
        mkdir -p "$session/tiles/$z/$x"
        for ((y = 0; y < n; y++)); do
            if (( (x + y) % 2 == 0 )); then src=tile-light.png; else src=tile-dark.png; fi
            cp "$view/tests/fixtures/e2e/$src" "$session/tiles/$z/$x/$y.png"
        done
    done
done
cat >"$session/basemap.json" <<EOF
{"v":1,"id":"e2e/checker","name":"E2E checkerboard","format":"raster",
 "tile_url":"file://$session/tiles/{z}/{x}/{y}.png","tile_size":512,
 "provider":"e2e","profiles":["offline"],"zoom":[0,3],
 "attribution":{"e2e fixtures":"file://$view/tests/fixtures/e2e"}}
EOF
added="$(value addBasemap "@$session/basemap.json")"
[[ "$(jq -r .id <<<"$added")" == "e2e/checker" ]] || fail "addBasemap: $added"

# ── 2. the view comes up against the core ─────────────────────────────────
log "starting maps_view_dev (headless)"
setsid "$view_dev" --logosctl "$LOGOSCTL" --view-session e2e --size 512x384 >"$session/view.log" 2>&1 &
view_pid=$!
view_id="maps_view/e2e/1"
has_view() { text listViews | jq -e --arg v "$view_id" '.views[] | select(.viewId == $v)'; }
wait_for 60 "the view to open" has_view
log "view $view_id is open"

# ── 3. basemap, scene, camera ─────────────────────────────────────────────
value setBasemap "str:$view_id" str:e2e/checker >/dev/null
wait_for 30 "the basemap to load" event_seen viewEvent basemapLoaded

cat >"$session/batch.json" <<'EOF'
{"v":1,"ops":[
 {"op":"putSource","id":"marks","type":"geojson","data":{"type":"FeatureCollection","features":[
   {"type":"Feature","id":1,"geometry":{"type":"Point","coordinates":[0,0]},"properties":{"kind":"origin"}},
   {"type":"Feature","id":2,"geometry":{"type":"Point","coordinates":[45,30]},"properties":{"kind":"ne"}},
   {"type":"Feature","id":3,"geometry":{"type":"Point","coordinates":[-60,-20]},"properties":{"kind":"sw"}}]}},
 {"op":"putSource","id":"box","type":"geojson","data":{"type":"FeatureCollection","features":[
   {"type":"Feature","id":1,"geometry":{"type":"Polygon","coordinates":[[[-30,-15],[30,-15],[30,15],[-30,15],[-30,-15]]]},"properties":{}}]}},
 {"op":"putLayer","id":"box-fill","type":"fill","source":"box","paint":{"fill-color":"#e07a3f","fill-opacity":0.55}},
 {"op":"putLayer","id":"box-line","type":"line","source":"box","paint":{"line-color":"#7a2d0c","line-width":3}},
 {"op":"putLayer","id":"dots","type":"circle","source":"marks","paint":{"circle-radius":9,"circle-color":"#1f4e8c","circle-stroke-width":2,"circle-stroke-color":"#ffffff"}}
]}
EOF
r="$(value applyBatch str:e2e/scene "@$session/batch.json")"
[[ "$(jq -r .revision <<<"$r")" == 1 ]] || fail "expected revision 1: $r"
[[ "$(jq -r '.rejected|length' <<<"$r")" == 0 ]] || fail "unexpected rejections: $r"
applied() { text listViews | jq -e --arg v "$view_id" '.views[] | select(.viewId == $v and .appliedRevision == 1)'; }
wait_for 30 "the view to apply revision 1" applied
event_seen revisionApplied || fail "revisionApplied was not emitted"

value camera "str:$view_id" str:'{"type":"jumpTo","center":[0,0],"zoom":1,"bearing":0,"pitch":0}' >/dev/null
at_target() {
    value getViewState "str:$view_id" | jq -e '.camera.zoom == 1 and (.camera.center[0]|fabs) < 0.001 and (.camera.center[1]|fabs) < 0.001'
}
wait_for 30 "the camera to reach zoom 1 over 0,0" at_target

# ── 4. snapshot ───────────────────────────────────────────────────────────
out_png="$session/snapshot.png"
req="$(value snapshot "str:$view_id" "str:{\"path\":\"$out_png\"}")"
request_id="$(jq -r .requestId <<<"$req")"
[[ -n "$request_id" && "$request_id" != null ]] || fail "snapshot gave no requestId: $req"
snapshot_ready() {
    jq -e --arg r "$request_id" \
        'select(.event == "viewEvent") | (.data.arg0 | if type == "string" then fromjson else . end) | select(.topic == "snapshotReady") | .payload | select(.requestId == $r)' \
        "$session/events.jsonl" 2>/dev/null | grep -q .
}
wait_for 30 "snapshotReady for $request_id" snapshot_ready
ready="$(jq -c --arg r "$request_id" \
    'select(.event == "viewEvent") | (.data.arg0 | if type == "string" then fromjson else . end) | select(.topic == "snapshotReady") | .payload | select(.requestId == $r)' \
    "$session/events.jsonl" | head -1)"
[[ "$(jq -r '.error // empty' <<<"$ready")" == "" ]] || fail "snapshot failed: $ready"
[[ "$(jq -r .bytesRef <<<"$ready")" == "file://$out_png" ]] || fail "bytesRef points elsewhere: $ready"
[[ -s "$out_png" ]] || fail "no PNG at $out_png"
log "snapshot $(jq -r '"\(.width)x\(.height)"' <<<"$ready") at $out_png"

# ── 5. golden comparison ──────────────────────────────────────────────────
if [[ "${MAPS_E2E_UPDATE_GOLDEN:-0}" == 1 || ! -f "$golden" ]]; then
    cp "$out_png" "$golden"
    log "golden recorded at $golden (commit it)"
    log "OK: maps end-to-end run completed (golden updated)"
    exit 0
fi

tolerance="${MAPS_E2E_TOLERANCE:-0.005}"
fuzz="${MAPS_E2E_FUZZ:-6%}"
dims="$(jq -r '"\(.width)x\(.height)"' <<<"$ready")"
# `compare -metric AE` prints the count of differing pixels on stderr and
# exits 1 when it is non-zero; the diff image is kept for inspection.
differing="$(compare_png -metric AE -fuzz "$fuzz" "$golden" "$out_png" "$session/diff.png" 2>&1 >/dev/null || true)"
# The count is printed as "N" or "N (fraction)"; keep the integer part only.
[[ "$differing" =~ ^[0-9]+ ]] || fail "compare could not read the images: $differing"
differing="${BASH_REMATCH[0]}"
total=$(( ${dims%x*} * ${dims#*x} ))
ratio="$(jq -n --argjson d "$differing" --argjson t "$total" '$d / $t')"
if jq -e --argjson r "$ratio" --argjson tol "$tolerance" '$r > $tol' <<<null >/dev/null; then
    [[ "${KEEP_SESSION:-0}" == 1 ]] || { cp "$out_png" /tmp/maps-e2e-actual.png; cp "$session/diff.png" /tmp/maps-e2e-diff.png 2>/dev/null || true; }
    fail "snapshot differs from golden: $differing of $total pixels ($ratio > $tolerance); actual at /tmp/maps-e2e-actual.png, diff at /tmp/maps-e2e-diff.png"
fi

log "OK: maps end-to-end snapshot matches golden ($differing of $total pixels differ, tolerance $tolerance)"
