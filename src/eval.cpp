#include "eval.h"

#include <algorithm>

#include "types.h"

namespace seger {

namespace {

// ---------------------------------------------------------------------------
// Piece values. Middlegame and endgame values differ slightly: rooks gain as
// the board empties, while knights and queens lose a little.
// ---------------------------------------------------------------------------
constexpr int MG_VALUE[PIECE_TYPE_NB] = {0, 82, 337, 365, 477, 1025, 0};
constexpr int EG_VALUE[PIECE_TYPE_NB] = {0, 94, 281, 297, 512,  936, 0};

// Game-phase weights. The opening scores 24 and material is removed from it as
// exchanges happen, so the evaluation slides smoothly from middlegame to end.
constexpr int PHASE_WEIGHT[PIECE_TYPE_NB] = {0, 0, 1, 1, 2, 4, 0};
constexpr int TOTAL_PHASE = 24;

// ---------------------------------------------------------------------------
// Piece-square tables, from White's point of view, index 0 = a8. Values are in
// centipawns and are applied on top of the piece value. They reward central
// pawns and development in the middlegame; a separate endgame table lets the
// king walk to the centre once the queens are off.
//
// The shape follows the well-known "simplified evaluation function" tabulated
// by Tomasz Michniewski and reproduced widely for teaching.
// ---------------------------------------------------------------------------
constexpr int PAWN_TABLE[64] = {
     0,   0,   0,   0,   0,   0,   0,   0,
    60,  60,  60,  60,  60,  60,  60,  60,
    10,  10,  20,  30,  30,  20,  10,  10,
     4,   6,  10,  22,  22,  10,   6,   4,
     0,   0,   4,  16,  16,   4,   0,   0,
     4,  -4,  -8,   2,   2,  -8,  -4,   4,
     4,   8,   8, -16, -16,   8,   8,   4,
     0,   0,   0,   0,   0,   0,   0,   0,
};
constexpr int KNIGHT_TABLE[64] = {
   -50, -40, -30, -30, -30, -30, -40, -50,
   -40, -20,   0,   0,   0,   0, -20, -40,
   -30,   0,  10,  15,  15,  10,   0, -30,
   -30,   5,  15,  20,  20,  15,   5, -30,
   -30,   0,  15,  20,  20,  15,   0, -30,
   -30,   5,  10,  15,  15,  10,   5, -30,
   -40, -20,   0,   5,   5,   0, -20, -40,
   -50, -40, -30, -30, -30, -30, -40, -50,
};
constexpr int BISHOP_TABLE[64] = {
   -20, -10, -10, -10, -10, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,  10,  10,   5,   0, -10,
   -10,   5,   5,  10,  10,   5,   5, -10,
   -10,   0,  10,  10,  10,  10,   0, -10,
   -10,  10,  10,  10,  10,  10,  10, -10,
   -10,   5,   0,   0,   0,   0,   5, -10,
   -20, -10, -10, -10, -10, -10, -10, -20,
};
constexpr int ROOK_TABLE[64] = {
     0,   0,   0,   0,   0,   0,   0,   0,
    10,  15,  15,  15,  15,  15,  15,  10,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
     0,   0,   4,  10,  10,   4,   0,   0,
};
constexpr int QUEEN_TABLE[64] = {
   -20, -10, -10,  -5,  -5, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,   5,   5,   5,   0, -10,
    -5,   0,   5,   5,   5,   5,   0,  -5,
     0,   0,   5,   5,   5,   5,   0,  -5,
   -10,   5,   5,   5,   5,   5,   0, -10,
   -10,   0,   5,   0,   0,   0,   0, -10,
   -20, -10, -10,  -5,  -5, -10, -10, -20,
};
constexpr int KING_MG_TABLE[64] = {
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -20, -30, -30, -40, -40, -30, -30, -20,
   -10, -20, -20, -20, -20, -20, -20, -10,
    20,  20,   0,   0,   0,   0,  20,  20,
    20,  30,  10,   0,   0,  10,  30,  20,
};
constexpr int KING_EG_TABLE[64] = {
   -50, -40, -30, -20, -20, -30, -40, -50,
   -30, -20, -10,   0,   0, -10, -20, -30,
   -30, -10,  20,  30,  30,  20, -10, -30,
   -30, -10,  30,  40,  40,  30, -10, -30,
   -30, -10,  30,  40,  40,  30, -10, -30,
   -30, -10,  20,  30,  30,  20, -10, -30,
   -30, -30,   0,   0,   0,   0, -30, -30,
   -50, -30, -30, -30, -30, -30, -30, -50,
};

// Passed-pawn bonus by relative rank (1 = just advanced, 6 = about to promote).
constexpr int PASSED_MG[8] = {0, 5, 10, 20, 40, 70, 120, 0};
constexpr int PASSED_EG[8] = {0, 10, 20, 35, 60, 100, 160, 0};

// Threats, indexed by the type of the attacked piece. A piece standing on a
// square covered by an enemy pawn or king is a real, cheap-to-see liability,
// and the classic "hanging piece" blunders the search otherwise only finds at
// greater depth show up here immediately. Knights and bishops hurt most (they
// are usually undefended after moving), queens least (they are rarely left en
// prise and a pawn attack on one mostly just forces it to move).
constexpr int THREAT_PAWN_MG[PIECE_TYPE_NB] = {0, 0, 55, 55, 30, 18, 0};
constexpr int THREAT_PAWN_EG[PIECE_TYPE_NB] = {0, 0, 45, 45, 22, 12, 0};
constexpr int THREAT_KING_MG[PIECE_TYPE_NB] = {0, 0, 20, 20, 10,  5, 0};
constexpr int THREAT_KING_EG[PIECE_TYPE_NB] = {0, 0, 25, 25, 15,  8, 0};

constexpr uint64_t FILE_A_BB = 0x0101010101010101ULL;
constexpr uint64_t FILE_H_BB = 0x8080808080808080ULL;

// Precomputed file/span masks. Built once at static-initialisation time.
struct Masks {
    static bool onBoard(int sq) {
        return sq >= 0 && sq < BOARD_SIZE && isOnBoard(sq);
    }
    uint64_t fileMask[8];        // all squares on a file
    uint64_t adjacent[8];        // fileMask[f-1] | fileMask[f] | fileMask[f+1]
    uint64_t passed[COLOR_NB][BOARD_SIZE];  // enemy stop-squares for a pawn here

    // Attack sets by dense bit index (a1 = 0), so mobility can be counted with
    // one popcount instead of a per-square shift loop.
    uint64_t knightAtt[64];
    // Sliding rays from each square, one per direction, ending at the board
    // edge. Direction order: 0:+1 1:+8 2:+9 3:+7 4:-1 5:-8 6:-9 7:-7 (bit-index
    // steps). Directions 0..3 increase the bit index, 4..7 decrease it.
    uint64_t ray[64][8];

    Masks() {
        for (int f = 0; f < 8; ++f) fileMask[f] = 0x0101010101010101ULL << f;
        for (int f = 0; f < 8; ++f) {
            adjacent[f] = fileMask[f];
            if (f > 0) adjacent[f] |= fileMask[f - 1];
            if (f < 7) adjacent[f] |= fileMask[f + 1];
        }
        for (int c = 0; c < COLOR_NB; ++c) {
            for (int sq = 0; sq < BOARD_SIZE; ++sq) {
                if (!isOnBoard(sq)) { passed[c][sq] = 0; continue; }
                int f = fileOf(sq), r = rankOf(sq);
                uint64_t span = 0;
                for (int rr = r + 1; rr <= 7; ++rr) {
                    uint64_t rankMask = 0xFFULL << (8 * rr);
                    span |= rankMask & adjacent[f];
                }
                // For Black, a pawn moves down the board, so mirror the span.
                passed[c][sq] = (c == WHITE) ? span : 0;
                if (c == BLACK) {
                    span = 0;
                    for (int rr = 0; rr < r; ++rr) {
                        uint64_t rankMask = 0xFFULL << (8 * rr);
                        span |= rankMask & adjacent[f];
                    }
                    passed[c][sq] = span;
                }
            }
        }
        // Knight attack sets and sliding rays, keyed by dense bit index.
        static const int kOff[8] = {-18, -33, -31, -14, 14, 31, 33, 18};
        static const int rOff[8]  = {1, 16, 17, 15, -1, -16, -17, -15};
        for (int bi = 0; bi < 64; ++bi) {
            int sq = squareOfBitIndex(bi);
            uint64_t k = 0;
            for (int i = 0; i < 8; ++i) {
                int a = sq + kOff[i];
                if (onBoard(a)) k |= bitOf(a);
            }
            knightAtt[bi] = k;
            for (int d = 0; d < 8; ++d) {
                uint64_t m = 0;
                for (int to = sq + rOff[d]; onBoard(to); to += rOff[d]) m |= bitOf(to);
                ray[bi][d] = m;
            }
        }
    }
};
const Masks M;

inline int tableIndexWhite(int sq) { return (7 - rankOf(sq)) * 8 + fileOf(sq); }
inline int tableIndexBlack(int sq) { return rankOf(sq) * 8 + fileOf(sq); }
inline int tableIndexFor(Color c, int sq) {
    return c == WHITE ? tableIndexWhite(sq) : tableIndexBlack(sq);
}
inline int relativeRank(Color c, int sq) {
    return c == WHITE ? rankOf(sq) : 7 - rankOf(sq);
}

// Chebyshev (king-move) distance between two 0x88 squares.
inline int chebyshev(int a, int b) {
    const int df = fileOf(a) - fileOf(b);
    const int dr = rankOf(a) - rankOf(b);
    return std::max(df < 0 ? -df : df, dr < 0 ? -dr : dr);
}

inline int taper(int mg, int eg, int phase) {
    return (mg * phase + eg * (TOTAL_PHASE - phase)) / TOTAL_PHASE;
}

inline int popcount(uint64_t b) { return __builtin_popcountll(b); }

// Number of pseudo-legal destination squares for a sliding or knight piece,
// counted with one popcount per direction using the precomputed rays.
inline int mobilityFor(const Position& pos, Color c, int sq, PieceType pt) {
    const uint64_t own = pos.colorBB(c);
    const uint64_t occ = pos.occupiedBB();
    const int bi = bitIndexOf(sq);
    if (pt == KNIGHT)
        return popcount(M.knightAtt[bi] & ~own);

    uint64_t attacks = 0;
    const uint64_t* rays = M.ray[bi];
    auto rayAttack = [&](int d) {
        const uint64_t r = rays[d];
        const uint64_t blockers = r & occ;
        if (!blockers) return r;
        const int b = (d < 4) ? __builtin_ctzll(blockers) : 63 - __builtin_clzll(blockers);
        // Keep the blocker square itself (an enemy there is a capture); drop
        // only the squares strictly beyond it. The ray's bits are ordered along
        // the direction, so this is a bit-index range: `<= b` increasing, `>= b`
        // decreasing. Masking avoids the second, randomly-indexed ray[] load
        // the previous `r & ~M.ray[b][d]` performed per direction.
        return (d < 4) ? (r & (uint64_t)(~0ULL >> (63 - b)))
                       : (r & (uint64_t)(~0ULL << b));
    };
    if (pt == BISHOP) {
        attacks = rayAttack(2) | rayAttack(3) | rayAttack(6) | rayAttack(7);
    } else if (pt == ROOK) {
        attacks = rayAttack(0) | rayAttack(1) | rayAttack(4) | rayAttack(5);
    } else {  // QUEEN
        for (int d = 0; d < 8; ++d) attacks |= rayAttack(d);
    }
    return popcount(attacks & ~own);
}

}  // namespace

int evaluate(const Position& pos) {
    const Color us = pos.sideToMove();
    const Color them = ~us;

    const uint64_t ourPawns = pos.pieceBB(PAWN) & pos.colorBB(us);
    const uint64_t theirPawns = pos.pieceBB(PAWN) & pos.colorBB(them);

    int mg = 0, eg = 0;
    int phase = 0;
    int mob[COLOR_NB] = {0, 0};
    uint64_t passed[COLOR_NB] = {0, 0};  // our passed pawns, per colour
    const uint64_t occ = pos.occupiedBB();

    // --- Material + piece-square -------------------------------------------
    // Walk only the occupied squares: in the endgame that is a handful of
    // iterations instead of a full 64-square scan with a branch per square.
    for (uint64_t bb = occ; bb; bb &= bb - 1) {
        const int sq = squareOfBitIndex(__builtin_ctzll(bb));
        const Piece p = pos.pieceOn(sq);

        const Color c = colorOf(p);
        const PieceType pt = typeOf(p);
        const int sign = (c == us) ? 1 : -1;
        const int idx = tableIndexFor(c, sq);
        phase += PHASE_WEIGHT[pt];

        int psq = 0;
        switch (pt) {
            case PAWN:   psq = PAWN_TABLE[idx]; break;
            case KNIGHT: psq = KNIGHT_TABLE[idx]; break;
            case BISHOP: psq = BISHOP_TABLE[idx]; break;
            case ROOK:   psq = ROOK_TABLE[idx]; break;
            case QUEEN:  psq = QUEEN_TABLE[idx]; break;
            default: break;
        }

        if (pt == KING) {
            mg += sign * KING_MG_TABLE[idx];
            eg += sign * KING_EG_TABLE[idx];
        } else {
            mg += sign * (MG_VALUE[pt] + psq);
            eg += sign * (EG_VALUE[pt] + psq);
        }

        if (pt == PAWN) {
            uint64_t enemy = (c == us) ? theirPawns : ourPawns;
            if (!(enemy & M.passed[c][sq])) {
                int r = relativeRank(c, sq);
                mg += sign * PASSED_MG[r];
                eg += sign * PASSED_EG[r];
                passed[c] |= bitOf(sq);
            }
        } else if (pt == KNIGHT || pt == BISHOP || pt == ROOK || pt == QUEEN) {
            // Mobility is gathered here to avoid a second full board scan.
            mob[c] += mobilityFor(pos, c, sq, pt);
        }
    }

    // --- Threats: pieces attacked by an enemy pawn or king ------------------
    // A pawn-cover map is built once per side, then each enemy non-pawn piece is
    // tested against it with one bitboard AND per type. Pawn attacks on a piece
    // are always worth something (a pawn is the cheapest attacker, so the trade
    // wins even against a defended piece); king attacks only count on pieces not
    // defended by a pawn, where the king would otherwise be able to win them.
    {
        uint64_t pawnAtt[COLOR_NB];
        for (Color c : {us, them}) {
            const uint64_t p = pos.colorBB(c) & pos.pieceBB(PAWN);
            pawnAtt[c] = (c == WHITE)
                             ? ((p & ~FILE_A_BB) << 7) | ((p & ~FILE_H_BB) << 9)
                             : ((p & ~FILE_H_BB) >> 7) | ((p & ~FILE_A_BB) >> 9);
        }
        for (Color c : {us, them}) {
            const Color e = ~c;
            const int sign = (c == us) ? -1 : 1;  // a threat against us is bad
            const int ksq = pos.kingSquare(e);
            uint64_t kingAtt = 0;
            if (ksq != SQ_NONE) {
                const uint64_t k = bitOf(ksq);
                kingAtt = ((k & ~FILE_A_BB) << 7) | ((k & ~FILE_H_BB) << 9) |
                          ((k & ~FILE_H_BB) >> 7) | ((k & ~FILE_A_BB) >> 9) |
                          (k << 8) | (k >> 8);
            }
            const uint64_t att = pawnAtt[e] | kingAtt;
            for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN}) {
                const uint64_t targets = pos.pieceBB(pt) & pos.colorBB(c) & att;
                if (!targets) continue;
                const uint64_t pawnHits = targets & pawnAtt[e];
                const uint64_t kingHits = targets & ~pawnAtt[e] & ~pawnAtt[c] & ~kingAtt;
                mg += sign * (popcount(pawnHits) * THREAT_PAWN_MG[pt] +
                              popcount(kingHits) * THREAT_KING_MG[pt]);
                eg += sign * (popcount(pawnHits) * THREAT_PAWN_EG[pt] +
                              popcount(kingHits) * THREAT_KING_EG[pt]);
            }
        }
    }

    // --- Bishop pair --------------------------------------------------------
    {
        int usBishops = popcount(pos.pieceBB(BISHOP) & pos.colorBB(us));
        int themBishops = popcount(pos.pieceBB(BISHOP) & pos.colorBB(them));
        if (usBishops >= 2) { mg += 30; eg += 50; }
        if (themBishops >= 2) { mg -= 30; eg -= 50; }
    }

    // --- Pawn structure: doubled and isolated pawns -------------------------
    for (Color c : {us, them}) {
        const int sign = (c == us) ? 1 : -1;
        uint64_t bb = pos.colorBB(c) & pos.pieceBB(PAWN);
        for (int f = 0; f < 8; ++f) {
            int count = popcount(bb & M.fileMask[f]);
            if (count > 1) { mg += sign * -12 * (count - 1); eg += sign * -22 * (count - 1); }
            if (count > 0 && !(bb & (M.adjacent[f] & ~M.fileMask[f]))) {
                mg += sign * -14; eg += sign * -18;
            }
        }
    }

    // --- Rooks on open and semi-open files ----------------------------------
    for (Color c : {us, them}) {
        const int sign = (c == us) ? 1 : -1;
        uint64_t rooks = pos.pieceBB(ROOK) & pos.colorBB(c);
        for (; rooks; rooks &= rooks - 1) {
            int sq = squareOfBitIndex(__builtin_ctzll(rooks));
            uint64_t onFile = M.fileMask[fileOf(sq)] & pos.pieceBB(PAWN);
            if (!onFile) { mg += sign * 26; eg += sign * 12; }
            else if (!(onFile & pos.colorBB(c))) { mg += sign * 12; eg += sign * 6; }
        }
    }

    // --- King pawn shield (middlegame only) ---------------------------------
    if (phase > 8) {
        for (Color c : {us, them}) {
            const int sign = (c == us) ? 1 : -1;
            int ksq = pos.kingSquare(c);
            if (ksq == SQ_NONE) continue;
            int rank = rankOf(ksq);
            int shieldRank = (c == WHITE) ? rank + 1 : rank - 1;
            if (shieldRank < 0 || shieldRank > 7) continue;
            uint64_t pawns = pos.colorBB(c) & pos.pieceBB(PAWN);
            int shield = 0;
            for (int df = -1; df <= 1; ++df) {
                int f = fileOf(ksq) + df;
                if (f < 0 || f > 7) continue;
                if (pawns & bitOf(makeSquare(f, shieldRank))) ++shield;
            }
            mg += sign * (shield - 2) * 12;
        }
    }

    // --- Mobility -----------------------------------------------------------
    // mob[] was accumulated during the board scan above, per colour.
    mg += (mob[us] - mob[them]) * 3;
    eg += (mob[us] - mob[them]) * 2;

    // --- Passed pawns and king proximity (endgame) --------------------------
    // The passer's own king wants to escort it; the enemy king wants to catch
    // it. Only meaningful once the board is mostly empty, and scaled so the
    // term stays small relative to the passed-pawn value itself.
    if (phase <= 12) {
        for (Color c : {us, them}) {
            if (!passed[c]) continue;
            const int sign = (c == us) ? 1 : -1;
            const int ownK = pos.kingSquare(c);
            const int oppK = pos.kingSquare(~c);
            for (uint64_t bb = passed[c]; bb; bb &= bb - 1) {
                const int sq = squareOfBitIndex(__builtin_ctzll(bb));
                const int r = relativeRank(c, sq);
                const int pushSq = (c == WHITE) ? sq + 16 : sq - 16;
                const int escort = 4 * r * (7 - chebyshev(ownK, pushSq));  // own king close is good
                const int block = 2 * r * (7 - chebyshev(oppK, pushSq));   // enemy king close is bad
                eg += sign * (escort - block) / 8;
            }
        }
    }

    // Tempo: a small edge for the side that has the move.
    mg += 10;

    // `mg`/`eg` are already accumulated from the side to move's point of view
    // (each term is signed by `c == us`), so no extra colour flip is needed.
    return taper(mg, eg, phase);
}

}  // namespace seger
