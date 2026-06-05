#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ENV_NAME="${PIOENV:-cardputer_adv}"
PORT=""
BAUD="115200"
LOG_ROOT="captures/cardputer-wardrive"
TAG=""
DO_BUILD=1
DO_UPLOAD=0
USE_CAFFEINATE=1
RECONNECT_DELAY=2
MAX_RECONNECTS=0
CAFFEINATE_PID=""

usage() {
    cat <<'USAGE'
Capture Cardputer Wardrive crashes from a Mac.

Default behavior:
  - builds cardputer_adv first so esp32_exception_decoder has a fresh ELF
  - starts caffeinate while the capture is running
  - runs pio device monitor with the env's exception/time filters
  - writes raw serial plus a compact event/crash log
  - reconnects if the serial monitor exits

Usage:
  tools/cardputer_wardrive_capture.sh [options]

Options:
  -e, --env ENV              PlatformIO env (default: cardputer_adv)
  -p, --port PORT            Serial port, e.g. /dev/cu.usbmodem1101
  -b, --baud BAUD            Serial baud (default: 115200)
      --tag NAME             Add a label to the log folder name
      --log-dir DIR          Capture root (default: captures/cardputer-wardrive)
      --build                Build before monitor (default)
      --no-build             Skip preflight build
      --upload               Build and upload before monitor
      --no-caffeinate        Do not hold macOS awake
      --reconnect-delay SEC  Delay before relaunch after monitor exit (default: 2)
      --max-reconnects N     Stop after N monitor launches (default: 0 = forever)
      --list-ports           Show PlatformIO serial ports and exit
  -h, --help                 Show this help

Examples:
  tools/cardputer_wardrive_capture.sh --port /dev/cu.usbmodem1101 --tag high-rf
  tools/cardputer_wardrive_capture.sh --upload --port /dev/cu.usbmodem1101

Note:
  caffeinate prevents idle sleep while the script runs. On some Macs, closed-lid
  sleep still requires AC power and normal clamshell conditions.
USAGE
}

need_arg() {
    if [ "$#" -lt 2 ] || [ -z "$2" ]; then
        echo "error: $1 requires an argument" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -e|--env)
            need_arg "$1" "${2:-}"
            ENV_NAME="$2"
            shift 2
            ;;
        -p|--port)
            need_arg "$1" "${2:-}"
            PORT="$2"
            shift 2
            ;;
        -b|--baud|--monitor-speed)
            need_arg "$1" "${2:-}"
            BAUD="$2"
            shift 2
            ;;
        --tag)
            need_arg "$1" "${2:-}"
            TAG="$2"
            shift 2
            ;;
        --log-dir)
            need_arg "$1" "${2:-}"
            LOG_ROOT="$2"
            shift 2
            ;;
        --build)
            DO_BUILD=1
            shift
            ;;
        --no-build)
            DO_BUILD=0
            shift
            ;;
        --upload)
            DO_UPLOAD=1
            DO_BUILD=1
            shift
            ;;
        --no-caffeinate)
            USE_CAFFEINATE=0
            shift
            ;;
        --reconnect-delay)
            need_arg "$1" "${2:-}"
            RECONNECT_DELAY="$2"
            shift 2
            ;;
        --max-reconnects)
            need_arg "$1" "${2:-}"
            MAX_RECONNECTS="$2"
            shift 2
            ;;
        --list-ports)
            cd "$REPO_ROOT"
            export PLATFORMIO_SETTING_ENABLE_TELEMETRY=no
            pio device list
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

cd "$REPO_ROOT"
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=no

if ! command -v pio >/dev/null 2>&1; then
    echo "error: PlatformIO CLI not found in PATH (expected 'pio')" >&2
    exit 1
fi

if [ "$USE_CAFFEINATE" -eq 1 ] && ! command -v caffeinate >/dev/null 2>&1; then
    echo "warning: caffeinate not found; continuing without macOS sleep assertion" >&2
    USE_CAFFEINATE=0
fi

SAFE_TAG="$(printf '%s' "$TAG" | tr -c 'A-Za-z0-9_.-' '_' | sed 's/^_*//; s/_*$//')"
STAMP="$(date '+%Y%m%d_%H%M%S')"
SESSION_NAME="$STAMP"
if [ -n "$SAFE_TAG" ]; then
    SESSION_NAME="${SESSION_NAME}_${SAFE_TAG}"
fi

SESSION_DIR="${LOG_ROOT}/${SESSION_NAME}"
mkdir -p "$SESSION_DIR"
SESSION_DIR_ABS="$(cd "$SESSION_DIR" && pwd)"
LOG_ROOT_ABS="$(cd "$LOG_ROOT" && pwd)"
ln -sfn "$SESSION_DIR_ABS" "${LOG_ROOT_ABS}/latest"

RAW_LOG="${SESSION_DIR_ABS}/serial_raw.log"
EVENT_LOG="${SESSION_DIR_ABS}/events.log"
SUMMARY_LOG="${SESSION_DIR_ABS}/summary.log"
RUN_LOG="${SESSION_DIR_ABS}/run.log"
INFO_LOG="${SESSION_DIR_ABS}/capture_info.txt"

ELF_PATH=".pio/build/${ENV_NAME}/firmware.elf"

write_info() {
    {
        echo "capture_start_local=$(date '+%Y-%m-%d %H:%M:%S %z')"
        echo "repo_root=${REPO_ROOT}"
        echo "env=${ENV_NAME}"
        echo "port=${PORT:-auto}"
        echo "baud=${BAUD}"
        echo "build=${DO_BUILD}"
        echo "upload=${DO_UPLOAD}"
        echo "caffeinate=${USE_CAFFEINATE}"
        echo "raw_log=${RAW_LOG}"
        echo "events_log=${EVENT_LOG}"
        echo "summary_log=${SUMMARY_LOG}"
        echo
        echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
        echo
        echo "git_status_short:"
        git status --short 2>/dev/null || true
    } > "$INFO_LOG"
}

interesting_line() {
    case "$1" in
        *"[SYSTEM]"*|*"[WIFI-MODE]"*|*"[WARDRIVE"*|*"[WARDRIVE/CP]"*|*"[P4-BRIDGE]"*|*"[P4-UART]"*|*"[PI4IOE]"*|*"[SD]"*|*"[SD-LATE]"*|*"[TREATY]"*|*"[GPS]"*|*"[HAL]"*) return 0 ;;
        *"Disconnected "*|*"Reconnecting to "*|*" Connected!"*|*"Device not configured"*|*"could not open port"*|*"Please build project in debug configuration"*) return 0 ;;
        *"Guru Meditation"*|*"Backtrace:"*|*"Core "*panic*|*"panic'ed"*|*"abort()"*|*"abort called"*|*"assert failed"*) return 0 ;;
        *"Task watchdog"*|*"Brownout detector"*|*"CORRUPT HEAP"*|*"heap corruption"*|*"stack canary"*) return 0 ;;
        *"LoadProhibited"*|*"StoreProhibited"*|*"IllegalInstruction"*|*"InstrFetchProhibited"*|*"IntegerDivideByZero"*) return 0 ;;
        *"ELF file SHA256"*|*"Rebooting..."*|*"rst:"*|*".cpp:"*|*".h:"*) return 0 ;;
    esac
    return 1
}

summarize_capture() {
    {
        echo "Cardputer Wardrive capture summary"
        echo "finished_local=$(date '+%Y-%m-%d %H:%M:%S %z')"
        echo
        echo "raw_log=${RAW_LOG}"
        echo "events_log=${EVENT_LOG}"
        echo "run_log=${RUN_LOG}"
        echo
        echo "Crash markers:"
        if [ -s "$RAW_LOG" ]; then
            grep -E -n 'Guru Meditation|Backtrace:|panic|panic.ed|abort|assert failed|Task watchdog|Brownout|CORRUPT HEAP|heap corruption|stack canary|LoadProhibited|StoreProhibited|IllegalInstruction|InstrFetchProhibited|IntegerDivideByZero|ELF file SHA256|Rebooting|rst:|Disconnected|Reconnecting|Device not configured|Reset reason|Please build project in debug configuration' "$RAW_LOG" | tail -n 120 || true
        else
            echo "(raw log is empty)"
        fi
        echo
        echo "Last 120 system/Wardrive/P4/SD/GPS/crash events:"
        if [ -s "$EVENT_LOG" ]; then
            tail -n 120 "$EVENT_LOG"
        else
            echo "(no interesting lines captured)"
        fi
    } > "$SUMMARY_LOG"
}

cleanup() {
    rc=$?
    trap - EXIT INT TERM
    if [ -n "$CAFFEINATE_PID" ] && kill -0 "$CAFFEINATE_PID" >/dev/null 2>&1; then
        kill "$CAFFEINATE_PID" >/dev/null 2>&1 || true
    fi
    summarize_capture
    echo
    echo "Capture stopped."
    echo "Raw log:     $RAW_LOG"
    echo "Events log:  $EVENT_LOG"
    echo "Summary:     $SUMMARY_LOG"
    exit "$rc"
}
trap cleanup EXIT INT TERM

write_info

{
    echo "=== Cardputer Wardrive capture ==="
    echo "session: $SESSION_DIR_ABS"
    echo "env:     $ENV_NAME"
    echo "port:    ${PORT:-auto}"
    echo "baud:    $BAUD"
    echo "start:   $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo
} | tee -a "$RUN_LOG"

if [ "$DO_UPLOAD" -eq 1 ]; then
    echo "Uploading ${ENV_NAME} before capture..." | tee -a "$RUN_LOG"
    pio run -e "$ENV_NAME" -t upload 2>&1 | tee -a "$RUN_LOG"
elif [ "$DO_BUILD" -eq 1 ]; then
    echo "Building ${ENV_NAME} before capture for exception decoding..." | tee -a "$RUN_LOG"
    pio run -e "$ENV_NAME" 2>&1 | tee -a "$RUN_LOG"
fi

if [ -f "$ELF_PATH" ]; then
    echo "Decoder ELF: $ELF_PATH" | tee -a "$RUN_LOG"
else
    echo "WARNING: $ELF_PATH not found; panic backtraces may not decode to file:line." | tee -a "$RUN_LOG"
fi

if [ "$USE_CAFFEINATE" -eq 1 ]; then
    caffeinate -dimsu -w "$$" &
    CAFFEINATE_PID="$!"
    echo "caffeinate active (pid $CAFFEINATE_PID)" | tee -a "$RUN_LOG"
fi

MONITOR_CMD=(pio device monitor -e "$ENV_NAME" -b "$BAUD")
if [ -n "$PORT" ]; then
    MONITOR_CMD+=(-p "$PORT")
fi

echo "Starting monitor. Press Ctrl-C to stop." | tee -a "$RUN_LOG"
echo "Logs are under: $SESSION_DIR_ABS" | tee -a "$RUN_LOG"
echo

attempt=0
while :; do
    attempt=$((attempt + 1))
    if [ "$MAX_RECONNECTS" -gt 0 ] && [ "$attempt" -gt "$MAX_RECONNECTS" ]; then
        echo "Reached --max-reconnects=$MAX_RECONNECTS; stopping." | tee -a "$RUN_LOG"
        break
    fi

    {
        echo
        echo "===== monitor launch ${attempt} @ $(date '+%Y-%m-%d %H:%M:%S %z') ====="
        printf 'command:'
        printf ' %q' "${MONITOR_CMD[@]}"
        echo
    } | tee -a "$RAW_LOG" "$EVENT_LOG" "$RUN_LOG" >/dev/null

    set +e
    "${MONITOR_CMD[@]}" 2>&1 | while IFS= read -r line; do
        printf '%s\n' "$line" | tee -a "$RAW_LOG"
        if interesting_line "$line"; then
            printf '%s\n' "$line" >> "$EVENT_LOG"
        fi
    done
    monitor_rc=${PIPESTATUS[0]}
    set -e

    {
        echo "monitor exited rc=${monitor_rc} @ $(date '+%Y-%m-%d %H:%M:%S %z')"
        echo "relaunching in ${RECONNECT_DELAY}s..."
    } | tee -a "$RAW_LOG" "$EVENT_LOG" "$RUN_LOG" >/dev/null
    sleep "$RECONNECT_DELAY"
done
