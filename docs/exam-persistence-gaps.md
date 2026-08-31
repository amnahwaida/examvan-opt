# Exam Persistence and Duplicate-Creation Gaps

Status: updated 2026-08-31 after durable idempotency implementation.

## Resolved

1. **Idempotency is now durable.** `Idempotency-Key` is stored through `ExamStore` (memory for tests, PostgreSQL in production) using a three-phase state machine: `reserve → finalize/release`. Duplicate creates across processes or after restart are now blocked by the PostgreSQL `exam_idempotency` table, which uses `INSERT ... ON CONFLICT DO NOTHING` for atomic reservation and stores the finalized response body for replay.
2. **Concurrent cross-process create is covered.** PostgreSQL `exam_idempotency` table reservation is atomic via `ON CONFLICT DO NOTHING` + subsequent SELECT. Two concurrent same-key requests produce exactly one exam and one 409 conflict.
3. **Response replay is durable.** Finalized response body, status code, and content type are persisted in `exam_idempotency` and returned on replay, including across process restarts.
4. **R2 orphan cleanup on failed DB insert.** If `add()` fails after a successful R2 upload, the handler now calls `R2Client::remove()` (or the mock equivalent) to clean up the orphaned object before releasing the idempotency reservation.
5. **`clear_all()` resets idempotency state.** Both memory and PostgreSQL stores clear `exam_idempotency` when `clear_all()` is called, ensuring test isolation and consistent state.
6. **Pagination overflow is overflow-safe.** `offset` in list handler uses `int64_t` arithmetic with clamping, preventing signed-integer overflow for extreme page values.
7. **Idempotency tests are isolated.** Tests that rely on idempotency call `clear_exams_for_testing()` at the start, preventing cross-test contamination.

## Remaining limitations

1. **PostgreSQL field-update coverage is incomplete.** The current `ExamStorePostgres::update()` does not yet persist every `models::Exam` field, including question data, visibility/security flags, delegation, messages, and several lifecycle fields.
2. **PostgreSQL integration coverage is limited.** Existing tests cover memory store behavior and handler logic. Migration of existing databases, sequence allocation after restart, and database failure recovery require a configured PostgreSQL integration environment.
3. **`ExamStorePostgres::claim_token()` is still check-then-act.** Token uniqueness for the create flow is enforced atomically by `add()` (UNIQUE constraint) but `claim_token()` itself does not use `SELECT ... FOR UPDATE`. This is acceptable for the create path (which falls back to `add()` collision) but would be racy if called independently.
4. **`ExamStorePostgres::finalize_idempotency()` does not include the exam in its transaction.** The handler calls `add()` then `finalize_idempotency()` as separate operations. For full atomicity, both would need to share a single transaction — this requires a new store method (`create_with_idempotency()`) that is not yet implemented.
5. **Dashboard rendering still has legacy coupling.** The live dashboard continues to prefer the pre-rendered snapshot and uses string replacement for non-empty data rather than binding one canonical template path.

## Production invariants

- R2 is mandatory; startup must fail closed if required R2 initialization/configuration fails.
- PDFs must never be written to local filesystem storage.
- PostgreSQL must be the durable source of truth when enabled; do not silently fall back to an empty store.
- `ExamStoreMemory` is for tests only; production must use `ExamStorePostgres` (built with `HAS_LIBPQ` and configured with valid `DATABASE_URL`).

## Required follow-up tests

- [x] Same key/same payload replay returns identical response.
- [x] Same key/different payload returns `409` without a second exam.
- [x] Concurrent same-key requests create exactly one row.
- [x] Reservation is released on validation failure (retry permitted).
- [x] Reservation is released on token claim failure (retry permitted).
- [x] `clear_all()` resets idempotency state.
- [ ] Every `models::Exam` field survives PostgreSQL insert, reload, and update.
- [ ] Migration and sequence behavior work on an existing schema and after restart.
- [ ] Canonical dashboard output renders exactly one table for empty and non-empty stores.
