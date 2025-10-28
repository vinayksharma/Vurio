# Cross-Group Similarity Checking

## Overview

The duplicate detection system now includes **cross-group similarity checking** functionality that identifies files with low similarity scores in their current groups and checks if they would be better matched in other groups. This feature helps optimize duplicate group quality by moving files to more appropriate groups.

## How It Works

### Threshold Range Control

Cross-group checking is controlled by the **threshold range** configuration:

- **`duplicates.threshold.min`**: Minimum similarity threshold for adding files to groups
- **`duplicates.threshold.max`**: Maximum similarity threshold for representative swaps

### Cross-Group Checking Logic

1. **Identification**: After normal duplicate processing, the system identifies files with similarity scores **below** the `threshold.max` in their current groups
2. **Cross-Group Comparison**: These low-similarity files are compared against representatives of **other groups**
3. **File Movement**: If a file finds a better match (higher similarity score) in another group, it's moved there
4. **Group Updates**: Member counts and representative information are updated accordingly

### Example Scenario

```
Group 1: File A (rep), File B (similarity=0.75 to A)
Group 2: File C (rep), File D (similarity=0.85 to C)

If threshold.max = 0.80:
- File B (0.75 < 0.80) is eligible for cross-group checking
- File B compared to File C → similarity = 0.90
- Since 0.90 > 0.75, File B moves to Group 2
- Result: Group 1 has File A only, Group 2 has Files C, D, B
```

## Configuration Behavior

### Narrow Range (More Cross-Group Checking)

```yaml
duplicates.threshold.min: 0.90 # Low threshold for group formation
duplicates.threshold.max: 0.95 # High threshold for cross-group moves
```

- More files will have scores below 0.95
- More cross-group checking occurs
- More aggressive optimization

### Wide Range (Less Cross-Group Checking)

```yaml
duplicates.threshold.min: 0.85 # Higher threshold for group formation
duplicates.threshold.max: 0.98 # Very high threshold for cross-group moves
```

- Fewer files will have scores below 0.98
- Less cross-group checking occurs
- More conservative optimization

## Implementation Details

### Database Operations

The system uses these new database operations:

- `removeMember()`: Removes a file from its current group
- `updateGroupMemberCount()`: Updates group member counts after moves
- `addToGroup()`: Adds a file to a new group (existing function)

### Performance Considerations

- Cross-group checking runs **after** normal batch processing
- Only files with low similarity scores are checked
- Representative-based comparison (not all-pairs) for efficiency
- Metadata filtering prevents incompatible file comparisons

### Error Handling

- Failed moves are rolled back to original group
- Partial move counts are returned on exceptions
- Comprehensive logging for debugging

## API Integration

Cross-group checking is automatically integrated into the duplicate finding process:

```cpp
// In processBatch() after normal processing:
double threshold_max = getThresholdMax(mode);
int cross_group_moves = performCrossGroupChecking(mode, threshold_max);
```

The number of files moved is logged and can be monitored through the duplicate finder statistics.

## Testing

Comprehensive unit tests verify:

- Cross-group checking with no groups (returns 0 moves)
- Cross-group checking with single group (returns 0 moves)
- Cross-group checking with multiple groups (basic functionality)
- Edge case handling and error conditions

## Benefits

1. **Improved Group Quality**: Files are moved to groups where they have higher similarity
2. **Flexible Control**: Threshold range allows tuning of optimization aggressiveness
3. **Automatic Operation**: No manual intervention required
4. **Performance Optimized**: Only checks files that need optimization
5. **Robust Error Handling**: Failed operations don't corrupt group integrity

## Configuration Recommendations

### Conservative (Default)

```yaml
duplicates.threshold.min: 0.92
duplicates.threshold.max: 0.96
```

### Aggressive Optimization

```yaml
duplicates.threshold.min: 0.88
duplicates.threshold.max: 0.94
```

### Minimal Optimization

```yaml
duplicates.threshold.min: 0.94
duplicates.threshold.max: 0.98
```

The optimal settings depend on your image collection characteristics and quality requirements.
