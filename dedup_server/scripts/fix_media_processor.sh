#!/bin/bash
# Script to fix media_processor.cpp for single-mode refactoring
# Run from project root: ./scripts/fix_media_processor.sh

FILE="src/media_processors/media_processor.cpp"

echo "Fixing $FILE..."

# Backup original
cp "$FILE" "${FILE}.backup"

# Remove ServerMode variable declarations in video/audio lambdas
sed -i '' 's/ServerMode server_mode_copy = server_mode;$//' "$FILE"
sed -i '' 's/, server_mode_copy\]/]/' "$FILE"
sed -i '' 's/\[file_path_copy, server_mode_copy,/\[file_path_copy,/' "$FILE"

# Replace processed_fast/balanced/quality field access with processed
sed -i '' 's/current_file->processed_fast/current_file->processed/g' "$FILE"
sed -i '' 's/current_file->processed_balanced/current_file->processed/g' "$FILE"
sed -i '' 's/current_file->processed_quality/current_file->processed/g' "$FILE"

# Remove switch statements checking processed status
# (This is complex, may need manual cleanup)

# Replace processor method calls
sed -i '' 's/image_processor\.ProcessFast(/image_processor.Process(/g' "$FILE"
sed -i '' 's/image_processor\.ProcessBalanced(/image_processor.Process(/g' "$FILE"
sed -i '' 's/image_processor\.ProcessQuality(/image_processor.Process(/g' "$FILE"

sed -i '' 's/video_processor\.ProcessFast(/video_processor.Process(/g' "$FILE"
sed -i '' 's/video_processor\.ProcessBalanced(/video_processor.Process(/g' "$FILE"
sed -i '' 's/video_processor\.ProcessQuality(/video_processor.Process(/g' "$FILE"

sed -i '' 's/audio_processor\.ProcessFast(/audio_processor.Process(/g' "$FILE"
sed -i '' 's/audio_processor\.ProcessBalanced(/audio_processor.Process(/g' "$FILE"
sed -i '' 's/audio_processor\.ProcessQuality(/audio_processor.Process(/g' "$FILE"

# Remove int current_status = 0; lines before switch statements
sed -i '' 's/int current_status = 0;$//' "$FILE"

# Remove switch statements - replace them with direct assignment
sed -i '' '/switch (server_mode_copy)/,/^[[:space:]]*}/ { 
    /case ServerMode::/d
    /break;/d
    /default:/d
    /switch (server_mode_copy)/d
    /^[[:space:]]*}/d
}' "$FILE"

# Fix the remaining pattern where switch was removed
sed -i '' '/^[[:space:]]*case ServerMode::/d' "$FILE"
sed -i '' '/^[[:space:]]*break;$/d' "$FILE"

echo "Done. Backup saved as ${FILE}.backup"
echo "Manual review still recommended for remaining issues"

