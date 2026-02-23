#!/usr/bin/env bash
# Text and optional time-range search on server log output.
# Log lines must start with ISO-like timestamp: YYYY-MM-DD HH:MM:SS (e.g. from Poco Logger).
# Default log path: logs/server.log (relative to dedup_server directory).
#
# Usage:
#   ./scripts/search_logs.sh [OPTIONS] [PATTERN] [LOGFILE]
#
# Options:
#   -s, --since DATETIME   Only lines on or after this (YYYY-MM-DD or YYYY-MM-DD HH:MM or HH:MM)
#   -u, --until DATETIME   Only lines before this (same formats)
#   -i, --ignore-case      Case-insensitive pattern
#   -n, --no-time          Ignore timestamps; filter by pattern only (faster on large files)
#
# Examples:
#   ./search_logs.sh "KNN"                    # search default logs/server.log
#   ./search_logs.sh "KNN" logs/other.log     # search specific file
#   ./search_logs.sh -s "2025-02-22 10:00" -u "2025-02-22 11:00" "full scan"
#   tail -f logs/server.log | ./search_logs.sh -n "error"   # stdin
#
# If LOGFILE is omitted and not reading from a pipe, uses logs/server.log. Exits with error if that file is missing.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEDUP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_LOG="${DEDUP_DIR}/logs/server.log"

PATTERN=""
LOGFILE=""
SINCE=""
UNTIL=""
IGNORE_CASE=""
NO_TIME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s|--since)   SINCE="$2"; shift 2 ;;
        -u|--until)   UNTIL="$2"; shift 2 ;;
        -i|--ignore-case) IGNORE_CASE="-i"; shift ;;
        -n|--no-time) NO_TIME=1; shift ;;
        -h|--help)
            head -21 "$SCRIPT_DIR/search_logs.sh"
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *)
            if [[ -z "$PATTERN" ]]; then
                PATTERN="$1"
            else
                LOGFILE="$1"
            fi
            shift
            ;;
    esac
done

# Default pattern: match all (so time filter alone works)
[[ -z "$PATTERN" ]] && PATTERN="."

# Default log path when not reading from stdin
input=""
if [[ -z "$LOGFILE" ]]; then
    if [[ -t 0 ]]; then
        LOGFILE="$DEFAULT_LOG"
        if [[ ! -f "$LOGFILE" ]]; then
            echo "Error: No log file found at $LOGFILE" >&2
            echo "Run the server with: ./start  (or redirect output to logs/server.log)" >&2
            echo "Or pass a log file: $0 PATTERN /path/to/logfile" >&2
            exit 1
        fi
        input="$LOGFILE"
    else
        input="-"
    fi
else
    input="$LOGFILE"
fi

if [[ -n "$input" ]] && [[ "$input" != "-" ]] && [[ ! -f "$input" ]]; then
    echo "Error: Log file not found: $input" >&2
    echo "Run the server with: ./start  or pass a valid log file path." >&2
    exit 1
fi

# Optional: parse timestamp from start of line (YYYY-MM-DD HH:MM:SS or YYYY-MM-DD HH:MM)
# We use awk to filter by time when --since/--until given
filter_time() {
    if [[ -n "$NO_TIME" ]] || [[ -z "$SINCE" ]] && [[ -z "$UNTIL" ]]; then
        cat
        return
    fi
    awk -v since="$SINCE" -v until="$UNTIL" '
    function parse(s) {
        if (match(s, /^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}/)) {
            return substr(s, RSTART, RLENGTH)
        }
        if (match(s, /^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}/)) {
            return substr(s, RSTART, RLENGTH)
        }
        if (match(s, /^[0-9]{4}-[0-9]{2}-[0-9]{2}/)) {
            return substr(s, RSTART, RLENGTH) " 00:00:00"
        }
        return ""
    }
    {
        t = parse($0)
        if (t == "") { print; next }
        if (since != "" && t < since) next
        if (until != "" && t >= until) next
        print
    }
    '
}

if [[ "$input" = "-" ]]; then
    grep $IGNORE_CASE -E "$PATTERN" | filter_time
else
    [[ ! -f "$input" ]] && { echo "File not found: $input" >&2; exit 1; }
    grep $IGNORE_CASE -E "$PATTERN" "$input" | filter_time
fi
