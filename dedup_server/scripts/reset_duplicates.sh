#!/bin/bash
# Script to reset duplicate detection database tables
# This removes all duplicate groups, members, and checkpoints
# Useful after fixing duplicate detection bugs to start fresh

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DB_PATH="$PROJECT_ROOT/data/dedup_server.db"

echo "=========================================="
echo "  Duplicate Detection Database Reset"
echo "=========================================="
echo ""

# Check if database exists
if [ ! -f "$DB_PATH" ]; then
    echo "[ERROR] Database not found at: $DB_PATH"
    exit 1
fi

echo "[INFO] Database path: $DB_PATH"
echo ""

# Prompt for confirmation
echo "[WARNING] This will delete ALL duplicate detection data:"
echo "  - All duplicate groups"
echo "  - All duplicate members"  
echo "  - All duplicate processing checkpoints"
echo ""
read -p "Are you sure you want to continue? (yes/no): " confirmation

if [ "$confirmation" != "yes" ]; then
    echo "[INFO] Operation cancelled"
    exit 0
fi

echo ""
echo "[INFO] Resetting duplicate detection tables..."

# Execute SQL commands
sqlite3 "$DB_PATH" <<EOF
-- Delete all duplicate members
DELETE FROM duplicate_members;

-- Delete all duplicate groups
DELETE FROM duplicate_groups;

-- Delete all checkpoints
DELETE FROM duplicate_processing_checkpoint;

-- Vacuum to reclaim space
VACUUM;

-- Show final counts (should all be 0)
SELECT 'duplicate_groups count: ' || COUNT(*) FROM duplicate_groups;
SELECT 'duplicate_members count: ' || COUNT(*) FROM duplicate_members;
SELECT 'checkpoints count: ' || COUNT(*) FROM duplicate_processing_checkpoint;
EOF

echo ""
echo "[SUCCESS] Duplicate detection data reset complete!"
echo ""
echo "Next steps:"
echo "  1. Restart the server: ./start"
echo "  2. Duplicate finder will rebuild groups from scratch"
echo "  3. Monitor logs for progress"
echo ""

