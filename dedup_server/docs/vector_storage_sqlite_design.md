# Persisted Vector Storage in SQLite (sqlite-vec)

Since we can start the DB from scratch, we can add vector search **inside the same SQLite database** using the **sqlite-vec** extension. Storage is persisted in the same `.sqlite` file.

---

## 1. What is sqlite-vec?

- **SQLite extension** (loadable `.so` / `.dylib` / `.dll`) that adds vector types and KNN search.
- **vec0 virtual tables**: store vectors in a virtual table; query with `WHERE embedding MATCH ? AND k = N` for K-nearest neighbors.
- **Persistence**: vec0 tables live in your existing SQLite database file; no separate store.
- **Distance**: Supports **cosine** (we use cosine similarity), L2, L1, so we can use `distance_metric=cosine` and keep our thresholds.
- **Pure C, no extra runtime deps**; works on macOS, Linux, Windows.

---

## 2. What we need to add

### A. Dependency: sqlite-vec extension

- **Option 1 – Prebuilt:** Download the extension for your platform from [sqlite-vec releases](https://github.com/asg017/sqlite-vec/releases) (e.g. `vec0.dylib` for macOS, `vec0.so` for Linux). Put it in a known path (e.g. `dedup_server/extensions/` or next to the binary).
- **Option 2 – Build from source:** Clone [asg017/sqlite-vec](https://github.com/asg017/sqlite-vec), build the shared library, install it where the server can load it.

The app will **load** this extension at DB open time; the DB file itself stays a normal SQLite DB.

### B. Load the extension when the DB is opened

SQLite must load the extension before creating or using vec0 tables. With **Poco::Data::SQLite** we open the DB via `Session("SQLite", db_path)`. To load an extension you need the underlying `sqlite3*` handle:

- **Preferred:** If Poco exposes the native connection (e.g. via a backend accessor), use it to call `sqlite3_load_extension(db, "path/to/vec0", nullptr, nullptr)` once per connection (or once per process if connections share the same DB).
- **Alternative:** Run `SELECT load_extension('path/to/vec0');` as SQL. Many SQLite builds disable `load_extension()` by default; in that case we must either enable it at compile time or use the C API from code that has the `sqlite3*` handle.

So the **code change** is: in `DatabaseManager::initialize()` (or in the session manager when a new connection is created), after the connection is open, load the extension. That may require adding a small path that uses the native SQLite C API for that single call if Poco doesn’t expose it.

### C. Create a vec0 virtual table for embeddings

We already have embeddings in `image_artifacts` (BLOB, float32, `embedding_dim` e.g. 512). We have two design options:

**Option 1 – Single “index” table (recommended for start):**

- One vec0 table that holds **all processed file embeddings** we care about for duplicate search (e.g. group representatives + ungrouped, or simply all with `processed = 2`).
- Schema idea:
  - `rowid` or primary key = `file_id` (so we can JOIN back to `scanned_files` / `image_artifacts`).
  - One vector column, e.g. `embedding float[512]` (match `embedding_dim` from config), with `distance_metric=cosine`.
- Example (dim and metric from config):

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS vec_embedding_index USING vec0(
  file_id INTEGER PRIMARY KEY,
  embedding float[512] distance_metric=cosine
);
```

- **Persistence:** This table is part of the same SQLite DB; it’s stored in the same file as `scanned_files`, `image_artifacts`, `duplicate_groups`, etc.

**Option 2 – Separate tables for “reps” vs “ungrouped”:**

- e.g. `vec_rep_embeddings` and `vec_ungrouped_embeddings` with the same vector shape. Duplicate finder would run two KNN queries (reps, then ungrouped). More flexible for filtering; a bit more sync logic.

For a first version, Option 1 is enough: one index over “all embeddings that participate in duplicate search,” and we filter by `file_id` in `duplicate_groups` / `duplicate_members` when interpreting results.

### D. Keep the vector table in sync (single source of truth)

- **On artifact write:** When we insert/update a row in `image_artifacts` (embedding computed), we also **INSERT OR REPLACE** into the vec0 table: `(file_id, embedding)`. So the vec table is a **mirror** of the embeddings we want to search.
- **On group changes:** When we create/delete groups or change the representative, we do **not** need to change the vec table if we index “all processed embeddings” and resolve rep vs ungrouped in app logic. If we had separate “reps only” tables we’d remove/add rows when reps change.
- **Dimension:** `embedding_dim` is config-driven (e.g. 512). The vec0 table must use the same dimension; we can create it at startup from config (or recreate it if the dimension ever changes and we’re OK to rebuild).

So the **changes** are:

- Where we write to `image_artifacts`, also write the same `(file_id, embedding)` into the vec0 table.
- Optionally: when we delete or overwrite an artifact, delete or replace the corresponding row in the vec table so the index doesn’t keep stale vectors.

### E. Duplicate finder: use KNN instead of full scan

**Today:** For each new file we load all group representatives (and ungrouped) and call `computeSimilarity` in a loop.

**With vec0:**

1. Load the new file’s embedding (we already have it).
2. Run one (or two) KNN query(ies), e.g.:
   - `SELECT file_id, distance FROM vec_embedding_index WHERE embedding MATCH :embedding_blob AND k = 100`
   - Bind the new file’s embedding as `:embedding_blob`.
3. sqlite-vec returns rows ordered by **distance** (for cosine, smaller = more similar). Convert to “similarity” if needed (e.g. `similarity = 1 - distance` for cosine distance, depending on how sqlite-vec defines it).
4. Take the top candidates (e.g. top 50 or 100), then in code:
   - Resolve which of those `file_id`s are current group representatives vs ungrouped (JOIN to `duplicate_groups` / `duplicate_members` or use a small query).
   - Apply the **same** logic as today: best match above similarity threshold, duplicate vs similar tier, split, add to group, cross-batch style.

So the **only** behavioral change in the finder is **how we get the candidate set** (KNN from vec0 instead of “load everyone and loop”). Thresholds, two-tier logic, and split/redistribution stay the same; we just run them on the KNN result set.

### F. Schema / migrations

- Since we’re starting from scratch, we can:
  - Add “create vec0 table” to the same place we create other tables (e.g. in `ensureTables` or a dedicated “ensure vector index” step that runs after the extension is loaded).
  - Use `CREATE VIRTUAL TABLE IF NOT EXISTS vec_embedding_index USING vec0(...)` so it’s idempotent.
- We do **not** need to migrate old `image_artifacts` BLOBs into vec0 if we’re starting fresh: the first time we write an artifact we also write to vec0, and we can backfill vec0 from existing `image_artifacts` once at startup if we ever add the extension to an existing DB later.

---

## 3. Summary: what is needed

| Item | What to do |
|------|------------|
| **Persistence** | Use sqlite-vec **inside** the same SQLite DB; vec0 virtual table is stored in the same `.sqlite` file. No separate vector DB. |
| **Dependency** | Add the sqlite-vec loadable extension (prebuilt or built from source) and a known path for it. |
| **Load extension** | After opening the DB (in `DatabaseManager` or session manager), load the extension via `sqlite3_load_extension()` or `SELECT load_extension(...)` so vec0 is available. |
| **Create table** | Run `CREATE VIRTUAL TABLE vec_embedding_index USING vec0(file_id, embedding float[N] distance_metric=cosine)` with N = `embedding_dim`, in the same place we create other tables. |
| **Sync** | On every insert/update of an embedding in `image_artifacts`, insert/replace the same (file_id, embedding) into `vec_embedding_index`. |
| **Duplicate finder** | Replace the “load all reps / ungrouped and loop” step with “query vec_embedding_index WHERE embedding MATCH :new_embedding AND k = K”, then run existing threshold/split logic on the returned file_ids. |

That’s what’s needed to have **persisted vector storage as part of SQLite itself** and to use it for duplicate search without worrying about existing data, since we’re starting from scratch.

---

## 4. Configuration and extension path

The server uses the following config keys (see `config_manager_factory.cpp` and `config/config.yaml`):

| Key | Default | Description |
|-----|---------|-------------|
| `database.vector.enabled` | `false` | Set to `true` to enable the vector index (requires extension path). |
| `database.vector.extensionPath` | `""` | Full path to the sqlite-vec loadable extension (e.g. `vec0.dylib` on macOS, `vec0.so` on Linux). |
| `duplicates.finder.knnK` | `200` | When the vector index is enabled, only this many KNN candidates are considered per file; `0` disables KNN and uses full scan. |

**Obtaining the extension**

- **Prebuilt:** Download for your platform from [sqlite-vec releases](https://github.com/asg017/sqlite-vec/releases) (e.g. `vec0-0.1.0-x86_64-apple-darwin.zip` → `vec0.dylib`). Place the shared library in a fixed path and set `database.vector.extensionPath` to that path (e.g. `/opt/media-dedup/extensions/vec0.dylib` or relative to the server binary).
- **Build from source:** Clone [asg017/sqlite-vec](https://github.com/asg017/sqlite-vec), build the shared library (`make` or CMake), then set `database.vector.extensionPath` to the built `vec0.dylib` / `vec0.so`.

The server opens a **dedicated** SQLite connection (separate from Poco sessions) to load the extension and create/query the vec0 table; the main DB file path is the same as the rest of the app. If the extension fails to load or the vec0 table cannot be created, the vector index is left disabled and the duplicate finder falls back to the full linear scan over representatives and ungrouped files.
