#ifndef FARLANDS_GODOT_BINDINGS_CACHED_NODE_HPP
#define FARLANDS_GODOT_BINDINGS_CACHED_NODE_HPP

// ---------------------------------------------------------------------------
// Cached scene nodes, held as instance IDs.
//
// A node a binding caches can be freed while the binding itself lives on: a
// scene rebuild, a camera swap, the editor reloading the 3D viewport, or a
// sibling replaced by hand. A raw pointer cached in `_ready()` becomes a
// use-after-free on the next frame the moment that happens, and the failure is
// silent — the freed block usually still holds something plausible.
//
// An instance ID cannot dangle. `ObjectDB::get_instance()` answers nullptr for
// an object that has been freed, and `Object::cast_to` answers nullptr for an ID
// that an object of some other type has since reused, so a stale cache degrades
// to "not found" and the caller re-resolves from the scene. That is the whole
// contract: cache an ID, resolve on every use, re-cache when it resolves to
// nullptr.
//
// These live in one header because more than one binding needs them and three
// copies of the same four lines would eventually disagree (the ChunkManager had
// its own pair before this file existed).
// ---------------------------------------------------------------------------

#include <cstdint>

#include <godot_cpp/core/object.hpp>

namespace VoxelEngine {

// The object an ID names, or nullptr when it is gone (or was never cached). An
// ID that no longer resolves is cleared, so the caller's re-resolve path runs
// once rather than every frame.
template <typename T>
inline T* resolve_cached(uint64_t& id) {
    if (id == 0) return nullptr;
    T* resolved = godot::Object::cast_to<T>(godot::ObjectDB::get_instance(id));
    if (resolved == nullptr) id = 0;
    return resolved;
}

// Cache a node by ID. A null pointer caches nothing, which is the "not found
// yet" state the resolve above treats as absent.
template <typename T>
inline void cache_object(uint64_t& id, const T* object) {
    id = object != nullptr ? object->get_instance_id() : 0;
}

} // namespace VoxelEngine

#endif // FARLANDS_GODOT_BINDINGS_CACHED_NODE_HPP
