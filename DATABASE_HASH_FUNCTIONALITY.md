# Database Hash Functionality

## Overview

The dedup server now includes functionality to generate SHA256 hashes of database table contents. This feature allows you to:

- Get a hash of all database tables combined
- Get a hash of individual database tables
- Detect changes in database content by comparing hashes
- Verify database integrity and consistency

## API Endpoints

### 1. Get Database Hash

**Endpoint:** `GET /api/database/hash`

**Description:** Retrieves a SHA256 hash of all database table contents combined.

**Authentication:** Required (JWT Bearer token)

**Response:**

```json
{
  "status": "success",
  "database_hash": "b5d9b737e735db9b9a21a0581ea2b39d68387c472922c300db0dea51bab39eb1"
}
```

**Example:**

```bash
curl -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer YOUR_JWT_TOKEN"
```

### 2. Get Table Hash

**Endpoint:** `GET /api/database/table/{table_name}/hash`

**Description:** Retrieves a SHA256 hash of a specific database table's contents.

**Parameters:**

- `table_name` (path parameter): Name of the database table to hash

**Authentication:** Required (JWT Bearer token)

**Response:**

```json
{
  "status": "success",
  "table_name": "scanned_files",
  "table_hash": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
}
```

**Example:**

```bash
curl -X GET "http://localhost:8080/api/database/table/scanned_files/hash" \
  -H "Authorization: Bearer YOUR_JWT_TOKEN"
```

## Implementation Details

### Hash Generation Process

1. **Table Data Extraction**: All rows and columns from the table are read
2. **Data Serialization**: Data is converted to a string representation:
   - `NULL` values → `"NULL"`
   - Integers → Direct string representation
   - Floats → Direct string representation
   - Text → Direct string content
   - BLOBs → SHA256 hash of binary data (prefixed with "BLOB:")
3. **Row Formatting**: Each row is formatted as `col1|col2|col3|...`
4. **Final Hashing**: The entire serialized table data is hashed using SHA256

### Database Hash Process

1. **Table Discovery**: All tables in the database are identified
2. **Combined Serialization**: Each table's data is serialized with table markers:
   ```
   TABLE:table_name
   row1_data
   row2_data
   ...
   END_TABLE:table_name
   ```
3. **Final Hashing**: The combined serialization is hashed using SHA256

### Error Handling

- **Non-existent tables**: Returns error with descriptive message
- **Database connection issues**: Returns appropriate error messages
- **Authentication failures**: Returns 401 Unauthorized

## Use Cases

### 1. Database Integrity Verification

```bash
# Get initial hash
INITIAL_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | jq -r '.database_hash')

# After operations...
NEW_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | jq -r '.database_hash')

if [ "$INITIAL_HASH" != "$NEW_HASH" ]; then
    echo "Database content has changed"
fi
```

### 2. Table-Specific Change Detection

```bash
# Monitor specific table changes
TABLE_HASH=$(curl -s -X GET "http://localhost:8080/api/database/table/scanned_files/hash" \
  -H "Authorization: Bearer $TOKEN" | jq -r '.table_hash')
```

### 3. Backup Verification

```bash
# Verify backup integrity by comparing hashes
BACKUP_HASH="expected_hash_here"
CURRENT_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | jq -r '.database_hash')

if [ "$BACKUP_HASH" = "$CURRENT_HASH" ]; then
    echo "Database integrity verified"
else
    echo "Database content differs from backup"
fi
```

## Technical Notes

### Hash Characteristics

- **Deterministic**: Same data always produces the same hash
- **Order-dependent**: Row order affects the hash (uses `ORDER BY rowid`)
- **Content-sensitive**: Any change in data will produce a different hash
- **SHA256**: Uses industry-standard SHA256 algorithm for security

### Performance Considerations

- **Large tables**: Hashing large tables may take time
- **Memory usage**: Entire table content is loaded into memory for hashing
- **Network overhead**: Hash generation happens server-side, only hash is transmitted

### Security

- **Authentication required**: All endpoints require valid JWT token
- **No data exposure**: Only hash values are returned, not actual data
- **Input validation**: Table names are validated against existing tables

## Testing

A test script `tests/scripts/test_database_hash.sh` is provided to demonstrate the functionality:

```bash
./tests/scripts/test_database_hash.sh
```

This script:

1. Starts the server
2. Authenticates and gets a token
3. Tests database hash retrieval
4. Tests individual table hash retrieval
5. Tests error handling
6. Demonstrates hash changes when data is added

## OpenAPI Documentation

The endpoints are fully documented in the OpenAPI specification and available through the Swagger UI at:

```
http://localhost:8080/docs
```

## Database Tables

The following tables are available for hashing:

- `scanned_files` - Files discovered during scanning
- `user_inputs` - User-provided input data
- `media_processing_results` - Results from media processing
- `cache_map` - Cache mapping for transcoded files
- `flags` - System flags and state information

## Example Output

```bash
# Database hash
{
  "status": "success",
  "database_hash": "b5d9b737e735db9b9a21a0581ea2b39d68387c472922c300db0dea51bab39eb1"
}

# Table hash
{
  "status": "success",
  "table_name": "scanned_files",
  "table_hash": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
}

# Error response
{
  "error": "Failed to get table hash: Table does not exist: nonexistent_table"
}
```
