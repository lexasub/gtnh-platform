# War Stories — Bugs Encountered

> Collection of non-obvious bugs, kernel quirks, and subtle logic errors found during development.
> Every entry includes: symptoms → root cause → fix → how to avoid.

---

## 1. Kernel io_uring Zero-Read Bug

**Symptoms:**
- Client's poll loop closes the connection ~2-8 seconds after connect.
- Log: `"bad header: raw_len=0 header_hex=0000000000"` appearing in bursts.
- On some kernels (observed on custom 6.x), `io_uring_prep_read()` on an empty TCP socket spuriously completes with `res=5` and a zero-filled buffer — as if 5 zero bytes were read.

**Root Cause:**
A kernel bug (io_uring + TCP) where an SQPOLL-driven read on a socket with no data available completes with 5 zero bytes instead of waiting for data. The 5 bytes are consumed by the frame parser, which sees `raw_len=0` and (in the old code) closes the connection.

**What fixed it:**
1. Switch reads from io_uring to `poll() + read()` — the write ring still uses io_uring (SQPOLL), but the read path uses plain POSIX poll + non-blocking read.
2. Add a grace period (`grace_elapsed()` — first 2 seconds after connect) that suppresses bad-header counting, because the bug often fires right after connection.
3. Track `consecutive_bad_headers_`: after grace, 3 consecutive bad headers → close the connection (real protocol corruption).

**Avoidance:**
- Always handle EAGAIN in non-blocking read loops — see Bug #2.
- Always have a grace period + counter for spurious zero-reads on new TCP connections.
- If using io_uring for reads, test on the target kernel with an idle TCP socket. Consider `poll()` + `read()` as a simpler fallback.

**Files:** `src/libs/libgtnh-net/src/io_uring_connection.cpp`

---

## 2. Spurious POLLIN After poll() — EAGAIN on read()

**Symptoms:**
- `poll()` returns `POLLIN` (indicating data available), but `read()` returns `-1` with `errno == EAGAIN`.
- Old code treated this as a fatal error and closed the connection — causing disconnect storms.

**Root Cause:**
`poll()`'s `POLLIN` is a hint, not a guarantee. On a non-blocking socket, the kernel may report readability, but by the time `read()` is called, the data has been consumed by another thread/interrupt, or the socket's internal state changed. `read()` then returns `EAGAIN`. This is standard POSIX behavior, not a kernel bug.

**What fixed it:**
Treat `EINTR` and `EAGAIN` from `read()` the same way: `continue` and retry the poll loop instead of closing.

```cpp
if (errno == EINTR || errno == EAGAIN) {
    continue;  // spurious POLLIN — retry
}
```

**Avoidance:**
- Treat `EAGAIN` after `POLLIN` as normal on non-blocking sockets. Never close on `EAGAIN`.
- Same treatment needed for both header reads and payload reads.

**Files:** `src/libs/libgtnh-net/src/io_uring_connection.cpp`

---

## 3. DDA Raycaster — Wrong px When Stepping in Y

**Symptoms:**
- The highlighted block (wireframe overlay) appears 1 block ABOVE the actual targeted block.
- Some blocks on horizontal edges cannot be highlighted (DDA terminates early due to wrong distance tracking).

**Root Cause:**
A copy-paste bug in the DDA (Digital Differential Analyzer) voxel traversal. When the DDA steps in the Y direction (lines 84-90 of `Raycaster.cpp`), the `px` position was computed as:

```cpp
px = ray.origin.x + tMaxY * dy;  // ❌
```

Used `dy` (the Y component of ray direction) instead of `dx` to compute the X position at the Y-face. The correct formula:

```cpp
px = ray.origin.x + tMaxY * dx;  // ✅
```

This didn't affect the DDA stepping logic itself (which uses `tMax` comparisons, not `px`), but corrupted the distance-squared check (`distSq`), causing the loop to terminate at the wrong block in some view angles.

**Avoidance:**
- When writing DDA traversal, keep the position-update pattern consistent across all three axes:
  - Axis X step: `px = ox + tMaxX * dx; py = oy + tMaxX * dy; pz = oz + tMaxX * dz;`
  - Axis Y step: `px = ox + tMaxY * dx; py = oy + tMaxY * dy; pz = oz + tMaxY * dz;`
  - Axis Z step: `px = ox + tMaxZ * dx; py = oy + tMaxZ * dy; pz = oz + tMaxZ * dz;`
- Always use the same `tMax{AXIS}` for ALL three components in a given step.
- Test DDA with rays at various angles, especially near-vertical and shallow grazing angles.

**Files:** `src/services/game_client/RenderLib/Utils/Raycaster.cpp`

---

## 4. Chunk Drop on IsPending Race

**Symptoms:**
- Some chunks are requested but never appear in the world.
- No error in logs — chunks silently disappear.

**Root Cause (potential):**
In `MeshManager::OnChunkData`:
```cpp
if (!world.IsPending(coord))
    return;  // silently drops the chunk
```

`World::pendingRequests_` is a `tbb::concurrent_unordered_set`. While `IsPending` (using `contains()`) and `OnChunkData` (using `unsafe_erase`) are individually thread-safe, there's a TOCTOU gap: between `IsPending` returning `true` and `OnChunkData` being called, another thread could evict the pending request (via `EvictFarChunks`), causing the chunk to be stored but its pending flag lost, or the reverse — the chunk data arrives but `IsPending` returns `false` because eviction ran first.

**Avoidance (not yet applied):**
- Use an atomic operation: "erase-if-present and return whether it was present" instead of separate `contains()` + `erase()`.
- Or move the pending-flag management server-side (gateway tracks what it sent).
- For now, the guard is kept as-is — stale chunks from an evicted area are safely ignored.

**Files:** `src/services/game_client/Cache/MeshManager.cpp`

---

## 5. IoUring SQPOLL Write Ring — 0-byte CQE on Partial Write

**Symptoms (rare):**
- A write completes with `res=0` (zero bytes written) on a valid TCP connection.
- The write ring treats it as a partial write, re-submits with the same offset, creating an infinite loop.

**Root Cause:**
Under heavy write load with SQPOLL, the io_uring completion for a write SQE may report `res=0` spuriously. The kernel hasn't actually attempted the write, but the CQE fires.

**What fixed it:**
In `on_write_complete`, if `res == 0`, skip the partial-write re-submission — just mark the write as complete and move on. Zero bytes written means nothing was sent, but re-submitting the same SQE immediately would likely produce the same result. The next write in the queue will carry the pending data.

*(Note: this was a theoretical fix discussed during implementation — verify on target kernel before relying on it.)*

**Avoidance:**
- Track write offsets independently of CQE results.
- If `res == 0` and the connection is healthy, treat it as a no-op completion rather than a partial write.

---

## 6. FlatBuffer Verification Rejects Valid Chunks

**Symptoms:**
- Log: `"NetClient: invalid ChunkData buffer"` for chunks that appear valid on the server side.
- Some chunks missing client-side.

**Root Cause:**
The `flatbuffers::Verifier` checks buffer alignment, offset bounds, and required fields. If the gateway sends a chunk with a different schema version, or if the chunk data is truncated by a partial read, verification fails silently.

**Avoidance:**
- Always verify FlatBuffer alignment: `Verifier(payload, size, /* alignment */ 1)` for byte-aligned data.
- Log the specific verification error (`Verifier::GetError()`) when available.
- Ensure all services use identical `.fbs` schemas.
- After partial reads (kernel zero-read bug), the remaining data may be misaligned — re-sync by closing and reconnecting if too many verification failures occur.

---

## Patterns Summary

| Pattern | Do | Don't |
|---------|----|-------|
| Non-blocking TCP read | Handle `EAGAIN` by retrying | Close on `EAGAIN` |
| Kernel zero-reads | Grace period + bad-header counter | Assume `read() > 0` means valid data |
| DDA voxel traversal | Use consistent `tMax` per-axis for all position components | Mix `tMax` from different axes |
| Concurrent pending checks | Atomic check-and-erase | Separate `contains()` then `erase()` |
| io_uring write completions | Handle `res == 0` as no-op | Re-submit partial writes blindly |
| FlatBuffer verification | Align buffers, check schema version | Trust wire data without verification |

---

**Last updated:** 2026-06-20
