#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>

#include "sediment/arena.h"

namespace sediment {

// Ordered set of unique keys. Nodes live in the caller's Arena and are never
// freed individually. Key must be trivially destructible, since destructors
// are never run. Single writer; not thread-safe.
//
// Comparator is a callable returning <0, 0, >0 like strcmp.
template <typename Key, class Comparator>
class SkipList {
 public:
  static constexpr int kMaxHeight = 12;

  // `seed` fixes the height sequence so tests are repeatable.
  SkipList(Comparator cmp, Arena* arena, uint32_t seed = 0xdeadbeef)
      : compare_(cmp),
        arena_(arena),
        head_(NewNode(Key(), kMaxHeight)),
        rng_(seed) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Requires: no entry comparing equal to `key` is already present.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || compare_(key, x->key) != 0);

    const int height = RandomHeight();
    if (height > max_height_) {
      for (int i = max_height_; i < height; ++i) prev[i] = head_;
      max_height_ = height;
    }
    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      x->SetNext(i, prev[i]->Next(i));
      prev[i]->SetNext(i, x);
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && compare_(key, x->key) == 0;
  }

 private:
  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;

    // The `height` next-pointers are stored directly after the Node itself.
    Node* Next(int level) const { return Links()[level]; }
    void SetNext(int level, Node* n) { Links()[level] = n; }

   private:
    Node** Links() const {
      return reinterpret_cast<Node**>(
          const_cast<char*>(reinterpret_cast<const char*>(this + 1)));
    }
  };

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(
        sizeof(Node) + sizeof(Node*) * static_cast<size_t>(height),
        alignof(Node));
    return new (mem) Node(key);
  }

  // Branching factor 4: each level holds ~1/4 of the level below.
  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && rng_() % 4 == 0) ++height;
    return height;
  }

  // First node >= key, or nullptr. If `prev` is non-null, fills it with the
  // predecessor at every level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_ - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next != nullptr && compare_(next->key, key) < 0) {
        x = next;
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        --level;
      }
    }
  }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  int max_height_ = 1;
  std::mt19937 rng_;
};

}  // namespace sediment
