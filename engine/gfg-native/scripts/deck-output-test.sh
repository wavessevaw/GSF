#!/usr/bin/env bash
# GFG Output Engine SteamOS qualification. Isolated single-child Vulkan layer.
# No sudo, no rootfs changes, no Steam/Decky/plugin configuration changes.
set -Eeuo pipefail
umask 077

SELF_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODE=direct
PHASE=both
FRAMES=120
ACTION=""
WINDOW_WIDTH=1280
WINDOW_HEIGHT=800
usage() {
  cat <<'HELP'
GFG Output Engine: experimental Deck hardware qualification

  bash deck-output-test.sh --check
  bash deck-output-test.sh --run --mode=direct --phase=both --frames=120
  bash deck-output-test.sh --run --mode=gaming --phase=output --frames=120
  bash deck-output-test.sh --run --mode=nested --phase=both --frames=120

--check               Verify the bundle, linked libraries and checksums only.
--run                 Explicitly permit running an isolated test client.
--mode=direct         Desktop X11/XWayland: verify pixels using XCB GetImage.
--mode=gaming         SteamOS Gaming Mode: skip INVALID XCB backing readback
                      under Gamescope WSI, track GPU/Vulkan output submissions
                      separately as UNVERIFIED on physical OLED. Use this
                      mode as the Non-Steam Game; no system modifications.
--mode=nested         Launch a *new* nested gamescope in Desktop Mode; only
                      the client (never Gamescope) receives the test layer.
--phase=both          Unmodified Vulkan baseline (8 frames), then output.
--phase=output        Only the opt-in experimental output test.
--frames=120|8        Run 120 originals + 119 A/B splits (or 8+7).

Output data stays in ~/GFG-Output-Deck-Tests/. No system changes.
A successful app log is NOT proof of actual OLED scanout; use external
high-frame-rate camera video of the screen for hardware verification.
HELP
}
fail(){ printf 'GSF_DECK_KIT_FAIL: %s\n' "$*" >&2;exit 1;}
for arg in "$@"; do
  case "$arg" in
    --check) ACTION=check;;
    --run) ACTION=run;;
    --mode=direct) MODE=direct;;
    --mode=gaming) MODE=gaming;;
    --mode=nested) MODE=nested;;
    --phase=both) PHASE=both;;
    --phase=output) PHASE=output;;
    --frames=8) FRAMES=8;;
    --frames=120) FRAMES=120;;
    --help|-h) usage;exit 0;;
    *) fail "Unsupported option: $arg";;
  esac
done
[[ -n "$ACTION" ]] || { usage; fail "Pass --check or --run explicitly"; }
[[ "$(uname -m)" == x86_64 ]] || fail "Only x86_64 AMD64 binaries are packaged."
command -v sha256sum >/dev/null || fail "sha256sum missing"
command -v timeout >/dev/null || fail "timeout missing"
[[ -r "$SELF_DIR/SHA256SUMS" ]] || fail "SHA256SUMS missing; use whole GitHub Actions bundle"
for name in libVkLayer_gfg_native.so VkLayer_gfg_native.json loader-live-output; do
  [[ -f "$SELF_DIR/$name" ]] || fail "Missing $name"
done
(
  cd "$SELF_DIR"
  sha256sum -c SHA256SUMS
) || fail "Checksum mismatch: abort before loading any code"
[[ -r "$SELF_DIR/libVkLayer_gfg_native.so" ]] || fail "Layer not readable"
# GitHub Actions ZIPs normalize file permissions to 0644. Restore only the
# execute bit of our checksum-verified local test binary; never touch SteamOS,
# system Vulkan layers, Decky or Steam files.
if [[ ! -x "$SELF_DIR/loader-live-output" ]]; then
  chmod u+x -- "$SELF_DIR/loader-live-output" || \
    fail "Cannot mark local test binary executable; unpack into a user-writable directory"
fi
[[ -x "$SELF_DIR/loader-live-output" ]] || fail "Test app not executable"
# Verify ELF architecture and linked libraries, never modify the OS.
if command -v ldd >/dev/null; then
  for f in "$SELF_DIR/libVkLayer_gfg_native.so" "$SELF_DIR/loader-live-output"; do
    out="$(ldd "$f" 2>&1)" || fail "Cannot load shared dependencies for ${f##*/}: $out"
    if grep -q 'not found' <<< "$out"; then
      printf '%s\n' "$out" >&2
      fail "Missing runtime dependency for ${f##*/}"
    fi
  done
fi
printf 'GSF_DECK_KIT_CHECK_OK checksum=pass arch=x86_64 mode=%s phase=%s\n' "$MODE" "$PHASE"
[[ "$ACTION" == run ]] || exit 0

[[ "$(id -u)" != 0 ]] || fail "Never run as root."
[[ -n "${DISPLAY:-}" ]] || fail "No XWayland DISPLAY. Launch in Gaming Mode as a Non-Steam app or in Desktop Mode."
if [[ "$MODE" == nested ]]; then
  command -v gamescope >/dev/null || fail "Nested gamescope executable not found"
fi
command -v env >/dev/null || fail "env missing"

osname="$(. /etc/os-release 2>/dev/null; printf '%s' "${PRETTY_NAME:-unknown}")"
if [[ "$osname" != *SteamOS* && "$osname" != *steamOS* ]]; then
  printf 'GSF_DECK_KIT_WARNING: detected OS %s, not verified as SteamOS\n' "$osname" >&2
fi

RUN_DIR="${HOME:?}/GFG-Output-Deck-Tests/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p -- "$RUN_DIR"
printf 'GSF_DECK_KIT_RESULTS=%s\n' "$RUN_DIR"
{
  printf 'mode=%s\nphase=%s\nframes=%s\nos=%s\nuname=%s\n' \
    "$MODE" "$PHASE" "$FRAMES" "$osname" "$(uname -r)"
  printf 'display_is_set=%s\nwayland_is_set=%s\n' \
    "$([[ -n "${DISPLAY:-}" ]] && echo yes || echo no)" \
    "$([[ -n "${WAYLAND_DISPLAY:-}" ]] && echo yes || echo no)"
  printf 'date_utc=%s\n' "$(date -u --iso-8601=seconds)"
} > "$RUN_DIR/metadata.txt"
if command -v gamescope >/dev/null; then
  gamescope --version > "$RUN_DIR/gamescope-version.txt" 2>&1 || :
fi
if command -v vulkaninfo >/dev/null; then
  timeout 8s vulkaninfo --summary > "$RUN_DIR/vulkan-summary.txt" 2>&1 || :
fi

# The parent gamescope is always run WITHOUT the test layer. Vulkan layer
# injection is limited to this one test executable's child environment.
common=(
  GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT=1
  GFG_NATIVE_TEST_PROXY_CREATE=1
  GFG_NATIVE_TEST_WINDOW_WIDTH="$WINDOW_WIDTH"
  GFG_NATIVE_TEST_WINDOW_HEIGHT="$WINDOW_HEIGHT"
  GFG_NATIVE_TEST_PACED_45_90=1
)
if [[ "$MODE" == gaming ]]; then
  # Gamescope WSI's Vulkan image need not be readable via XCB GetImage.
  # Preserve the hardware/physical scanout UNKNOWN qualification.
  common+=( GFG_NATIVE_TEST_GAME_MODE=1 )
fi
run_phase() {
  local phase="$1" label log rc
  label="baseline"
  local -a client_env
  if [[ "$phase" == output ]]; then
    label="output"
    client_env=( "${common[@]}"
      GFG_NATIVE_FG=1 GFG_NATIVE_TRACE=1
      GFG_NATIVE_TEST_OUTPUT_INTERLEAVE=1
      VK_INSTANCE_LAYERS=VK_LAYER_GFG_native
      VK_LAYER_PATH="$SELF_DIR"
    )
    if [[ "$MODE" == gaming ]]; then
      client_env+=( GFG_NATIVE_TEST_GAME_MODE=1 )
    fi
    if [[ "$FRAMES" == 120 ]]; then
      client_env+=( GFG_NATIVE_TEST_SUSTAINED=1 )
    fi
  else
    # Baseline intentionally has NO GFG Vulkan layer installed/loaded.
    # Eight source originals, no diagnostic inserts.
    client_env=(
      GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT=1
      GFG_NATIVE_TEST_PROXY_CREATE=1
      GFG_NATIVE_TEST_WINDOW_WIDTH="$WINDOW_WIDTH"
      GFG_NATIVE_TEST_WINDOW_HEIGHT="$WINDOW_HEIGHT"
      GFG_NATIVE_FG=0
      VK_INSTANCE_LAYERS=
      VK_LAYER_PATH="$SELF_DIR"
    )
    if [[ "$MODE" == gaming ]]; then
      client_env+=( GFG_NATIVE_TEST_GAME_MODE=1 )
    fi
  fi
  log="$RUN_DIR/${label}.log"
  printf 'GSF_DECK_KIT_PHASE=%s start (hard timeout 30s)\n' "$label"
  # Purge forced lavapipe/ICD and inherited Vulkan injection, so the actual
  # hardware ICD handles the app. Nested compositor inherits NONE of our
  # experimental layer variables.
  local -a clean_env=(
    -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH -u VK_ADD_LAYER_PATH
    -u VK_ICD_FILENAMES -u VK_DRIVER_FILES -u VK_ADD_DRIVER_FILES
    -u GFG_NATIVE_FG -u GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT
    -u GFG_NATIVE_TEST_PROXY_CREATE -u GFG_NATIVE_TEST_OUTPUT_INTERLEAVE
    -u GFG_NATIVE_TEST_SUSTAINED -u GFG_NATIVE_TEST_PACED_45_90
    -u GFG_NATIVE_TEST_PACED_30_60 -u GFG_NATIVE_TEST_GAME_MODE
  )
  set +e
  if [[ "$MODE" == nested ]]; then
    # Only "env client_env testbinary" runs inside nested Gamescope.
    timeout -k 3s 30s env "${clean_env[@]}" \
      gamescope -W 1280 -H 800 -w 1280 -h 800 -r 90 -f -- \
      env "${client_env[@]}" "$SELF_DIR/loader-live-output" > "$log" 2>&1
  else
    timeout -k 3s 30s env "${clean_env[@]}" \
      env "${client_env[@]}" "$SELF_DIR/loader-live-output" > "$log" 2>&1
  fi
  rc=$?
  set -e
  if ((rc)); then
    tail -n 35 -- "$log" >&2 || :
    printf 'GSF_DECK_KIT_PHASE=%s FAIL exit=%s log=%s\n' "$label" "$rc" "$log" >&2
    return 1
  fi
  if [[ "$phase" == output ]]; then
    grep -q 'GSF_LOADER_LIVE_INIT virtual_images=' "$log" \
      || fail "No virtual-Wsi layer; ordinary passthrough is NOT an output pass"
    if [[ "$MODE" == gaming ]]; then
      local expected=$FRAMES
      local intermediate=$((FRAMES-1))
      local total=$((FRAMES*2-1))
      grep -q "GSF_GAMESCOPE_SUBMIT source_originals=${expected} expected_diagnostic_intermediates=${intermediate} x11_readback=bypassed" "$log" \
        || fail "Gamescope application did not complete Vulkan submissions"
      grep -q "GSF_OUTPUT_PACER rate=90 emitted=${total} originals=${expected} intermediate=${intermediate}" "$log" \
        || fail "Gamescope output worker did not submit all expected Vulkan presents"
      if grep -q 'x11_distinct_pixels=1 PASS' "$log"; then
        fail "Gamescope mode falsely reported verified X11 pixels"
      fi
    else
      if [[ "$FRAMES" == 120 ]]; then
        grep -q 'GSF_LOADER_SPLIT_APP output_visible=239 original=120 intermediate=119 x11_distinct_pixels=1 PASS' "$log" \
          || fail "239-frame X11 verification marker missing (see $log)"
      else
        grep -q 'GSF_LOADER_SPLIT_APP output_visible=15 original=8 intermediate=7 x11_distinct_pixels=1 PASS' "$log" \
          || fail "15-frame X11 verification marker missing (see $log)"
      fi
      grep -q 'GSF_OUTPUT_PACER rate=90 emitted=' "$log" \
        || fail "Output pacing evidence missing"
    fi
  else
    if [[ "$MODE" == gaming ]]; then
      grep -q 'GSF_GAMESCOPE_APP_TERMINAL originals=8 api_submission=PASS physical_scanout=UNVERIFIED' "$log" \
        || fail "Baseline Gaming Mode Vulkan submission did not complete"
    else
      grep -q 'GSF_LOADER_LIVE_APP 8 GPU-rendered real source frames visible through one output WSI PASS' "$log" \
        || fail "Baseline X11 GPU original frames not observed"
    fi
    if grep -q 'GSF_LOADER_LIVE_INIT' "$log"; then
      fail "Experimental layer leaked into baseline. Not a valid comparison"
    fi
  fi
  if grep -E 'Validation Error|VUID-|UNASSIGNED-.*ERROR' "$log" > "$RUN_DIR/${label}-validation-errors.txt"; then
    fail "Vulkan validation error in $label (see logs)"
  fi
  if [[ "$MODE" == gaming ]]; then
    printf 'GSF_DECK_KIT_PHASE=%s VULKAN_SUBMISSIONS_PASS physical_scanout=UNVERIFIED log=%s\n' "$label" "$log"
  else
    printf 'GSF_DECK_KIT_PHASE=%s SOFTWARE_EVIDENCE_PASS log=%s\n' "$label" "$log"
  fi
}
if [[ "$PHASE" == both ]]; then
  run_phase baseline
fi
run_phase output
{
  if [[ "$MODE" == gaming ]]; then
    printf 'software_visible_frames=UNVERIFIED_GAMESCOPE_WSI\n'
    printf 'gpu_output_submissions=checked\n'
  else
    printf 'software_visible_frames=verified_by_X11_backing_image\n'
  fi
  printf 'physical_oled_scanout=UNVERIFIED\n'
  printf 'intermediate_type=GPU_AB_DIAGNOSTIC_NOT_LMC\n'
  printf 'real_game_proton=UNVERIFIED\n'
} > "$RUN_DIR/QUALIFICATION_STATUS.txt"
if [[ "$MODE" == gaming ]]; then
  printf 'GSF_DECK_KIT_GAMESCOPE_SUBMIT_PASS physical_scanout=UNVERIFIED output=%s\n' "$RUN_DIR"
else
  printf 'GSF_DECK_KIT_SOFTWARE_PASS output=%s\n' "$RUN_DIR"
fi
printf 'Physical OLED proof requires external-camera video of this session.\n'
printf 'No system Vulkan layer was installed. Stopping this process disables the test.\n'
