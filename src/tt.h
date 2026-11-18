#pragma once

#include <cstdint>
#include <vector>

#include "types.h"

namespace seger {

// Transposition-table entry bound types.
enum BoundType : uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,  // fail-low: no move here beats alpha
    BOUND_LOWER = 2,  // fail-high: some move here beats beta
    BOUND_EXACT = 3   // exact score, stored from a PV node
};

struct TTEntry {
    uint64_t key = 0;
    int32_t score = 0;
    int16_t depth = 0;      // signed so we can store -1 for a "no data" probe
    uint8_t bound = BOUND_NONE;
    uint8_t hasMove = 0;
    Move move{};
};

class TranspositionTable {
public:
    TranspositionTable() { resize(64); }

    // Resizes the table to roughly `mb` megabytes (rounded to a power of two).
    void resize(size_t mb);

    void clear();

    // Always stores; replaces the entry when the new search is deeper or when
    // it reaches a position from a different game (different key).
    void store(uint64_t key, int score, int depth, BoundType bound, const Move& move);

    // Returns a pointer to the entry for `key`, or nullptr if there is no entry
    // or the stored key does not match.
    const TTEntry* probe(uint64_t key) const;

    size_t sizeEntries() const { return entries_.size(); }

private:
    std::vector<TTEntry> entries_;
    size_t mask_ = 0;

    TTEntry& slot(uint64_t key) { return entries_[key & mask_]; }
    const TTEntry& slot(uint64_t key) const { return entries_[key & mask_]; }
};

}  // namespace seger
