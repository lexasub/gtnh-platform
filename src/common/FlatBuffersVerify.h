#pragma once

// ──────────────────────────────────────────────────────────────────────────
// Verify-then-GetRoot for buffers that arrive off the network (gp-wyvv).
//
// WHY THIS FILE EXISTS
// --------------------
// flatbuffers::GetRoot<T>(ptr) performs NO validation whatsoever. It reads a
// root offset out of the first bytes and hands back a Table* built from
// attacker-controlled offsets. Every accessor on that table then dereferences
// pointers derived from those offsets, so a malformed or truncated buffer is
// an out-of-bounds read (or a wild pointer) rather than a rejected message.
//
// The mistake this repo actually made is subtler than "no check at all": the
// call sites LOOK guarded, because they do
//
//     auto* flow = flatbuffers::GetRoot<Protocol::EnergyFlowEvent>(data.data());
//     if (!flow || !flow->pos()) return;
//
// but GetRoot never returns null — it always manufactures a Table* from
// whatever the bytes say. So the `!flow` arm is dead code that reads as
// protection, and `flow->pos()` is the first point where the unchecked offsets
// are actually dereferenced. A null check on a pointer the parse layer cannot
// produce is not a validation gate.
//
// THE RULE
// --------
// Run a flatbuffers::Verifier over the SAME (pointer, length) you are about to
// parse, and only call GetRoot when it succeeds. This header is that gate in
// one place so the twenty-odd call sites stop re-spelling it (and stop getting
// it subtly wrong). The reference shape to copy is still
// src/apps/chunk_store/Network/IoUringChunkStoreService.cpp:71-78.
//
// This file deliberately logs nothing and links nothing: gtnh_wire is an
// INTERFACE target that pulls in cstdint only, so a wire header must not drag
// spdlog in behind it. The caller logs the rejection — it is the only layer
// that knows the topic and the service name worth putting in the message.
// ──────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

namespace gtnh::wire {

// Verifies `data` as a complete FlatBuffer whose root is a T, and returns the
// root table only when verification passed.
//
// Returns nullptr when:
//   * data is null, or
//   * size is below FLATBUFFERS_MIN_BUFFER_SIZE (the size check
//     FLATBUFFERS_MIN_BUFFER_SIZE exists because a 0-byte buffer still yields
//     a non-null Table* from GetRoot), or
//   * the buffer is malformed: a bad root offset, a vtable that does not fit
//     inside the buffer, an out-of-range string/vector/struct, or a nested
//     buffer that fails its own verification.
//
// The length MUST be the same length that bounds the buffer at the call site.
// A Verifier built over a length the caller guessed (rather than measured)
// re-opens the hole it is meant to close, because every bound check is taken
// against that length. Callers that only have a `const void*` and no length do
// not have a verifiable parse and must plumb the length through instead.
//
// On success the returned pointer aliases `data`; it is valid exactly as long
// as the buffer is, and the caller must not mutate it.
template <typename T>
[[nodiscard]] inline const T* VerifyAndGetRoot(const std::uint8_t* data,
                                               std::size_t size) {
    if (data == nullptr || size < FLATBUFFERS_MIN_BUFFER_SIZE) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(data, size);
    if (!verifier.VerifyBuffer<T>(nullptr)) {
        return nullptr;
    }
    // Only now is it safe to walk the table: every offset the accessors will
    // dereference has been bounds-checked against [data, data + size).
    return flatbuffers::GetRoot<T>(data);
}

}  // namespace gtnh::wire
