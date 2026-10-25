#!/usr/bin/env bash
# maps_core headless smoke test (docs/architecture.md §16.7, core half).
#
# Installs the maps_core .lgx into an isolated logosctl session, loads it,
# publishes a scene through applyBatch and checks that the published
# revision, the scene, the change log, the catalogue and the capability
# report come back as the §12.1 contract says. No view process is involved;
# the full §16.7 run (with maps_view rendering a snapshot) lives in M7.
#
# Usage:
#   core/tests/logosctl/smoke.sh                 # builds .lgx and logosctl itself
#   MAPS_CORE_LGX=/path/to/maps_core.lgx \
#   LOGOSCTL=/path/to/logosctl core/tests/logosctl/smoke.sh
#
# Environment:
#   MAPS_CORE_LGX   prebuilt package (default: nix build core#lgx)
#   LOGOSCTL        logosctl binary (default: nix build logos-logoscore-cli#ctl)
#   LOGOSCTL_FLAKE  flake ref for logosctl (default: github:logos-co/logos-logoscore-cli)
#   BUILDER_OVERRIDE  extra nix args, e.g. "--override-input logos-module-builder path:../logos-module-builder"
#   KEEP_SESSION=1  leave the session directory behind for inspection
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
core="$(cd "$here/../.." && pwd)"

log()  { printf '\033[1;34m[smoke]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[smoke] FAIL\033[0m %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing tool: $1"; }
need jq

# ── artefacts ─────────────────────────────────────────────────────────────
if [[ -z "${MAPS_CORE_LGX:-}" ]]; then
    log "building maps_core .lgx"
    # shellcheck disable=SC2086
    nix build "path:$core#lgx" --out-link "$here/.result-lgx" ${BUILDER_OVERRIDE:-}
    MAPS_CORE_LGX="$(ls "$here"/.result-lgx/*.lgx | head -1)"
fi
[[ -f "$MAPS_CORE_LGX" ]] || fail "no .lgx at $MAPS_CORE_LGX"

if [[ -z "${LOGOSCTL:-}" ]]; then
    log "building logosctl"
    nix build "${LOGOSCTL_FLAKE:-github:logos-co/logos-logoscore-cli}#ctl" --out-link "$here/.result-logosctl"
    LOGOSCTL="$here/.result-logosctl/bin/logosctl"
fi
[[ -x "$LOGOSCTL" ]] || fail "no logosctl at $LOGOSCTL"

# ── isolated session (absolute paths; never rm -rf through $HOME) ─────────
session="$(mktemp -d /tmp/maps-smoke.XXXXXX)"
mkdir -p "$session/home" "$session/cfg"
export HOME="$session/home" LOGOSCTL_CONFIG_DIR="$session/cfg" QT_QPA_PLATFORM=offscreen
ctl() { timeout 60 "$LOGOSCTL" "$@"; }
cleanup() {
    ctl daemon stop >/dev/null 2>&1 || true
    if [[ "${KEEP_SESSION:-0}" == 1 ]]; then log "session kept at $session"; else rm -rf "/tmp/${session#/tmp/}"; fi
}
trap cleanup EXIT

log "session $session"
ctl daemon start --detach >/dev/null
ctl install "$MAPS_CORE_LGX" -y >/dev/null
ctl module load maps_core >/dev/null
sleep 1

# `call` prints one JSON object when piped:
#   {"status":"ok","module":"maps_core","method":"...","result":<value>}
# `result` is the decoded return value: a JSON object for LIDL `result`
# (StdLogosResult), a plain string for `std::string`, a bool for `bool`.
call() {
    local out; out="$(ctl call maps_core "$@" | tail -n 1)"
    jq -e '.status == "ok"' <<<"$out" >/dev/null || fail "$1: $(jq -r '.message // .' <<<"$out")"
    jq -c '.result' <<<"$out"
}
# std::string methods that return JSON text: decode the inner document.
text() { call "$@" | jq -c 'fromjson'; }
# StdLogosResult arrives as {"success","value","error"}; unwrap or fail.
value() {
    local out; out="$(call "$@")"
    jq -e '.success == true' <<<"$out" >/dev/null || fail "$1 failed: $(jq -r '.error' <<<"$out")"
    jq -c '.value' <<<"$out"
}

# ── 1. schema and empty scene ─────────────────────────────────────────────
[[ "$(call schemaVersion | jq -r .)" == "maps/1" ]] || fail "schemaVersion"
rev0="$(value getScene | jq -r .revision)"
[[ "$rev0" == 0 ]] || fail "fresh scene should be at revision 0, got $rev0"

# ── 2. applyBatch publishes a revision ────────────────────────────────────
cat >"$session/batch.json" <<'EOF'
{"v":1,"ops":[
 {"op":"putSource","id":"pins","type":"geojson","data":{"type":"FeatureCollection","features":[
   {"type":"Feature","id":1,"geometry":{"type":"Point","coordinates":[13.405,52.52]},"properties":{"name":"Berlin"}}]}},
 {"op":"putLayer","id":"dots","type":"circle","source":"pins","paint":{"circle-radius":6},"placement":{"anchor":"labels"}},
 {"op":"putLayer","id":"bad","type":"circle","source":"missing"}
]}
EOF
r="$(value applyBatch str:demo/pins "@$session/batch.json")"
[[ "$(jq -r .revision <<<"$r")" == 1 ]] || fail "expected revision 1: $r"
[[ "$(jq -r '.accepted|length' <<<"$r")" == 2 ]] || fail "expected 2 accepted: $r"
[[ "$(jq -r '.rejected[0].code' <<<"$r")" == "maps.unknownId" ]] || fail "expected per-op rejection: $r"

# Identity gate: the same batch again publishes nothing.
r2="$(value applyBatch str:demo/pins "@$session/batch.json")"
[[ "$(jq -r .published <<<"$r2")" == false ]] || fail "rebatch should not publish: $r2"
[[ "$(jq -r .revision <<<"$r2")" == 1 ]] || fail "revision moved on a no-op: $r2"

# ── 3. scene, changes, order ──────────────────────────────────────────────
scene="$(value getScene str:demo/pins)"
[[ "$(jq -r '.layers[0].id' <<<"$scene")" == "demo/pins:dots" ]] || fail "scene layer id: $scene"
changes="$(value getChanges 0)"
[[ "$(jq -r '[.ops[].op]|join(",")' <<<"$changes")" == "putSource,replaceGeoJson,putLayer" ]] || fail "change log order: $changes"

# ── 4. a view registers, acks, and gets its layer order ──────────────────
v="$(value openView str:smoke/v1 str:token str:'{"render":{"terrain":false},"surface":{"transport":"none"}}')"
[[ "$(jq -r .basemap <<<"$v")" == "openfreemap/bright" ]] || fail "default basemap: $v"
order="$(value getLayerOrder str:smoke/v1)"
[[ "$(jq -r '.layers[0].before' <<<"$order")" == "waterway_line_label" ]] || fail "labels anchor on bright: $order"
[[ "$(call ackRevision str:smoke/v1 1)" == true ]] || fail "ackRevision"
[[ "$(text listViews | jq -r '.views[0].appliedRevision')" == 1 ]] || fail "applied revision not recorded"
value setBasemap str:smoke/v1 str:osm/standard >/dev/null
[[ "$(value getLayerOrder str:smoke/v1 | jq -r '.layers[0].before')" == "" ]] || fail "raster anchors resolve to top"

# ── 5. catalogue and capabilities ─────────────────────────────────────────
n="$(text listBasemaps str:'{}' | jq '.basemaps|length')"
[[ "$n" -ge 3 ]] || fail "bundled catalogue too small: $n"
caps="$(value getCapabilities)"
[[ "$(jq -r '.scene.patchFeatures' <<<"$caps")" == true ]] || fail "capabilities: $caps"

# ── 6. errors are coded, not exceptions ──────────────────────────────────
err="$(call applyBatch str:nosession str:'[]')"
[[ "$(jq -r .success <<<"$err")" == false && "$(jq -r .error <<<"$err")" == maps.badArgument:* ]] || fail "coded error: $err"

# ── 7. drop ───────────────────────────────────────────────────────────────
[[ "$(call dropSession str:demo/pins)" == true ]] || fail "dropSession"
[[ "$(value getScene | jq -r '.sessions|length')" == 0 ]] || fail "scene not empty after drop"

log "OK: maps_core headless smoke passed"
