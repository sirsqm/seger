#include "search.h"

#include <algorithm>
#include <iostream>

#include "eval.h"

namespace seger {

namespace {

// MVV-LVA: most valuable victim first, least valuable attacker as tie-break.
// Returns 0 for a quiet move; callers rely on that. The common quiet case is
// handled first so the function is cheap: it is called twice per move by the
// move-ordering comparator below.
int captureScore(const Position& pos, const Move& m) {
    if (m.hasFlag(MF_ENPASSANT)) {
        int score = 100 * 16 - pieceValue(PAWN);
        if (m.promotion != NO_PIECE_TYPE) score += pieceValue(m.promotion);
        return score;
    }
    if (m.promotion == NO_PIECE_TYPE) {
        Piece victim = pos.pieceOn(m.to);
        if (victim == NO_PIECE) return 0;
        return pieceValue(typeOf(victim)) * 16 -
               pieceValue(typeOf(pos.pieceOn(m.from)));
    }
    // Promotion: the destination may still hold a captured piece before the
    // pawn is replaced, so score the victim from the board, not from pieceOn(to).
    int score = pieceValue(m.promotion);
    Piece victim = pos.pieceOn(m.to);
    if (victim != NO_PIECE) {
        score += pieceValue(typeOf(victim)) * 16 -
                 pieceValue(typeOf(pos.pieceOn(m.from)));
    }
    return score;
}

// Transposition-table scores store mate distances relative to the current
// node, so they must be shifted by the ply when written and read back.
int scoreToTT(int s, int ply) {
    if (s >= MATE_IN_MAX_PLY) return s + ply;
    if (s <= -MATE_IN_MAX_PLY) return s - ply;
    return s;
}
int scoreFromTT(int s, int ply) {
    if (s >= MATE_IN_MAX_PLY) return s - ply;
    if (s <= -MATE_IN_MAX_PLY) return s + ply;
    return s;
}

}  // namespace

bool Search::timeUp() {
    if (stopped_) return true;
    if (stopRequested_.load()) {
        stopped_ = true;
        return true;
    }
    if (nodeLimit_ > 0 && nodes_ >= nodeLimit_) {
        stopped_ = true;
        return true;
    }
    if (hardDeadlineMs_ <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
    if (elapsed >= hardDeadlineMs_) {
        stopped_ = true;
        return true;
    }
    return false;
}

void Search::scoreMoves(std::vector<Move>& moves, const Move& ttMove, int ply) {
    const int k1 = pieceValue(QUEEN) * 2;
    // Rank every move once, then sort pairs. The comparator used to call
    // captureScore on both operands for every comparison, i.e. O(n log n) board
    // probes per node; precomputing pays each move a single time. Stable sorting
    // the (score, original index) pairs reproduces the old ordering exactly.
    const size_t n = moves.size();
    std::vector<std::pair<int, Move>> ranked(n);
    for (size_t i = 0; i < n; ++i) {
        const Move& m = moves[i];
        int r;
        if (m == ttMove) {
            r = 1'000'000;
        } else {
            const int cs = captureScore(pos_, m);
            if (cs != 0) r = 500'000 + cs;
            else if (m == killers_[ply][0]) r = 400'000;
            else if (m == killers_[ply][1]) r = k1;
            else r = history_[pos_.pieceOn(m.from)][m.to];
        }
        ranked[i] = {r, m};
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const std::pair<int, Move>& a, const std::pair<int, Move>& b) {
                         return a.first > b.first;
                     });
    for (size_t i = 0; i < n; ++i) moves[i] = ranked[i].second;
}

int Search::quiescence(int alpha, int beta, int ply) {
    ++nodes_;
    if ((nodes_ & 2047) == 0 && timeUp()) return alpha;

    // A capture can leave bare kings or a lone minor; then the line is dead.
    if (pos_.hasInsufficientMaterial()) return DRAW;

    const bool inCheck = pos_.isInCheck(pos_.sideToMove());
    int standPat = -INF;
    if (!inCheck) {
        standPat = evaluate(pos_);
        if (standPat >= beta) return beta;
        if (standPat > alpha) alpha = standPat;
    } else if (ply >= MAX_PLY - 1) {
        return evaluate(pos_);
    }

    std::vector<Move> moves;
    moves.reserve(32);
    // In check, search every evasion; otherwise only captures and promotions.
    pos_.generateMoves(moves, /*onlyCaptures=*/!inCheck);
    scoreMoves(moves, MOVE_NONE, ply);

    for (const Move& m : moves) {
        // Delta pruning: skip captures that cannot raise alpha even if they win
        // the victim outright.
        if (!inCheck && !isMateScore(alpha)) {
            int gain = m.hasFlag(MF_ENPASSANT) ? pieceValue(PAWN) : 0;
            if (!gain) {
                Piece victim = pos_.pieceOn(m.to);
                if (victim != NO_PIECE) gain = pieceValue(typeOf(victim));
            }
            if (m.promotion != NO_PIECE_TYPE) gain += pieceValue(m.promotion);
            if (standPat + gain + 200 <= alpha) continue;
        }
        if (!pos_.doMoveIfLegal(m)) continue;
        int score = -quiescence(-beta, -alpha, ply + 1);
        pos_.undoMove(m);
        if (stopped_) return alpha;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

int Search::search(int depth, int alpha, int beta, int ply, bool canNull) {
    // Draw by repetition, the fifty-move rule, or insufficient material.
    if (ply > 0) {
        if (pos_.fiftyMove() >= 100) return DRAW;
        if (pos_.isRepetition()) return DRAW;
        if (pos_.hasInsufficientMaterial()) return DRAW;
    }

    const bool inCheck = pos_.isInCheck(pos_.sideToMove());
    if (inCheck && ply < MAX_PLY) ++depth;  // check extension

    if (depth <= 0) return quiescence(alpha, beta, ply);
    if (ply >= MAX_PLY - 1) return evaluate(pos_);

    ++nodes_;
    if ((nodes_ & 2047) == 0 && timeUp()) return alpha;

    const int alphaOrig = alpha;

    // --- Transposition table probe ------------------------------------------
    Move ttMove = MOVE_NONE;
    if (ttEnabled_) {
        if (const TTEntry* e = tt_.probe(pos_.key())) {
            if (e->hasMove) ttMove = e->move;
            if (e->depth >= depth) {
                int s = scoreFromTT(e->score, ply);
                if (e->bound == BOUND_EXACT ||
                    (e->bound == BOUND_LOWER && s >= beta) ||
                    (e->bound == BOUND_UPPER && s <= alpha)) {
                    return s;
                }
            }
        }
    }

    const int staticEval = inCheck ? -INF : evaluate(pos_);

    // --- Reverse-futility pruning (static null move) -------------------------
    // If we are ahead of beta by more than the material that could plausibly be
    // lost in `depth` plies, assume the cutoff. The margin is deliberately tight
    // so only genuinely hopeless nodes are pruned.
    if (!inCheck && depth <= 5 && !isMateScore(alpha) && !isMateScore(beta) &&
        pos_.hasNonPawnMaterial(pos_.sideToMove())) {
        if (staticEval - 120 * depth >= beta) return staticEval;
    }

    // --- Null-move pruning ---------------------------------------------------
    // Skip when in check, in a pawn-only ending (zugzwang), or when disallowed
    // by a preceding null move.
    if (canNull && !inCheck && depth >= 3 && pos_.hasNonPawnMaterial(pos_.sideToMove())) {
        int eval = staticEval;
        if (eval >= beta) {
            int R = 2 + depth / 4;
            pos_.makeNullMove();
            int score = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false);
            pos_.undoNullMove();
            if (stopped_) return alpha;
            if (score >= beta) return beta;
        }
    }

    // --- Internal iterative deepening ---------------------------------------
    // Past the cheap cutoffs, if there is still no table move to order by, run
    // a shallower search to find one and leave it in the table, so the real
    // search does not start from board order.
    if (ttMove.isNone() && depth >= 6) {
        search(depth - 2, alpha, beta, ply, canNull);
        if (stopped_) return alpha;
        if (ttEnabled_) {
            if (const TTEntry* e = tt_.probe(pos_.key()))
                if (e->hasMove) ttMove = e->move;
        }
    }

    std::vector<Move> moves;
    moves.reserve(64);
    pos_.generateMoves(moves, false);
    scoreMoves(moves, ttMove, ply);

    Move bestMove = MOVE_NONE;
    int bestScore = -INF;
    int moveCount = 0;
    bool anyLegal = false;

    for (const Move& m : moves) {
        // Classify the move *before* it is applied: after doMove the destination
        // holds the mover, so testing then would call every move a capture and
        // silently disable the killer/history updates below.
        const bool isCapture = pos_.pieceOn(m.to) != NO_PIECE || m.hasFlag(MF_ENPASSANT) ||
                               m.promotion != NO_PIECE_TYPE;

        // Futility: at a shallow node a quiet move that cannot lift alpha even
        // with a free tempo is skipped. Never skip the first move, a check, or
        // any move while a mate is on the table. Tested before making the move
        // so a skipped move costs no make/unmake.
        if (!inCheck && !isCapture && depth <= 3 && moveCount > 0 &&
            staticEval != -INF && !isMateScore(alpha) &&
            staticEval + 120 * depth + 80 <= alpha)
            continue;

        if (!pos_.doMoveIfLegal(m)) continue;
        anyLegal = true;

        int score;
        if (moveCount == 0) {
            score = -search(depth - 1, -beta, -alpha, ply + 1, true);
        } else {
            // Late move reduction: with TT/killer/history ordering now working,
            // quiet moves late in the list are unlikely to be best, so scout
            // them one or two plies shallower and re-search if they surprise us.
            int R = 0;
            if (depth >= 3 && moveCount >= 4 && !isCapture && !inCheck && m != ttMove)
                R = 1 + (moveCount >= 8 ? 1 : 0);
            score = -search(depth - 1 - R, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && R > 0)
                score = -search(depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && score < beta)
                score = -search(depth - 1, -beta, -alpha, ply + 1, true);
        }
        pos_.undoMove(m);
        ++moveCount;

        if (stopped_) return alpha;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score >= beta) {
            if (!isCapture) {
                if (killers_[ply][0] != m) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = m;
                }
                history_[pos_.pieceOn(m.from)][m.to] += depth * depth;
            }
            break;
        }
        if (score > alpha) alpha = score;
    }

    if (!anyLegal) {
        if (inCheck) return -MATE + ply;  // checkmate
        return DRAW;                      // stalemate
    }

    // --- Transposition table store ------------------------------------------
    if (ttEnabled_ && !stopped_) {
        BoundType bound = BOUND_EXACT;
        if (bestScore <= alphaOrig) bound = BOUND_UPPER;
        else if (bestScore >= beta) bound = BOUND_LOWER;
        tt_.store(pos_.key(), scoreToTT(bestScore, ply), depth, bound, bestMove);
    }

    return bestScore;
}

Move Search::think(const SearchLimits& limits) {
    start_ = std::chrono::steady_clock::now();
    stopped_ = false;
    stopRequested_.store(false);
    nodes_ = 0;
    nodeLimit_ = limits.nodes;

    std::fill(&killers_[0][0], &killers_[0][0] + MAX_PLY * 2, MOVE_NONE);
    for (auto& row : history_) for (int& v : row) v = 0;

    // Time management: budget a slice of the remaining clock.
    hardDeadlineMs_ = 0;
    softDeadlineMs_ = 0;
    if (limits.movetime > 0) {
        // A fixed move time is treated as both the soft and hard ceiling; there
        // is no extension, but iteration should run until the budget is used.
        hardDeadlineMs_ = limits.movetime;
        softDeadlineMs_ = limits.movetime;
    } else if (limits.timeLeft > 0) {
        // Scale the slice with the amount of time left: spend more of it when
        // there is plenty, and less when the clock is short, so a lost position
        // or a low clock does not run out mid-search. A sudden-death game keeps
        // a safety margin; a fixed move count divides evenly.
        int estMoves = (limits.movestogo > 0) ? limits.movestogo : 24;
        double scale = 1.0;
        const int tl = limits.timeLeft;
        if (tl < 2000)        scale = 0.40;
        else if (tl < 5000)   scale = 0.55;
        else if (tl < 15000)  scale = 0.75;
        else                  scale = 1.00;
        if (limits.movestogo > 0) scale = 1.0;  // respect the explicit control

        int budget = (int)((double)tl / estMoves * scale) + limits.increment / 2;
        int cap = (limits.movestogo > 0) ? tl : tl / 4;  // never reserve too much
        if (budget > cap) budget = cap;
        if (budget < 1) budget = 1;
        softDeadlineMs_ = budget;
        hardDeadlineMs_ = budget * 3;
        if (hardDeadlineMs_ > tl - 30 && limits.movestogo == 0) hardDeadlineMs_ = std::max((int64_t)1, (int64_t)tl - 30);
        if (hardDeadlineMs_ < 1) hardDeadlineMs_ = 1;
    } else if (limits.infinite) {
        hardDeadlineMs_ = 0;  // rely on "stop"
    }

    std::vector<Move> rootMoves;
    rootMoves.reserve(64);
    pos_.generateLegalMoves(rootMoves);
    if (rootMoves.empty()) return MOVE_NONE;

    Move best = rootMoves.front();
    Move prevBest = MOVE_NONE;   // best at depth-1 (for stability detection)
    Move prevBest2 = MOVE_NONE;  // best at depth-2
    int maxDepth = limits.useDepth ? limits.depth : MAX_PLY;
    int prevScore = DRAW;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        const auto iterStart = std::chrono::steady_clock::now();
        // Aspiration window around the previous score (widened on a fail).
        int delta = 25;
        int winAlpha = -INF, winBeta = INF;  // window actually searched this pass

        Move iterBest = MOVE_NONE;
        int bestScore = -INF;
        const Move ttRootMove = [&]() -> Move {
            uint64_t k = pos_.key();
            const TTEntry* e = ttEnabled_ ? tt_.probe(k) : nullptr;
            return (e && e->hasMove) ? e->move : MOVE_NONE;
        }();
        scoreMoves(rootMoves, ttRootMove, 0);

        auto searchRoot = [&]() {
            winAlpha = (depth >= 5 && !isMateScore(prevScore)) ? std::max(-INF, prevScore - delta) : -INF;
            winBeta = (depth >= 5 && !isMateScore(prevScore)) ? std::min(INF, prevScore + delta) : INF;
            int alpha = winAlpha, beta = winBeta;
            iterBest = MOVE_NONE;
            bestScore = -INF;
            int mc = 0;
            for (const Move& m : rootMoves) {
                pos_.doMove(m);
                int score;
                if (mc == 0) {
                    score = -search(depth - 1, -beta, -alpha, 1, true);
                } else {
                    score = -search(depth - 1, -alpha - 1, -alpha, 1, true);
                    if (score > alpha && score < beta)
                        score = -search(depth - 1, -beta, -alpha, 1, true);
                }
                pos_.undoMove(m);
                ++mc;
                if (stopped_) return;
                if (score > bestScore) {
                    bestScore = score;
                    iterBest = m;
                }
                if (score > alpha) alpha = score;
            }
        };

        // Re-search with progressively wider aspiration windows on failure. The
        // window this pass searched must be captured before searchRoot() mutates
        // alpha/beta, otherwise the fail-low test below is true on every pass.
        for (int guard = 0; guard < 4; ++guard) {
            searchRoot();
            if (stopped_) break;
            if (iterBest.isNone()) break;
            if (bestScore <= winAlpha && winAlpha > -INF) { delta *= 4; continue; }
            if (bestScore >= winBeta && winBeta < INF) { delta *= 4; continue; }
            break;
        }

        if (stopped_ || iterBest.isNone()) break;
        best = iterBest;
        prevScore = bestScore;

        if (ttEnabled_) tt_.store(pos_.key(), scoreToTT(bestScore, 0), depth, BOUND_EXACT, best);

        if (out_) {
            int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start_).count();
            int64_t nps = elapsed > 0 ? (nodes_ * 1000 / elapsed) : 0;

            // Build the principal variation by walking the transposition table.
            std::string pv = moveToString(best);
            std::vector<Move> undoStack;
            pos_.doMove(best);
            undoStack.push_back(best);
            uint64_t seen[64];
            int seenN = 0;
            for (int i = 1; i < depth && i < MAX_PLY; ++i) {
                const TTEntry* e = ttEnabled_ ? tt_.probe(pos_.key()) : nullptr;
                if (!e || !e->hasMove) break;
                Move pm = e->move;
                bool dup = false;
                for (int j = 0; j < seenN; ++j) if (seen[j] == pos_.key()) dup = true;
                if (dup) break;
                if (seenN < 64) seen[seenN++] = pos_.key();
                if (!pos_.isLegalMove(pm)) break;
                pos_.doMove(pm);
                undoStack.push_back(pm);
                pv += ' ';
                pv += moveToString(pm);
            }
            for (auto it = undoStack.rbegin(); it != undoStack.rend(); ++it) pos_.undoMove(*it);

            int shown = bestScore;
            if (isMateScore(shown))
                shown = (shown > 0 ? (MATE - shown + 1) / 2 : -(MATE + shown + 1) / 2);
            *out_ << "info depth " << depth << " score "
                  << (isMateScore(bestScore) ? "mate " : "cp ") << shown
                  << " nodes " << nodes_ << " nps " << nps
                  << " time " << elapsed << " pv " << pv << "\n";
            out_->flush();
        }

        if (isMateScore(bestScore)) break;
        if (limits.useDepth && depth >= limits.depth) break;
        if (nodeLimit_ > 0 && nodes_ >= nodeLimit_) break;
        if (hardDeadlineMs_ > 0) {
            int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start_).count();
            int64_t iterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - iterStart).count();
            // Estimate the next iteration at ~1.7x the one just finished, then
            // only continue if it fits inside the soft budget, or (when the
            // best move has been stable for several depths) the hard budget.
            bool stable = (depth >= 6 && best == prevBest && best == prevBest2);
            int64_t limit = stable ? hardDeadlineMs_ : softDeadlineMs_;
            int64_t projected = elapsed + iterMs * 7 / 4;
            if (projected >= limit) break;
        }
        prevBest2 = prevBest;
        prevBest = best;
        if (stopped_) break;
    }

    return best;
}

}  // namespace seger
