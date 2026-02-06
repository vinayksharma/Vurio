#!/bin/bash

# Script to scan vinayksharma/Pictures for all non-raw files and create copies in a new subfolder
# Created following CURSOR_RULES.md - helper scripts go under /scripts

set -e  # Exit on any error

# Configuration
SOURCE_DIR="/Users/vinaysharma/Pictures"
DEST_DIR="/Users/vinaysharma/Pictures/non_raw_copies"
LOG_FILE="/Users/vinaysharma/developer/dedup_server/logs/copy_non_raw_files.log"

# Create log directory if it doesn't exist
mkdir -p "$(dirname "$LOG_FILE")"

# Function to log messages
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

# Function to check if a file is a raw image
is_raw_file() {
    local file="$1"
    local extension="${file##*.}"
    # Convert to lowercase for comparison
    extension=$(echo "$extension" | tr '[:upper:]' '[:lower:]')
    case "$extension" in
        arw|cr2|dng|nef|orf|pef|raf|rw2|srw|tiff|tif)
            return 0  # It's a raw file
            ;;
        *)
            return 1  # It's not a raw file
            ;;
    esac
}

# Function to check if a file is a media file (image, video, audio)
is_media_file() {
    local file="$1"
    local extension="${file##*.}"
    # Convert to lowercase for comparison
    extension=$(echo "$extension" | tr '[:upper:]' '[:lower:]')
    case "$extension" in
        # Image formats
        jpg|jpeg|png|gif|bmp|webp|svg|ico|tiff|tif|arw|cr2|dng|nef|orf|pef|raf|rw2|srw)
            return 0
            ;;
        # Video formats
        mp4|avi|mov|wmv|flv|webm|mkv|m4v|3gp|ogv)
            return 0
            ;;
        # Audio formats
        mp3|wav|flac|aac|ogg|wma|m4a|opus)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

log "Starting non-raw file copy operation"
log "Source directory: $SOURCE_DIR"
log "Destination directory: $DEST_DIR"

# Check if source directory exists
if [ ! -d "$SOURCE_DIR" ]; then
    log "ERROR: Source directory does not exist: $SOURCE_DIR"
    exit 1
fi

# Create destination directory
log "Creating destination directory: $DEST_DIR"
mkdir -p "$DEST_DIR"

# Initialize counters
total_files=0
copied_files=0
skipped_files=0
error_files=0

# Find all files recursively and process them
log "Scanning for files..."

# Use find with error handling for permission denied directories
while IFS= read -r -d '' file; do
    total_files=$((total_files + 1))
    
    # Get relative path from source directory
    rel_path="${file#$SOURCE_DIR/}"
    
    # Skip if it's a raw file
    if is_raw_file "$file"; then
        log "SKIP (RAW): $rel_path"
        skipped_files=$((skipped_files + 1))
        continue
    fi
    
    # Skip if it's not a media file
    if ! is_media_file "$file"; then
        log "SKIP (NOT MEDIA): $rel_path"
        skipped_files=$((skipped_files + 1))
        continue
    fi
    
    # Create destination path
    dest_file="$DEST_DIR/$rel_path"
    dest_dir="$(dirname "$dest_file")"
    
    # Create destination directory if it doesn't exist
    mkdir -p "$dest_dir"
    
    # Copy the file
    if cp "$file" "$dest_file" 2>/dev/null; then
        log "COPIED: $rel_path"
        copied_files=$((copied_files + 1))
    else
        log "ERROR: Failed to copy $rel_path"
        error_files=$((error_files + 1))
    fi

done < <(find "$SOURCE_DIR" -type f -print0 2>/dev/null)

# Print summary
log "=== COPY OPERATION COMPLETE ==="
log "Total files processed: $total_files"
log "Files copied: $copied_files"
log "Files skipped (raw): $skipped_files"
log "Files with errors: $error_files"
log "Destination directory: $DEST_DIR"

# Show some statistics about the copied files
if [ $copied_files -gt 0 ]; then
    log "=== COPIED FILES STATISTICS ==="
    log "Total size of copied files:"
    du -sh "$DEST_DIR" | tee -a "$LOG_FILE"
    
    log "File type breakdown:"
    find "$DEST_DIR" -type f -name "*.jpg" -o -name "*.jpeg" | wc -l | xargs -I {} log "  JPEG files: {}"
    find "$DEST_DIR" -type f -name "*.png" | wc -l | xargs -I {} log "  PNG files: {}"
    find "$DEST_DIR" -type f -name "*.gif" | wc -l | xargs -I {} log "  GIF files: {}"
    find "$DEST_DIR" -type f -name "*.mp4" | wc -l | xargs -I {} log "  MP4 files: {}"
    find "$DEST_DIR" -type f -name "*.mov" | wc -l | xargs -I {} log "  MOV files: {}"
    find "$DEST_DIR" -type f -name "*.mp3" | wc -l | xargs -I {} log "  MP3 files: {}"
    find "$DEST_DIR" -type f -name "*.wav" | wc -l | xargs -I {} log "  WAV files: {}"
fi

log "Operation completed successfully!"
