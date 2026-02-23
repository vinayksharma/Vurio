#!/usr/bin/env bash
# Check whether the media dedup database has the vector index (vec0/KNN) and report status.
# Usage: run from repo root or dedup_server: ./scripts/check_vector_index.sh
# Optional: CONFIG_PATH=/path/to/config.yaml DB_PATH=/path/to/db ./scripts/check_vector_index.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEDUP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_PATH="${CONFIG_PATH:-$DEDUP_DIR/config/config.yaml}"
DB_PATH="${DB_PATH:-}"

# Parse config.yaml for database.path, database.vector.enabled, database.vector.extensionPath
get_config() {
    local key="$1"
    local default="$2"
    if [[ -f "$CONFIG_PATH" ]]; then
        # Match "key: value" or "key: \"value\"" and strip quotes
        local line
        line=$(grep -E "^${key}[[:space:]]*:" "$CONFIG_PATH" 2>/dev/null | head -1)
        if [[ -n "$line" ]]; then
            local val
            val=$(echo "$line" | sed -E 's/^[^:]+:[[:space:]]*//' | sed -E 's/^"//; s/"$//')
            echo "$val"
            return
        fi
    fi
    echo "$default"
}

db_path_config=$(get_config "database.path" "data/dedup_server.db")
vector_enabled=$(get_config "database.vector.enabled" "false")
extension_path=$(get_config "database.vector.extensionPath" "")

# Resolve DB path
if [[ -z "$DB_PATH" ]]; then
    if [[ "$db_path_config" == /* ]]; then
        DB_PATH="$db_path_config"
    else
        # Relative to dedup_server dir (where server usually runs)
        DB_PATH="$DEDUP_DIR/$db_path_config"
    fi
fi

# Report config
echo "=== Vector index status ==="
echo "Config file:    ${CONFIG_PATH}"
echo "Database path:  ${DB_PATH}"
echo "Vector enabled: ${vector_enabled}"
echo "Extension path: ${extension_path:-'(not set)'}"
echo ""

if [[ ! -f "$DB_PATH" ]]; then
    echo "Database file not found. Server may not have been run yet."
    echo "Vector table and KNN cannot be checked without the DB file."
    exit 0
fi

# Check sqlite_master for vec_embedding_index (no extension needed)
vec_table=""
if command -v sqlite3 &>/dev/null; then
    vec_table=$(sqlite3 "$DB_PATH" "SELECT name FROM sqlite_master WHERE type='table' AND name='vec_embedding_index';" 2>/dev/null || true)
fi

if [[ -n "$vec_table" ]]; then
    echo "Vector index table:  present (vec_embedding_index)"
    echo "KNN backend:         configured (vec0 virtual table exists)"
else
    echo "Vector index table:  not present"
    echo "KNN backend:         not created (table vec_embedding_index missing)"
fi

# If extension path is set, try to load it and count vectors (needs sqlite3 with load_extension, e.g. Homebrew)
count=""
if [[ -n "$extension_path" ]] && [[ -n "$vec_table" ]]; then
    ext_abs="$extension_path"
    if [[ "$extension_path" != /* ]]; then
        ext_abs="$DEDUP_DIR/$extension_path"
    fi
    if [[ -f "$ext_abs" ]]; then
        for sqlite_cmd in /opt/homebrew/opt/sqlite/bin/sqlite3 /usr/local/opt/sqlite/bin/sqlite3 sqlite3; do
            if command -v $sqlite_cmd &>/dev/null; then
                count=$($sqlite_cmd "$DB_PATH" ".load '$ext_abs'" "SELECT COUNT(*) FROM vec_embedding_index;" 2>/dev/null || true)
                [[ -n "$count" ]] && [[ "$count" =~ ^[0-9]+$ ]] && break
                count=""
            fi
        done
        if [[ -n "$count" ]] && [[ "$count" =~ ^[0-9]+$ ]]; then
            echo "Vectors indexed:     $count"
            echo "KNN library in use:  yes (extension loads; index queryable)"
        else
            echo "Vectors indexed:     (query needs sqlite3 with load_extension, e.g. brew install sqlite)"
            echo "KNN library in use:  server uses it at runtime when enabled and extension loads"
        fi
    else
        echo "Vectors indexed:     (extension file not found at $ext_abs)"
        echo "KNN library in use:  no (extension file missing)"
    fi
elif [[ -n "$vec_table" ]]; then
    echo "Vectors indexed:     (set database.vector.extensionPath and re-run to query count)"
    echo "KNN library in use:  configured in DB; server uses it when enabled and extension loads"
fi

echo ""
echo "Summary:"
if [[ "$vector_enabled" == "true" ]] && [[ -n "$extension_path" ]] && [[ -n "$vec_table" ]]; then
    echo "  The server is configured for vector-based duplicate search (KNN)."
    echo "  When the duplicate finder runs, it uses the vec0 index when the extension loads successfully."
elif [[ -n "$vec_table" ]]; then
    echo "  Vector table exists; enable database.vector.enabled and set database.vector.extensionPath to use KNN."
else
    echo "  No vector index table yet. Enable vector in config and run the server to create and use it."
fi
