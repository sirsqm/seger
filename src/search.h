#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <vector>

#include "position.h"
#include "tt.h"

namespace seger {

constexpr int MAX_PLY = 64;
constexpr int MATE = 30000;
constexpr int MATE_IN_MAX_PLY = MATE - MAX_PLY;
constexpr int INF = 32000;
constexpr int DRAW = 0;

// Scores at or beyond MATE_IN_MAX_PLY encode a forced mate.
inline bool isMateScore(int s) {
    int a = s < 0 ? -s : s;
    return a >= MATE_IN_MAX_PLY;
}

struct SearchLimits {
    int depth = MAX_PLY;       // fixed depth, if `useDepth`
    bool useDepth = false;
    int movetime = 0;          // milliseconds for this move
    int timeLeft = 0;          // remaining time on the clock
    int increment = 0;         // per-move increment
    int movestogo = 0;         // moves until the next time control (0 = sudden death)
    uint64_t nodes = 0;        // stop after this many nodes (0 = no node limit)
    bool infinite = false;

    bool hasTimeControl() const {
        return movetime > 0 || timeLeft > 0 || infinite || nodes > 0;
    }
};

class Search {
public:
    explicit Search(Position& pos) : pos_(pos) {}

    // Runs an iterative-deepening search and returns the best move found.
    Move think(const SearchLimits& limits);

    // Reports search progress as UCI "info" lines to `out`.
    void setInfoCallback(std::ostream& out) { out_ = &out; }

    // Requests that an in-progress search stop as soon as possible.
    void stop() { stopRequested_.store(true); }

    TranspositionTable& tt() { return tt_; }
    uint64_t lastNodes() const { return nodes_; }
    void setTTEnabled(bool on) { ttEnabled_ = on; }
    void clearTT() { tt_.clear(); }

private:
    Position& pos_;
    std::ostream* out_ = nullptr;

    std::chrono::steady_clock::time_point start_;
    int64_t hardDeadlineMs_ = 0;
    int64_t softDeadlineMs_ = 0;
    uint64_t nodeLimit_ = 0;
    bool stopped_ = false;
    mutable std::atomic<bool> stopRequested_{false};

    uint64_t nodes_ = 0;

    TranspositionTable tt_;
    bool ttEnabled_ = true;

    // Move-ordering heuristics.
    Move killers_[MAX_PLY][2];
    int history_[PIECE_NB][BOARD_SIZE];

    // Per-ply ranked (score, move) lists, reused across nodes. Each ply owns
    // one list so a node's ranking survives the recursive child calls that run
    // while it is being consumed (a single shared buffer would be clobbered).
    // The list is not sorted up front: the move loop picks the best remaining
    // entry on demand, so a node that cuts off on its first move never pays for
    // ordering the rest.
    std::vector<std::pair<int, Move>> rankedScratch_[MAX_PLY];

    // Per-ply move list, reused across nodes. Each ply owns one buffer so a
    // node's list survives the recursive child calls that run while it is
    // being iterated (a single shared buffer would be clobbered). This removes
    // the `reserve()` heap allocation that `search` and `quiescence` used to
    // pay on every interior and every capture node.
    std::vector<Move> moveScratch_[MAX_PLY];

    int search(int depth, int alpha, int beta, int ply, bool canNull);
    int quiescence(int alpha, int beta, int ply);

    bool timeUp();
    void rankMoves(const std::vector<Move>& moves, const Move& ttMove, int ply);
};

}  // namespace seger