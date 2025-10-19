# Documentation Index

All project documentation is consolidated in this directory for easy navigation and maintenance.

## Project Guidelines

- **[CURSOR_RULES.md](CURSOR_RULES.md)** - Core development workflow and rules for AI agents and developers

## Architecture & Design

- **[duplicate_detection_architecture.md](duplicate_detection_architecture.md)** - Duplicate detection system architecture
- **[image_processing_architecture.md](image_processing_architecture.md)** - Image processing pipeline architecture
- **[files_manager.md](files_manager.md)** - FilesManager technical specification
- **[files_scanner.md](files_scanner.md)** - FileScanner technical specification

## API Documentation

- **[DUPLICATES_API.md](DUPLICATES_API.md)** - Duplicate Groups API - paginated retrieval with representatives and candidates
- **[THUMBNAIL_API.md](THUMBNAIL_API.md)** - Thumbnail generation API with caching

## Feature Implementation

- **[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)** - Summary of major feature implementations
- **[processing_status_codes.md](processing_status_codes.md)** - File processing status code definitions

## Technical Analysis

- **[OPENCV_VS_IMAGEMAGICK_ANALYSIS.md](OPENCV_VS_IMAGEMAGICK_ANALYSIS.md)** - Comparison of OpenCV vs ImageMagick roles
- **[RAW_JPEG_TRANSCODING_ANALYSIS.md](RAW_JPEG_TRANSCODING_ANALYSIS.md)** - RAW file transcoding: TIFF vs JPEG analysis

## Troubleshooting & Fixes

- **[IMAGEMAGICK_CRASH_FIX.md](IMAGEMAGICK_CRASH_FIX.md)** - Fix for SIGABRT crashes on corrupted TIFF files
- **[ARW_TRANSCODING_FIX.md](ARW_TRANSCODING_FIX.md)** - Sony ARW RAW file transcoding fixes
- **[THUMBNAIL_FAILURES_ANALYSIS.md](THUMBNAIL_FAILURES_ANALYSIS.md)** - Thumbnail generation failure analysis
- **[THUMBNAIL_PERFORMANCE_INVESTIGATION.md](THUMBNAIL_PERFORMANCE_INVESTIGATION.md)** - Performance optimization investigation
- **[LOGGING_LEVEL_FIX.md](LOGGING_LEVEL_FIX.md)** - Logging level configuration fix
- **[STDERR_CAPTURE_REMOVAL.md](STDERR_CAPTURE_REMOVAL.md)** - stderr capture removal rationale
- **[VALIDATION_IMPLEMENTATION.md](VALIDATION_IMPLEMENTATION.md)** - TIFF validation implementation

## User Guides

- **[START_SCRIPT_README.md](START_SCRIPT_README.md)** - Server startup script documentation
- **[WEB_SERVER_README.md](WEB_SERVER_README.md)** - Web server configuration and usage

## Executive Reports

- **[REPORT_EXECUTIVE_SUMMARY.md](REPORT_EXECUTIVE_SUMMARY.md)** - High-level executive summary

---

## Documentation Standards

Per `CURSOR_RULES.md`:

- ✅ ALL documentation (.md files) MUST be in `/docs` directory
- ✅ ONLY exception: `README.md` (stays in project root - GitHub standard)
- ✅ Never create .md files in the project root (except README.md)
- ✅ All technical, API, architecture, and troubleshooting docs go in `/docs`

## Total Documentation

**21 files** organized in `/docs` covering architecture, APIs, troubleshooting, and project guidelines.

