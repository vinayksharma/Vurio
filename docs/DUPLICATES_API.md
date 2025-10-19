# Duplicate Groups API

## Overview

REST API endpoint for retrieving paginated duplicate groups with their representative and candidate files (all as fully qualified paths).

## Endpoint

```
GET /api/v1/duplicates/groups
```

## Query Parameters

| Parameter | Type    | Required | Default | Max  | Description                         |
| --------- | ------- | -------- | ------- | ---- | ----------------------------------- |
| `start`   | integer | No       | 0       | -    | Starting index (0-based offset)     |
| `limit`   | integer | No       | 100     | 1000 | Number of groups to return per page |

### Alternative Parameter: `end`

Instead of `limit`, you can provide `end` to specify the ending index:

```
GET /api/v1/duplicates/groups?start=10&end=20
```

This will return groups from index 10 to 20 (limit = end - start = 10 groups).

## Response Format

```json
{
  "groups": [
    {
      "id": 1,
      "mode": "FAST",
      "representative": "/Users/vinaysharma/Pictures/IMG_001.jpg",
      "candidates": [
        "/Users/vinaysharma/Pictures/IMG_001_copy.jpg",
        "/Users/vinaysharma/Pictures/Backup/IMG_001.jpg"
      ],
      "similarity_threshold": 0.95,
      "member_count": 3,
      "created_at": "2025-10-18 10:30:45",
      "updated_at": "2025-10-18 10:31:20"
    },
    {
      "id": 2,
      "mode": "FAST",
      "representative": "/Users/vinaysharma/Pictures/DSC_7890.jpg",
      "candidates": ["/Users/vinaysharma/Pictures/DSC_7890_edited.jpg"],
      "similarity_threshold": 0.95,
      "member_count": 2,
      "created_at": "2025-10-18 10:32:15",
      "updated_at": "2025-10-18 10:32:15"
    }
  ],
  "total_count": 1523,
  "start": 0,
  "end": 100,
  "returned": 2
}
```

## Response Fields

### Root Object

| Field         | Type    | Description                                       |
| ------------- | ------- | ------------------------------------------------- |
| `groups`      | array   | Array of duplicate group objects (see below)      |
| `total_count` | integer | Total number of duplicate groups in the database  |
| `start`       | integer | Starting index of this page (from request)        |
| `end`         | integer | Ending index (start + limit)                      |
| `returned`    | integer | Actual number of groups returned in this response |

### Group Object

| Field                  | Type    | Description                                                         |
| ---------------------- | ------- | ------------------------------------------------------------------- |
| `id`                   | integer | Unique duplicate group ID                                           |
| `mode`                 | string  | Detection mode: `"FAST"` or `"QUALITY"`                             |
| `representative`       | string  | Fully qualified path to the representative (best) file              |
| `candidates`           | array   | Array of fully qualified paths to duplicate candidate files         |
| `similarity_threshold` | number  | Similarity threshold used for this group (e.g., 0.95)               |
| `member_count`         | integer | Total number of members in this group (representative + candidates) |
| `created_at`           | string  | ISO 8601 timestamp when the group was first created                 |
| `updated_at`           | string  | ISO 8601 timestamp when the group was last modified                 |

## Sorting

Results are **always sorted by `created_at` in ascending order** (oldest groups first).

This ensures consistent pagination and allows clients to:

- Get newest groups by requesting the last page
- Process groups chronologically
- Resume processing from a specific creation time

## Database Optimization

An index on `duplicate_groups(created_at)` ensures efficient pagination:

```sql
CREATE INDEX IF NOT EXISTS idx_duplicate_groups_created_at
ON duplicate_groups(created_at);
```

## Examples

### Get first 10 groups

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=10"
```

### Get groups 100-200

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=100&limit=100"
```

### Get groups using end parameter

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=50&end=75"
# Returns 25 groups (from index 50 to 74)
```

### Get maximum allowed page (1000 groups)

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1000"
```

## Error Responses

### 500 Internal Server Error

```json
{
  "error": "Internal server error",
  "message": "Failed to retrieve duplicate groups: ..."
}
```

## Representative vs Candidates

- **Representative**: The "best" file in the group (selected by file size and age)
- **Candidates**: All other duplicate files in the group
- The representative is **not included** in the `candidates` array
- `member_count` = 1 (representative) + number of candidates

## Use Cases

### Bulk Review

```bash
# Get all groups in batches of 100
for i in {0..1500..100}; do
  curl "http://localhost:8080/api/v1/duplicates/groups?start=$i&limit=100" \
    | jq '.groups[] | {id, representative, count: (.candidates | length)}'
done
```

### Find Specific Group Range

```bash
# Get groups created in a specific range
curl "http://localhost:8080/api/v1/duplicates/groups?start=1000&limit=500" \
  | jq '.groups[] | select(.mode == "QUALITY")'
```

### Extract All Duplicate Paths

```bash
# Get all duplicate candidates (excluding representatives)
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1000" \
  | jq -r '.groups[].candidates[]'
```

### Count Total Duplicates Without Fetching Data

```bash
# Get just the metadata
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1" \
  | jq '{total_groups: .total_count, this_page_returned: .returned}'
```

## Performance Characteristics

- **Query Time**: < 50ms for 100 groups (with members) using indexed `created_at`
- **Response Size**: ~500 bytes per group (varies with path lengths and candidate count)
- **Typical Page Size**: 100 groups = ~50KB response
- **Maximum Page Size**: 1000 groups = ~500KB response

## Implementation Details

- **Database**: Single query for groups + individual queries for members (N+1 pattern)
- **Indexing**: `idx_duplicate_groups_created_at` ensures fast pagination
- **Thread Safety**: Read-only operations using database session pool
- **Caching**: No caching (always returns fresh data from database)

## Future Enhancements

Potential improvements for consideration:

1. **Filtering**: Add query parameters for mode, date range, member count
2. **Sorting Options**: Allow sorting by updated_at, member_count, similarity
3. **Batch Operations**: Add endpoints for bulk actions (delete, review, keep)
4. **Streaming**: Support streaming large result sets with chunked encoding
5. **Metadata**: Include file sizes, creation dates, similarity scores in response
