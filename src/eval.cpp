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
// centipawns and are applied on top of the piece value. The middlegame and
// endgame tables are tuned separately (PeSTO's tables, which pair with the
// PeSTO material values and phase weights above), so every piece type uses a
// different array per phase rather than sharing one shape.
// ---------------------------------------------------------------------------
constexpr int PAWN_MG_TABLE[64] = {
       0,    0,    0,    0,    0,    0,    0,    0,
      43,   84,   93,   81,   81,   93,   84,   43,
     -13,   16,   41,   48,   48,   41,   16,  -13,
     -19,   15,    9,   22,   22,    9,   15,  -19,
     -26,    4,    0,   14,   14,    0,    4,  -26,
     -19,   14,   -1,   -4,   -4,   -1,   14,  -19,
     -29,   18,    2,  -19,  -19,    2,   18,  -29,
       0,    0,    0,    0,    0,    0,    0,    0,
};
constexpr int PAWN_EG_TABLE[64] = {
       0,    0,    0,    0,    0,    0,    0,    0,
     182,  169,  145,  140,  140,  145,  169,  182,
      89,   91,   69,   61,   61,   69,   91,   89,
      24,   20,    8,    1,    1,    8,   20,   24,
       6,    6,   -6,   -7,   -7,   -6,    6,    6,
      -2,    3,   -6,    0,    0,   -6,    3,   -2,
       3,    5,    4,   11,   11,    4,    5,    3,
       0,    0,    0,    0,    0,    0,    0,    0,
};
constexpr int KNIGHT_MG_TABLE[64] = {
    -137,  -52,  -66,    6,    6,  -66,  -52, -137,
     -45,  -17,   67,   29,   29,   67,  -17,  -45,
      -2,   66,   83,   74,   74,   83,   66,   -2,
       6,   17,   44,   45,   45,   44,   17,    6,
     -11,   12,   17,   20,   20,   17,   12,  -11,
     -20,    8,   14,   14,   14,   14,    8,  -20,
     -24,  -34,    3,   -2,   -2,    3,  -34,  -24,
     -64,  -20,  -43,  -25,  -25,  -43,  -20,  -64,
};
constexpr int KNIGHT_EG_TABLE[64] = {
     -79,  -51,  -20,  -30,  -30,  -20,  -51,  -79,
     -39,  -16,  -25,   -6,   -6,  -25,  -16,  -39,
     -33,  -20,    0,    4,    4,    0,  -20,  -33,
     -18,    5,   16,   22,   22,   16,    5,  -18,
     -18,   -1,   16,   20,   20,   16,   -1,  -18,
     -23,  -12,   -2,   12,   12,   -2,  -12,  -23,
     -43,  -22,  -15,   -4,   -4,  -15,  -22,  -43,
     -47,  -51,  -21,  -19,  -19,  -21,  -51,  -47,
};
constexpr int BISHOP_MG_TABLE[64] = {
     -19,    5,  -62,  -31,  -31,  -62,    5,  -19,
     -37,   17,   20,    8,    8,   20,   17,  -37,
      -9,   37,   46,   37,   37,   46,   37,   -9,
      -3,    6,   28,   43,   43,   28,    6,   -3,
      -1,   11,   12,   30,   30,   12,   11,   -1,
       5,   16,   21,   14,   14,   21,   16,    5,
       2,   24,   18,    3,    3,   18,   24,    2,
     -27,  -21,  -13,  -17,  -17,  -13,  -21,  -27,
};
constexpr int BISHOP_EG_TABLE[64] = {
     -19,  -19,  -10,   -8,   -8,  -10,  -19,  -19,
     -11,   -4,   -3,   -8,   -8,   -3,   -4,  -11,
       3,   -4,    3,   -2,   -2,    3,   -4,    3,
      -1,    6,   11,   11,   11,   11,    6,   -1,
      -8,    0,   11,   13,   13,   11,    0,   -8,
     -14,   -5,    5,   11,   11,    5,   -5,  -14,
     -21,  -17,   -8,    1,    1,   -8,  -17,  -21,
     -20,   -7,  -20,   -7,   -7,  -20,   -7,  -20,
};
constexpr int ROOK_MG_TABLE[64] = {
      37,   36,   20,   57,   57,   20,   36,   37,
      35,   29,   62,   71,   71,   62,   29,   35,
       5,   40,   35,   26,   26,   35,   40,    5,
     -22,  -10,   21,   25,   25,   21,  -10,  -22,
     -30,  -10,  -10,    4,    4,  -10,  -10,  -30,
     -39,  -15,   -8,   -7,   -7,   -8,  -15,  -39,
     -58,  -11,   -5,   -5,   -5,   -5,  -11,  -58,
     -23,  -25,    4,   16,   16,    4,  -25,  -23,
};
constexpr int ROOK_EG_TABLE[64] = {
       9,    9,   15,   13,   13,   15,    9,    9,
       7,   10,    8,    4,    4,    8,   10,    7,
       2,    1,    2,    4,    4,    2,    1,    2,
       3,    1,    7,    1,    1,    7,    1,    3,
      -4,   -2,    1,   -1,   -1,    1,   -2,   -4,
     -10,   -4,   -9,   -4,   -4,   -9,   -4,  -10,
      -5,   -9,   -5,   -4,   -4,   -5,   -9,   -5,
     -15,    3,   -5,   -3,   -3,   -5,    3,  -15,
};
constexpr int QUEEN_MG_TABLE[64] = {
       8,   21,   36,   35,   35,   36,   21,    8,
      15,   -6,   26,   -8,   -8,   26,   -6,   15,
      22,   15,   31,   18,   18,   31,   15,   22,
     -13,  -15,    0,   -9,   -9,    0,  -15,  -13,
      -6,  -12,   -7,   -6,   -6,   -7,  -12,   -6,
      -5,    8,   -5,   -4,   -4,   -5,    8,   -5,
     -17,   -6,   13,    5,    5,   13,   -6,  -17,
     -26,  -25,  -17,   -3,   -3,  -17,  -25,  -26,
};
constexpr int QUEEN_EG_TABLE[64] = {
       5,   16,   20,   27,   27,   20,   16,    5,
      -9,   25,   28,   49,   49,   28,   25,   -9,
      -6,   12,   22,   48,   48,   22,   12,   -6,
      19,   39,   32,   51,   51,   32,   39,   19,
       2,   33,   26,   39,   39,   26,   33,    2,
      -6,   -9,   16,    7,    7,   16,   -9,   -6,
     -27,  -30,  -27,  -16,  -16,  -27,  -30,  -27,
     -37,  -24,  -27,  -24,  -24,  -27,  -24,  -37,
};
constexpr int KING_MG_TABLE[64] = {
     -26,   12,   -9,  -36,  -36,   -9,   12,  -26,
       0,  -20,  -12,   -8,   -8,  -12,  -20,    0,
     -16,   23,    4,  -18,  -18,    4,   23,  -16,
     -27,  -17,  -19,  -29,  -29,  -19,  -17,  -27,
     -50,  -17,  -36,  -43,  -43,  -36,  -17,  -50,
     -21,  -15,  -26,  -45,  -45,  -26,  -15,  -21,
       4,    8,  -12,  -54,  -54,  -12,    8,    4,
      -1,   30,   -8,  -23,  -23,   -8,   30,   -1,
};
constexpr int KING_EG_TABLE[64] = {
     -46,  -16,   -2,  -15,  -15,   -2,  -16,  -46,
      -1,   20,   26,   17,   17,   26,   20,   -1,
      11,   30,   34,   17,   17,   34,   30,   11,
      -3,   24,   28,   26,   26,   28,   24,   -3,
     -15,    2,   22,   25,   25,   22,    2,  -15,
     -14,    2,   13,   22,   22,   13,    2,  -14,
     -22,   -8,    4,   13,   13,    4,   -8,  -22,
     -48,  -29,  -18,  -20,  -20,  -18,  -29,  -48,
};

// Passed-pawn bonus by relative rank (1 = just advanced, 6 = about to promote).
constexpr int PASSED_MG[8] = {0, 5, 10, 20, 40, 70, 120, 0};
constexpr int PASSED_EG[8] = {0, 10, 20, 35, 60, 100, 160, 0};

// Pawn threats, indexed by the type of the attacked piece. A piece standing on
// a square covered by an enemy pawn is a real, cheap-to-see liability, and the
// classic "hanging piece" blunders the search otherwise only finds at greater
// depth show up here immediately. Knights and bishops hurt most (they are
// usually undefended after moving), queens least (a pawn attack on one mostly
// just forces it to move).
constexpr int THREAT_PAWN_MG[PIECE_TYPE_NB] = {0, 0, 25, 25, 12, 8, 0};
constexpr int THREAT_PAWN_EG[PIECE_TYPE_NB] = {0, 0, 22, 22, 11, 6, 0};

// Threats from a minor piece (knight or bishop) onto a more valuable enemy
// piece. The attack sets are already built for mobility, so this costs a few
// bitboard ANDs and popcounts per side and catches loose rooks and queens that
// the static score would otherwise leave for a deeper search to find.
constexpr int THREAT_MINOR_MG[PIECE_TYPE_NB] = {0, 0, 0, 0, 34, 22, 0};
constexpr int THREAT_MINOR_EG[PIECE_TYPE_NB] = {0, 0, 0, 0, 30, 18, 0};

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
    // one popcount instead of a per-square shift loop. `kingAtt` is the eight
    // neighbouring squares (a king's move set), used to build a king's "zone".
    uint64_t knightAtt[64];
    uint64_t kingAtt[64];
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
            uint64_t g = 0;
            for (int i = 0; i < 8; ++i) {
                int a = sq + rOff[i];  // king steps are the rook offsets
                if (onBoard(a)) g |= bitOf(a);
            }
            kingAtt[bi] = g;
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

// Squares a sliding piece on `sq` attacks, built with one ray mask per
// direction. The result keeps the blocker square (an enemy there is a capture)
// and drops only the squares beyond it. Mobility counts the result minus the
// piece's own colour; king safety intersects it with a king's zone.
inline uint64_t slidingAttacks(const Position& pos, int sq, PieceType pt) {
    const uint64_t occ = pos.occupiedBB();
    const int bi = bitIndexOf(sq);
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
    return attacks;
}

}  // namespace

int evaluate(const Position& pos) {
    const Color us = pos.sideToMove();
    const Color them = ~us;

    const uint64_t ourPawns = pos.pieceBB(PAWN) & pos.colorBB(us);
    const uint64_t theirPawns = pos.pieceBB(PAWN) & pos.colorBB(them);

    // Pawn-cover maps, built once and reused by the threat term below. A pawn
    // attack on a piece is worth something even when the piece is defended: a
    // pawn is the cheapest attacker, so the exchange wins material, and the
    // attacked piece must move or be traded away.
    const uint64_t whitePawns = (us == WHITE) ? ourPawns : theirPawns;
    const uint64_t blackPawns = (us == WHITE) ? theirPawns : ourPawns;
    uint64_t pawnAtt[COLOR_NB];
    pawnAtt[WHITE] = ((whitePawns & ~FILE_A_BB) << 7) | ((whitePawns & ~FILE_H_BB) << 9);
    pawnAtt[BLACK] = ((blackPawns & ~FILE_H_BB) >> 7) | ((blackPawns & ~FILE_A_BB) >> 9);

    int mg = 0, eg = 0;
    int phase = 0;
    int mob[COLOR_NB] = {0, 0};
    uint64_t passed[COLOR_NB] = {0, 0};  // passed pawns, per colour

    // --- Material + piece-square + mobility --------------------------------
    // Walk each piece type's bitboard directly instead of every occupied
    // square, so the scan does not revisit the board for a piece type and does
    // not pay a per-square switch. The arithmetic is identical to the old
    // per-square accumulation.
    //
    // Each piece's attacks are built once and kept: mobility counts them, and
    // the king-safety term below reuses them to ask how many enemy pieces bear
    // down on a king's zone, so nothing is generated twice.
    uint64_t attacks[COLOR_NB][PIECE_TYPE_NB];
    uint64_t minorAtt[COLOR_NB] = {0, 0};  // knight+bishop attack sets, per colour
    for (Color c : {us, them}) {
        const int sign = (c == us) ? 1 : -1;
        const uint64_t enemyPawns = (c == us) ? theirPawns : ourPawns;
        for (int pt = PAWN; pt <= QUEEN; ++pt) attacks[c][pt] = 0;

        const int ksq = pos.kingSquare(c);
        if (ksq != SQ_NONE) {
            const int idx = tableIndexFor(c, ksq);
            mg += sign * KING_MG_TABLE[idx];
            eg += sign * KING_EG_TABLE[idx];
            attacks[c][KING] = M.kingAtt[bitIndexOf(ksq)];
        }

        for (uint64_t bb = pos.colorBB(c) & pos.pieceBB(PAWN); bb; bb &= bb - 1) {
            const int sq = squareOfBitIndex(__builtin_ctzll(bb));
            const int idx = tableIndexFor(c, sq);
            mg += sign * (MG_VALUE[PAWN] + PAWN_MG_TABLE[idx]);
            eg += sign * (EG_VALUE[PAWN] + PAWN_EG_TABLE[idx]);
            attacks[c][PAWN] |= pawnAtt[c] & M.kingAtt[bitIndexOf(sq)];
            if (!(enemyPawns & M.passed[c][sq])) {
                const int r = relativeRank(c, sq);
                mg += sign * PASSED_MG[r];
                eg += sign * PASSED_EG[r];
                passed[c] |= bitOf(sq);
            }
        }

        for (uint64_t bb = pos.colorBB(c) & pos.pieceBB(KNIGHT); bb; bb &= bb - 1) {
            const int sq = squareOfBitIndex(__builtin_ctzll(bb));
            const int idx = tableIndexFor(c, sq);
            phase += PHASE_WEIGHT[KNIGHT];
            mg += sign * (MG_VALUE[KNIGHT] + KNIGHT_MG_TABLE[idx]);
            eg += sign * (EG_VALUE[KNIGHT] + KNIGHT_EG_TABLE[idx]);
            const uint64_t a = M.knightAtt[bitIndexOf(sq)];
            attacks[c][KNIGHT] |= a;
            minorAtt[c] |= a;
            mob[c] += popcount(a & ~pos.colorBB(c));
        }

        for (uint64_t bb = pos.colorBB(c) & pos.pieceBB(BISHOP); bb; bb &= bb - 1) {
            const int sq = squareOfBitIndex(__builtin_ctzll(bb));
            const int idx = tableIndexFor(c, sq);
            phase += PHASE_WEIGHT[BISHOP];
            mg += sign * (MG_VALUE[BISHOP] + BISHOP_MG_TABLE[idx]);
            eg += sign * (EG_VALUE[BISHOP] + BISHOP_EG_TABLE[idx]);
            const uint64_t a = slidingAttacks(pos, sq, BISHOP);
            attacks[c][BISHOP] |= a;
            minorAtt[c] |= a;
            mob[c] += popcount(a & ~pos.colorBB(c));
        }

        for (uint64_t bb = pos.colorBB(c) & pos.pieceBB(ROOK); bb; bb &= bb - 1) {
            const int sq = squareOfBitIndex(__builtin_ctzll(bb));
            const int idx = tableIndexFor(c, sq);
            phase += PHASE_WEIGHT[ROOK];
            mg += sign * (MG_VALUE[ROOK] + ROOK_MG_TABLE[idx]);
            eg += sign * (EG_VALUE[ROOK] + ROOK_EG_TABLE[idx]);
            const uint64_t a = slidingAttacks(pos, sq, ROOK);
            attacks[c][ROOK] |= a;
            mob[c] += popcount(a & ~pos.colorBB(c));
        }

        for (uint64_t bb = pos.colorBB(c) & pos.pieceBB(QUEEN); bb; bb &= bb - 1) {
            const int sq = squareOfBitIndex(__builtin_ctzll(bb));
            const int idx = tableIndexFor(c, sq);
            phase += PHASE_WEIGHT[QUEEN];
            mg += sign * (MG_VALUE[QUEEN] + QUEEN_MG_TABLE[idx]);
            eg += sign * (EG_VALUE[QUEEN] + QUEEN_EG_TABLE[idx]);
            const uint64_t a = slidingAttacks(pos, sq, QUEEN);
            attacks[c][QUEEN] |= a;
            mob[c] += popcount(a & ~pos.colorBB(c));
        }
    }

    // --- Threats: pieces attacked by an enemy pawn ---------------------------
    // The per-colour pawn attack sets were already built above as `pawnAtt`,
    // so reuse them instead of shifting the pawn bitboards a second time.
    {
        for (Color c : {us, them}) {
            const int sign = (c == us) ? -1 : 1;
            const uint64_t att = pawnAtt[~c];
            for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN}) {
                const int n = popcount(pos.pieceBB(pt) & pos.colorBB(c) & att);
                if (!n) continue;
                mg += sign * n * THREAT_PAWN_MG[pt];
                eg += sign * n * THREAT_PAWN_EG[pt];
            }
        }
    }

    // --- Threats: rooks and queens attacked by an enemy minor -----------------
    // A knight or bishop hitting a rook or queen is a real liability that the
    // static score should see: the attack sets were already built for mobility,
    // so each test is one bitboard AND. Minor-on-minor is skipped (roughly
    // even trades) and so are pawns (covered by the term above).
    {
        for (Color c : {us, them}) {
            const int sign = (c == us) ? -1 : 1;
            const uint64_t att = minorAtt[~c];
            for (PieceType pt : {ROOK, QUEEN}) {
                const int n = popcount(pos.pieceBB(pt) & pos.colorBB(c) & att);
                if (!n) continue;
                mg += sign * n * THREAT_MINOR_MG[pt];
                eg += sign * n * THREAT_MINOR_EG[pt];
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

    // --- King safety: enemy pieces bearing down on the king's zone -----------
    // The shield term above only sees the pawns in front of the king. This one
    // counts the enemy pieces whose attack set reaches the king's eight
    // neighbouring squares, weighting the more dangerous pieces higher, and
    // squares the total: a lone attacker is harmless, but two or three pieces
    // converging on the king is a real threat. Middlegame only (scaled by
    // phase), because with the heavy pieces traded there is nothing to attack
    // with and the king is a fighting piece instead.
    if (phase > 8) {
        for (Color c : {us, them}) {
            const int ksq = pos.kingSquare(c);
            if (ksq == SQ_NONE) continue;
            const uint64_t zone = M.kingAtt[bitIndexOf(ksq)];
            const uint64_t* atk = attacks[~c];
            int w = 0;
            if (atk[PAWN] & zone)   w += 1;
            if (atk[KNIGHT] & zone) w += 2;
            if (atk[BISHOP] & zone) w += 2;
            if (atk[ROOK] & zone)   w += 3;
            if (atk[QUEEN] & zone)  w += 5;
            if (w) {
                const int sign = (c == us) ? -1 : 1;
                int safety = w * w * 2;
                if (safety > 128) safety = 128;
                mg += sign * safety;
            }
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
