# Multi-Instance Indexing Fix Plan

## Problem

`codebase-memory-mcp` currently runs as an MCP server over stdio. Each MCP client or agent session can spawn its own process. Multiple processes then share the same cache directory and project `.db` files without a cross-process coordination mechanism.

Observed failure modes:

- More than one `codebase-memory-mcp.exe` process can run at the same time.
- `cbm_pipeline_lock()` uses a process-local `static atomic_int`, so it only serializes threads inside one process.
- Indexing writes directly to the final database path with `fopen(path, "wb")`.
- One process can query or cache a SQLite handle while another process deletes, truncates, or rewrites the same database.
- Re-indexing can leave empty, stale, corrupt, or partially visible databases when processes overlap.

## Goals

- Make indexing and project deletion safe when multiple MCP stdio server processes are running.
- Prevent partially written database files from becoming visible to readers.
- Keep normal graph queries fast and mostly lock-free.
- Treat the graph database as a cache: source files remain the source of truth.
- Accept eventual consistency when agents edit source files while indexing is running.

## Non-Goals

- Do not implement a full MCP-over-HTTP shared daemon in this fix.
- Do not lock source repository files or block coding agents from editing files.
- Do not attempt to create a perfectly atomic whole-repository filesystem snapshot.
- Do not redesign the storage schema unless required by the lock/publish changes.

## Recommended Fix

### 1. Add A Cross-Process Index Lock

Create a process-wide lock in the cache directory, for example:

```text
<cache-dir>/_index.lock
```

Use this lock for operations that mutate project database files:

- `index_repository`
- watcher-triggered reindex
- UI-triggered `/api/index`
- `delete_project`
- any future compaction or migration command that rewrites project databases

Platform behavior:

- Windows: use a named mutex or `LockFileEx` on the lock file.
- Linux/macOS: use `flock` or `fcntl` advisory locking on the lock file.

Implementation shape:

- Add a small `foundation/process_lock.{c,h}` module.
- Expose blocking and try-lock APIs:

```c
typedef struct cbm_process_lock cbm_process_lock_t;

int cbm_process_lock_acquire(const char *path, cbm_process_lock_t **out);
int cbm_process_lock_try_acquire(const char *path, cbm_process_lock_t **out);
void cbm_process_lock_release(cbm_process_lock_t *lock);
```

Thread/process layering:

- Keep the existing atomic lock for same-process serialization.
- Wrap it with the process lock for mutating operations.
- Avoid holding the process lock for read-only graph queries.

Expected behavior:

- Manual `index_repository` should block until any other indexing/deletion operation finishes.
- Watcher reindex should use try-lock and skip/retry later if another process is busy.
- Delete should block or return a busy error consistently, not race with indexing.

### 2. Publish Databases Atomically

Stop writing directly to the final project database path.

Current risky behavior:

```c
FILE *fp = fopen(path, "wb");
```

Target behavior:

1. Build graph in memory as today.
2. Write SQLite file to a unique temp path in the same directory:

```text
<project>.db.tmp.<pid>.<counter>
```

3. Open/check the temp database before publishing.
4. Persist file hashes and FTS backfill against the temp database.
5. Close all handles to the temp database.
6. Under the cross-process lock, atomically replace the final path.
7. Remove stale `-wal` and `-shm` files for the final path before/after replacement.
8. Clean up temp files on failure.

Platform behavior:

- POSIX: `rename(temp, final)` is atomic when temp and final are on the same filesystem.
- Windows: use `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING` or `ReplaceFile`.
- On Windows, replacement may fail if another process has the DB open without compatible sharing. If that happens, return a clear busy/retryable error and keep the old DB intact.

Important rule:

- Never expose a newly indexed database at the final path until the file is complete and has passed basic integrity checks.

### 3. Handle Cached Query Stores Safely

Each MCP process caches one open `cbm_store_t` per current project. That cache can become stale after another process publishes a new DB.

Minimal safe approach:

- Track the database file mtime/size when `resolve_store()` opens it.
- Before using a cached store, stat the DB path.
- If mtime/size changed, close and reopen the store.
- If reopen fails because a replace is in progress, return a clear temporary error.

Optional stronger approach:

- Add a `metadata` table value such as `generation` or `indexed_at` and compare it after reopen.

This prevents long-lived MCP processes from serving stale graph data indefinitely.

### 4. Add Source Dirty-Check And Retry

The DB lock and atomic publish solve database corruption, but they do not make source files a perfect snapshot. Agents may edit files during indexing.

Use an eventual-consistency strategy:

1. At the start of indexing, capture a lightweight source fingerprint.
2. Before publishing, capture the fingerprint again.
3. If it changed, either:
   - publish but mark the index as `stale`, or
   - retry indexing once after a short debounce delay.

Suggested first version:

- For git repositories, fingerprint `HEAD` plus a concise worktree status signal.
- For non-git repositories, fingerprint discovered file count plus aggregate mtime/size.
- Retry once only to avoid infinite indexing loops while agents are actively editing.

Do not block edits to source files.

### 5. Debounce Auto-Index

Watcher-triggered indexing should avoid running while files are still being written.

Recommended behavior:

- Keep watcher try-lock behavior.
- Add a quiet window before indexing, for example 1-3 seconds after the latest detected change.
- If more changes arrive during the quiet window, restart the timer.

This reduces partial-parse graphs from half-written source files.

## Suggested Implementation Order

1. Add `foundation/process_lock.{c,h}` with Windows and POSIX implementations.
2. Add tests for lock acquire, try-acquire, release, and cross-process contention if the test harness supports subprocesses.
3. Wrap `handle_index_repository`, `handle_delete_project`, watcher reindex, and UI index subprocess path with the process lock.
4. Change full index publish to write temp DB first, validate it, then atomic replace final DB.
5. Change incremental index path to use the same temp-and-replace publish model, or temporarily force full indexing when concurrent safety cannot be guaranteed.
6. Add cached store invalidation based on DB mtime/size.
7. Add source dirty-check and one retry before publish.
8. Add watcher debounce.

## Tests

Add or update tests for:

- Two concurrent `index_repository` calls for the same repo serialize correctly.
- `delete_project` waits for or fails cleanly while indexing is active.
- A failed temp DB write leaves the old final DB untouched.
- Atomic publish replaces an old valid DB with a new valid DB.
- Cached query store reopens after another process publishes a new DB.
- Watcher reindex skips when the process lock is already held.
- Source dirty-check detects a file changed during indexing and triggers one retry or stale marker.

Manual smoke tests:

- Start two MCP clients that both call `index_repository` on the same repo.
- Start one client querying while another re-indexes the same repo.
- On Windows, verify replacement behavior while another process has the DB open.
- On Linux/macOS, verify `flock`/`fcntl` behavior across separate CLI processes.

## Operational Guidance Until Fixed

- Avoid running multiple agents that call `index_repository` or `delete_project` at the same time.
- Disable auto-index when using multiple MCP clients simultaneously.
- If the graph returns empty or stale results, kill old MCP server processes and re-index.
- Query-only concurrent usage is lower risk, but querying during re-index can still see stale data until store invalidation exists.

## Future Option: Shared MCP Daemon

A larger follow-up would be a true shared MCP daemon:

- One long-lived local process owns indexing, watcher, and store cache.
- MCP clients connect via a shared transport instead of each spawning a stdio server.
- This would reduce duplicate memory usage and remove many multi-process races.

This is not required for the immediate fix. Cross-process locking plus atomic DB publish is the smaller and more practical first step.
