# Duplicate Detection Threshold Range Implementation

## Overview

This document describes the implementation of the min/max threshold range system for duplicate detection, replacing the previous single threshold approach with a more flexible range-based system.

## Key Changes

### 1. Configuration Properties

**Before:**

```yaml
duplicates.threshold: 0.94
```

**After:**

```yaml
duplicates.threshold.min: 0.92 # Minimum threshold for adding files to groups
duplicates.threshold.max: 0.96 # Maximum threshold for representative swaps
```

### 2. Threshold Usage

- **Min Threshold (`duplicates.threshold.min`)**: Used for determining if files can be added to duplicate groups

  - Files with similarity score >= min threshold can be added to groups
  - Controls the "looseness" of duplicate detection
  - Changes trigger full reprocessing (groups deleted, checkpoint reset)

- **Max Threshold (`duplicates.threshold.max`)**: Used for representative file swaps
  - Only files with similarity score >= max threshold can become representatives
  - Controls the "strictness" of representative selection
  - Changes do NOT trigger reprocessing (existing groups preserved)

### 3. Behavior Changes

#### Group Formation

- Files are added to groups if similarity >= min threshold
- All-pairs similarity check ensures transitivity (A-B, A-C, B-C all >= min threshold)

#### Representative Selection

- Representative swaps only occur if new candidate has similarity >= max threshold
- Prevents "loose" matches from becoming representatives
- Maintains quality of representative files

#### Reprocessing Logic

- **Min threshold decrease**: Triggers full reprocessing (more permissive)
- **Min threshold increase**: No reprocessing (existing groups still valid)
- **Max threshold changes**: No reprocessing (doesn't affect group membership)

## Implementation Details

### Database Schema

The `duplicate_groups` table stores the max threshold for backwards compatibility:

```sql
similarity_threshold REAL NOT NULL  -- Stores max threshold value
```

### Configuration Change Handler

```cpp
// Only trigger reprocessing if min threshold changed
if (min_changed) {
    // Delete all EMBEDDING groups and reset checkpoint
    DuplicateGroupsOps::deleteGroupsByMode(db_, "EMBEDDING");
    DuplicateGroupsOps::resetCheckpoint(db_, "EMBEDDING");
}
```

### Representative Swap Logic

```cpp
bool should_update_rep = isBetterRepresentative(new_candidate, current_rep) &&
                          similarity_score >= getThresholdMax("EMBEDDING");
```

## Migration Strategy

### No Backward Compatibility

- Old `duplicates.threshold` property removed
- Database schema unchanged (stores max threshold)
- Full reprocessing required when switching to new system

### Configuration Migration

1. Update config files to use new properties
2. Reset duplicate groups: `DELETE /api/v1/duplicates/reset`
3. System will reprocess all files with new thresholds

## Testing

### Unit Tests

- `DuplicateFinderRangeTest.LoadsThreshold`: Verifies correct threshold loading
- `DuplicateFinderRangeTest.ThresholdExpansionTriggersReprocess`: Tests min threshold decrease
- `DuplicateFinderRangeTest.StricterThresholdDoesNotReprocess`: Tests max threshold increase

### Test Scenarios

1. **Min threshold decrease (0.92 → 0.90)**: Groups deleted, reprocessing triggered
2. **Max threshold increase (0.96 → 0.98)**: Groups preserved, no reprocessing
3. **Invalid range (min > max)**: Reverted to defaults with error logging

## Configuration Examples

### Conservative Settings (High Precision)

```yaml
duplicates.threshold.min: 0.95 # Strict group formation
duplicates.threshold.max: 0.98 # Very strict representative selection
```

### Balanced Settings (Recommended)

```yaml
duplicates.threshold.min: 0.92 # Good group formation
duplicates.threshold.max: 0.96 # Quality representative selection
```

### Permissive Settings (High Recall)

```yaml
duplicates.threshold.min: 0.88 # Loose group formation
duplicates.threshold.max: 0.94 # Moderate representative selection
```

## Benefits

1. **Flexible Control**: Separate thresholds for different aspects of duplicate detection
2. **Quality Preservation**: Max threshold ensures high-quality representatives
3. **Efficient Reprocessing**: Only reprocess when min threshold changes
4. **Clear Semantics**: Min for inclusion, max for quality
5. **Backward Compatibility**: Database schema unchanged

## Future Enhancements

- Tiered duplicate detection using the full range
- Dynamic threshold adjustment based on dataset characteristics
- Per-file-type threshold customization
- Machine learning-based threshold optimization
