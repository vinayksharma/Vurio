#!/bin/bash
# Quick check: which DB the server uses and whether it has data.
# Run from vurio root: ./scripts/check_dedup_db.sh

set -e
DB_PATH="${1:-dedup_server/data/dedup_server.db}"
if [ ! -f "$DB_PATH" ]; then
  echo "Database not found: $DB_PATH"
  exit 1
fi
echo "Database: $DB_PATH"
echo "  Media locations (registered paths): $(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM user_settings WHERE key LIKE 'mediaLocation:%';" 2>/dev/null || echo "?")"
echo "  Duplicate groups: $(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM duplicate_groups;" 2>/dev/null || echo "?")"
echo "  Scanned files: $(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM scanned_files;" 2>/dev/null || echo "?")"
echo ""
echo "If all are 0, register a media path in the UI and run a scan to populate."
