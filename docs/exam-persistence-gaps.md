# Exam Persistence and Duplicate-Creation Gaps

Status: documented on 2026-08-31. These items are intentionally not represented as production-ready fixes yet.

## Known limitations

1. **Idempotency is process-local.** `Idempotency-Key` is forwarded by the HTTP adapter and the handler has a mutex-protected in-memory replay map, but the key is not persisted through `ExamStore` or PostgreSQL. A retry routed to another process, or a retry after restart, can still create a second exam.
2. **Concurrent cross-process create is not covered.** The PostgreSQL `exam_idempotency` table exists in the migration schema, but no reservation/finalization/replay transaction uses it. There is no live PostgreSQL concurrency test.
3. **Response replay is not durable.** Replay data is held in process memory, so JSON/protobuf response representation cannot be reconstructed from a persisted idempotency record after restart.
4. **PostgreSQL field-update coverage is incomplete.** The current `ExamStorePostgres::update()` does not yet persist every `models::Exam` field, including question data, visibility/security flags, delegation, messages, and several lifecycle fields.
5. **PostgreSQL integration coverage is missing.** Existing tests cover the memory store and handler behavior. Migration, sequence allocation, restart-equivalent reconstruction, token uniqueness under concurrent processes, and database failure recovery need a configured PostgreSQL integration environment.
6. **Dashboard rendering still has legacy coupling.** The live dashboard continues to prefer the pre-rendered snapshot and uses string replacement for non-empty data rather than binding one canonical template path. This can drift from the full canonical dashboard template.
7. **Production query parsing is incomplete.** API pagination currently parses the raw query string locally; robust URL decoding, exact key matching, overflow handling, and consistent admin/dashboard filtering still require implementation and tests.

## Required follow-up tests

- Same key/same payload replay after a fresh server process.
- Same key/different payload returns `409` without a second exam.
- Concurrent same-key requests across separate database connections create one row.
- R2 or database failure releases the reservation and permits retry.
- Every `models::Exam` field survives PostgreSQL insert, reload, and update.
- Migration and sequence behavior work on an existing schema and after restart.
- Canonical dashboard output renders exactly one table for empty and non-empty stores.
- JSON/protobuf/admin pagination and URL-encoded filters have equivalent semantics.

## Invariants that must remain

- R2 is mandatory; startup must fail closed if required R2 initialization/configuration fails.
- PDFs must never be written to local filesystem storage.
- PostgreSQL must be the durable source of truth when enabled; do not silently fall back to an empty store.
