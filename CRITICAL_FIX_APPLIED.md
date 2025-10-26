# Critical Bug Fix Applied

## Issue Found

After refactoring to single-mode, the database schema removed the `mode` column from `image_artifacts` table, but the `upsertEmbedding` function was still trying to insert the `mode` value into the database.

## Error

```
ONNX embedding computation failed
```

## Root Cause

In `src/database/image_artifacts_ops.cpp`, the `upsertEmbedding` function was:

1. Reading the `mode` field from `ImageEmbeddingRecord` (line 57)
2. Trying to bind it to the SQL statement (line 64)
3. But the SQL statement (`kUpsertImageEmbedding`) no longer includes a `mode` column in the INSERT statement

This caused a SQL binding error (too many parameters) which caused all image processing to fail.

## Fix Applied

Removed the `mode` parameter from the `upsertEmbedding` function in `src/database/image_artifacts_ops.cpp`:

- Removed line: `std::string mode = r.mode;`
- Removed line from SQL bind: `Keywords::use(mode),`

Now the SQL binding matches the actual SQL statement which has 5 parameters (file_path, embedding_model, embedding_dim, embedding, version).

## Impact

- This was preventing ALL image processing from working
- After restarting the server with this fix, images should process successfully
- You may want to reset errors and reprocess files that failed

## Files Modified

- `src/database/image_artifacts_ops.cpp` - Removed `mode` binding from `upsertEmbedding`
- Server rebuilt successfully

## Next Steps

1. Restart the server to apply the fix
2. Monitor processing to verify images are now being processed successfully
3. Optionally: Reset errors to reprocess previously failed files
