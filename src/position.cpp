#include "position.h"

#include <cstring>
#include <iostream>
#include <sstream>

namespace seger {

namespace zobrist {
uint64_t psq[PIECE_NB][BOARD_SIZE];
uint64_t sideToMove;
uint64_t castling[CASTLING_NB];
uint64_t enPassant[BOARD_SIZE];

// Small deterministic PRNG (splitmix64) so keys are stable across runs.
static uint64_t rngState = 0x9E3779B97F4A7C15ULL;
static uint64_t nextRandom() {
    uint64_t z = (rngState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void init() {
    rngState = 0x9E3779B97F4A7C15ULL;
    for (int p = 0; p < PIECE_NB; ++p)
        for (int sq = 0; sq < BOARD_SIZE; ++sq)
            psq[p][sq] = nextRandom();
    sideToMove = nextRandom();
    for (int i = 0; i < CASTLING_NB; ++i) castling[i] = nextRandom();
    for (int sq = 0; sq < BOARD_SIZE; ++sq) enPassant[sq] = nextRandom();
}
}  // namespace zobrist

namespace {

constexpr int KnightOffsets[8] = {-18, -33, -31, -14, 14, 31, 33, 18};
constexpr int KingOffsets[8]   = {-17, -16, -15, -1, 1, 15, 16, 17};
constexpr int BishopDirs[4]    = {-17, -15, 15, 17};
constexpr int RookDirs[4]      = {-16, -1, 1, 16};

inline bool safe(int sq) { return sq >= 0 && sq < BOARD_SIZE && isOnBoard(sq); }

// Precomputed attack sets keyed by dense bit index, so `squareAttacked` answers
// with a few table lookups and bitboard ANDs instead of offset loops over the
// mailbox. `pawnAttackers[c][bi]` holds the squares a pawn of colour `c` would
// have to stand on to attack `bi` (the direction the test needs); the knight and
// king sets are symmetric so direction does not matter.
struct AttackTables {
    uint64_t pawnAttackers[COLOR_NB][64];
    uint64_t knight[64];
    uint64_t king[64];
    // Sliding rays from each square to the board edge. Direction order, in bit
    // index steps: 0:+1 1:+8 2:+9 3:+7 4:-1 5:-8 6:-9 7:-7; 0..3 increase.
    uint64_t ray[64][8];

    AttackTables() {
        static const int rOff[8] = {1, 16, 17, 15, -1, -16, -17, -15};
        for (int bi = 0; bi < 64; ++bi) {
            const int sq = squareOfBitIndex(bi);
            uint64_t k = 0, g = 0;
            for (int i = 0; i < 8; ++i) {
                int a = sq + KnightOffsets[i];
                if (safe(a)) k |= bitOf(a);
                int b = sq + KingOffsets[i];
                if (safe(b)) g |= bitOf(b);
            }
            knight[bi] = k;
            king[bi] = g;
            for (int d = 0; d < 8; ++d) {
                uint64_t m = 0;
                for (int to = sq + rOff[d]; safe(to); to += rOff[d]) m |= bitOf(to);
                ray[bi][d] = m;
            }

            uint64_t w = 0, b = 0;
            if (safe(sq - 15)) w |= bitOf(sq - 15);
            if (safe(sq - 17)) w |= bitOf(sq - 17);
            if (safe(sq + 15)) b |= bitOf(sq + 15);
            if (safe(sq + 17)) b |= bitOf(sq + 17);
            pawnAttackers[WHITE][bi] = w;
            pawnAttackers[BLACK][bi] = b;
        }
    }
};
const AttackTables AT;

}  // namespace

Position::Position() : side_(WHITE), castling_(NO_CASTLING), ep_(SQ_NONE),
                       fifty_(0), fullmove_(1), key_(0), occupied_(0),
                       nonPawn_(0) {
    board_.fill(NO_PIECE);
    byColor_[WHITE] = byColor_[BLACK] = 0;
    bishopQueen_[WHITE] = bishopQueen_[BLACK] = 0;
    rookQueen_[WHITE] = rookQueen_[BLACK] = 0;
    for (auto& b : byType_) b = 0;
    kingSq_[WHITE] = kingSq_[BLACK] = SQ_NONE;
}

void Position::putPiece(Piece p, int sq) {
    board_[sq] = p;
    if (p == NO_PIECE) return;
    uint64_t bit = bitOf(sq);
    byColor_[colorOf(p)] |= bit;
    byType_[typeOf(p)] |= bit;
    occupied_ |= bit;
    if (typeOf(p) != PAWN && typeOf(p) != KING) nonPawn_ |= bit;
    if (typeOf(p) == BISHOP || typeOf(p) == QUEEN) bishopQueen_[colorOf(p)] |= bit;
    if (typeOf(p) == ROOK || typeOf(p) == QUEEN) rookQueen_[colorOf(p)] |= bit;
    key_ ^= zobrist::psq[p][sq];
    if (typeOf(p) == KING) kingSq_[colorOf(p)] = sq;
}

void Position::removePiece(int sq) {
    Piece p = board_[sq];
    if (p == NO_PIECE) return;
    uint64_t bit = bitOf(sq);
    byColor_[colorOf(p)] &= ~bit;
    byType_[typeOf(p)] &= ~bit;
    occupied_ &= ~bit;
    nonPawn_ &= ~bit;
    bishopQueen_[colorOf(p)] &= ~bit;
    rookQueen_[colorOf(p)] &= ~bit;
    key_ ^= zobrist::psq[p][sq];
    board_[sq] = NO_PIECE;
}

void Position::movePiece(int from, int to) {
    Piece p = board_[from];
    removePiece(to);    // clear any captured piece (and its bitboards)
    removePiece(from);
    putPiece(p, to);
}

// Piece keys are maintained incrementally: putPiece/removePiece XOR the
// (piece, square) key in and out as pieces appear and disappear, so make/unmake
// never rescans the board. Side/castling/en-passant keys are applied by the
// callers (setFen, doMove, undoMove, null move).

void Position::setStartpos() {
    setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool Position::setFen(const std::string& fen) {
    board_.fill(NO_PIECE);
    byColor_[WHITE] = byColor_[BLACK] = 0;
    for (auto& b : byType_) b = 0;
    occupied_ = 0;
    nonPawn_ = 0;
    bishopQueen_[WHITE] = bishopQueen_[BLACK] = 0;
    rookQueen_[WHITE] = rookQueen_[BLACK] = 0;
    key_ = 0;
    kingSq_[WHITE] = kingSq_[BLACK] = SQ_NONE;
    castling_ = NO_CASTLING;
    ep_ = SQ_NONE;
    fifty_ = 0;
    fullmove_ = 1;
    history_.clear();
    keyHistory_.clear();

    std::istringstream ss(fen);
    std::string boardField, sideField, castleField, epField;
    if (!(ss >> boardField >> sideField)) return false;
    ss >> castleField >> epField;
    ss >> fifty_;
    ss >> fullmove_;

    int rank = 7, file = 0;
    for (char c : boardField) {
        if (c == '/') {
            --rank;
            file = 0;
            if (rank < 0) return false;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            Piece p = NO_PIECE;
            switch (c) {
                case 'P': p = W_PAWN; break;   case 'N': p = W_KNIGHT; break;
                case 'B': p = W_BISHOP; break; case 'R': p = W_ROOK; break;
                case 'Q': p = W_QUEEN; break;  case 'K': p = W_KING; break;
                case 'p': p = B_PAWN; break;   case 'n': p = B_KNIGHT; break;
                case 'b': p = B_BISHOP; break; case 'r': p = B_ROOK; break;
                case 'q': p = B_QUEEN; break;  case 'k': p = B_KING; break;
                default: return false;
            }
            if (file > 7 || rank < 0) return false;
            putPiece(p, makeSquare(file, rank));
            ++file;
        }
    }

    side_ = (sideField == "b") ? BLACK : WHITE;

    if (castleField != "-") {
        for (char c : castleField) {
            switch (c) {
                case 'K': castling_ |= WHITE_OO; break;
                case 'Q': castling_ |= WHITE_OOO; break;
                case 'k': castling_ |= BLACK_OO; break;
                case 'q': castling_ |= BLACK_OOO; break;
                default: break;
            }
        }
    }

    if (epField != "-") {
        int sq = stringToSquare(epField);
        if (sq != SQ_NONE) ep_ = sq;
    }

    // Pieces were loaded via putPiece, which accumulated their XOR keys.
    if (side_ == BLACK) key_ ^= zobrist::sideToMove;
    key_ ^= zobrist::castling[castling_];
    if (ep_ != SQ_NONE) key_ ^= zobrist::enPassant[ep_];

    keyHistory_.push_back(key_);
    return true;
}

std::string Position::fen() const {
    std::string out;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            Piece p = board_[makeSquare(file, rank)];
            if (p == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty) { out += char('0' + empty); empty = 0; }
            char c = '?';
            switch (typeOf(p)) {
                case PAWN:   c = 'p'; break;
                case KNIGHT: c = 'n'; break;
                case BISHOP: c = 'b'; break;
                case ROOK:   c = 'r'; break;
                case QUEEN:  c = 'q'; break;
                case KING:   c = 'k'; break;
                default: break;
            }
            if (colorOf(p) == WHITE) c = char(c - 'a' + 'A');
            out += c;
        }
        if (empty) out += char('0' + empty);
        if (rank) out += '/';
    }
    out += ' ';
    out += (side_ == WHITE ? 'w' : 'b');
    out += ' ';
    if (castling_ == NO_CASTLING) {
        out += '-';
    } else {
        if (castling_ & WHITE_OO)  out += 'K';
        if (castling_ & WHITE_OOO) out += 'Q';
        if (castling_ & BLACK_OO)  out += 'k';
        if (castling_ & BLACK_OOO) out += 'q';
    }
    out += ' ';
    out += (ep_ == SQ_NONE ? "-" : squareToString(ep_));
    out += ' ';
    out += std::to_string(fifty_);
    out += ' ';
    out += std::to_string(fullmove_);
    return out;
}

bool Position::squareAttacked(int sq, Color by) const {
    const int bi = bitIndexOf(sq);
    const uint64_t enemy = byColor_[by];

    // Pawns, knights and kings: one table lookup and a bitboard AND each.
    if (AT.pawnAttackers[by][bi] & byType_[PAWN] & enemy) return true;
    if (AT.knight[bi] & byType_[KNIGHT] & enemy) return true;
    if (AT.king[bi] & byType_[KING] & enemy) return true;

    // Sliding pieces: walk the precomputed rays from `sq`, stopping at the first
    // blocker. Directions 0..3 increase the bit index, 4..7 decrease it, so the
    // nearest blocker is the lowest/highest set bit of the ray's occupancy. The
    // blocker square itself stays reachable (an enemy there is a capture); only
    // squares beyond it are dropped.
    const uint64_t occ = occupied_;
    // bishopQueen_/rookQueen_ are maintained incrementally by put/removePiece,
    // so these are two loads instead of four ORs and two ANDs per call.
    const uint64_t bishopsQueens = bishopQueen_[by];
    const uint64_t rooksQueens = rookQueen_[by];
    static const int bishopDirs[4] = {2, 3, 6, 7};
    static const int rookDirs[4] = {0, 1, 4, 5};

    // `b` from ctz/clz is already a dense bit index, so the blocker bit is a
    // plain `1ULL << b` (bitOf(squareOfBitIndex(b)) recomputed the same value
    // through two conversions). `r ^ ray[b][d]` keeps the squares strictly
    // before the blocker; OR-ing the blocker bit back in makes it reachable.
    for (int d : bishopDirs) {
        const uint64_t r = AT.ray[bi][d];
        const uint64_t blockers = r & occ;
        if (!blockers) { if (r & bishopsQueens) return true; continue; }
        const int b = (d < 4) ? __builtin_ctzll(blockers) : 63 - __builtin_clzll(blockers);
        const uint64_t reach = (r ^ AT.ray[b][d]) | (1ULL << b);
        if (reach & bishopsQueens) return true;
    }
    for (int d : rookDirs) {
        const uint64_t r = AT.ray[bi][d];
        const uint64_t blockers = r & occ;
        if (!blockers) { if (r & rooksQueens) return true; continue; }
        const int b = (d < 4) ? __builtin_ctzll(blockers) : 63 - __builtin_clzll(blockers);
        const uint64_t reach = (r ^ AT.ray[b][d]) | (1ULL << b);
        if (reach & rooksQueens) return true;
    }

    return false;
}

bool Position::isInCheck(Color c) const {
    int ksq = kingSq_[c];
    if (ksq == SQ_NONE) return false;
    return squareAttacked(ksq, ~c);
}

void Position::generatePawnMoves(std::vector<Move>& moves, bool onlyCaptures) const {
    const int forward = (side_ == WHITE) ? 16 : -16;
    const int startRank = (side_ == WHITE) ? 1 : 6;
    const int promoRank = (side_ == WHITE) ? 7 : 0;
    const uint64_t them = byColor_[~side_];

    // Iterate the pawn bitboard directly rather than every friendly piece, so
    // the generator does not visit (and reject) the other piece types.
    for (uint64_t bb = byColor_[side_] & byType_[PAWN]; bb; bb &= bb - 1) {
        int from = squareOfBitIndex(__builtin_ctzll(bb));

        // Captures (including en passant).
        for (int dc : {forward - 1, forward + 1}) {
            int to = from + dc;
            if (!safe(to)) continue;
            bool enemy = testBit(them, to);
            int victimSq = to + (side_ == WHITE ? -16 : 16);
            bool epCapture = (to == ep_ && board_[to] == NO_PIECE &&
                              safe(victimSq) &&
                              board_[victimSq] == makePiece(~side_, PAWN));
            if (!enemy && !epCapture) continue;
            if (rankOf(to) == promoRank) {
                for (PieceType pt : {QUEEN, ROOK, BISHOP, KNIGHT}) {
                    Move m{from, to, pt, (uint8_t)(MF_PROMOTION | (epCapture ? MF_ENPASSANT : 0))};
                    moves.push_back(m);
                }
            } else {
                Move m{from, to, NO_PIECE_TYPE, (uint8_t)(epCapture ? MF_ENPASSANT : MF_NORMAL)};
                moves.push_back(m);
            }
        }

        if (onlyCaptures) continue;

        // Single push.
        int to = from + forward;
        if (safe(to) && board_[to] == NO_PIECE) {
            if (rankOf(to) == promoRank) {
                for (PieceType pt : {QUEEN, ROOK, BISHOP, KNIGHT})
                    moves.push_back(Move{from, to, pt, MF_PROMOTION});
            } else {
                moves.push_back(Move{from, to, NO_PIECE_TYPE, MF_NORMAL});
                // Double push.
                if (rankOf(from) == startRank) {
                    int to2 = to + forward;
                    if (board_[to2] == NO_PIECE)
                        moves.push_back(Move{from, to2, NO_PIECE_TYPE, MF_DOUBLE_PUSH});
                }
            }
        }
    }
}

void Position::generateKnightMoves(std::vector<Move>& moves, bool onlyCaptures) const {
    const uint64_t them = byColor_[~side_];
    for (uint64_t bb = byColor_[side_] & byType_[KNIGHT]; bb; bb &= bb - 1) {
        int from = squareOfBitIndex(__builtin_ctzll(bb));
        for (int off : KnightOffsets) {
            int to = from + off;
            if (!safe(to)) continue;
            bool enemy = testBit(them, to);
            if (onlyCaptures && !enemy) continue;
            if (board_[to] != NO_PIECE && !enemy) continue;  // own piece
            moves.push_back(Move{from, to, NO_PIECE_TYPE, MF_NORMAL});
        }
    }
}

void Position::generateSlidingMoves(std::vector<Move>& moves, bool onlyCaptures) const {
    const uint64_t them = byColor_[~side_];
    // Only bishops, rooks and queens are sliders; index those bitboards rather
    // than scanning every friendly piece and discarding the pawns and king.
    const uint64_t sliders = byColor_[side_] &
                             (byType_[BISHOP] | byType_[ROOK] | byType_[QUEEN]);
    for (uint64_t bb = sliders; bb; bb &= bb - 1) {
        int from = squareOfBitIndex(__builtin_ctzll(bb));
        Piece p = board_[from];
        PieceType pt = typeOf(p);

        const int* dirs;
        int n;
        if (pt == BISHOP) { dirs = BishopDirs; n = 4; }
        else if (pt == ROOK) { dirs = RookDirs; n = 4; }
        else { dirs = KingOffsets; n = 8; }  // queen uses all 8 directions

        for (int i = 0; i < n; ++i) {
            int d = dirs[i];
            for (int to = from + d; safe(to); to += d) {
                bool enemy = testBit(them, to);
                if (board_[to] != NO_PIECE) {
                    if (enemy && (onlyCaptures || true))
                        moves.push_back(Move{from, to, NO_PIECE_TYPE, MF_NORMAL});
                    break;
                }
                if (!onlyCaptures)
                    moves.push_back(Move{from, to, NO_PIECE_TYPE, MF_NORMAL});
            }
        }
    }
}

void Position::generateKingMoves(std::vector<Move>& moves, bool onlyCaptures) const {
    int from = kingSq_[side_];
    if (from == SQ_NONE) return;
    const uint64_t them = byColor_[~side_];
    for (int off : KingOffsets) {
        int to = from + off;
        if (!safe(to)) continue;
        bool enemy = testBit(them, to);
        if (onlyCaptures && !enemy) continue;
        if (board_[to] != NO_PIECE && !enemy) continue;
        moves.push_back(Move{from, to, NO_PIECE_TYPE, MF_NORMAL});
    }
}

void Position::generateCastlingMoves(std::vector<Move>& moves) const {
    // Most positions have no castling rights left, and this generator runs once
    // per interior node: bail out before the isInCheck() probe below, which
    // would otherwise duplicate the check test the search already made on entry
    // to the node. Same moves either way (no rights means no castling moves).
    if (castling_ == NO_CASTLING) return;
    if (side_ == WHITE) {
        if (isInCheck(WHITE)) return;
        if ((castling_ & WHITE_OO) &&
            board_[makeSquare(5, 0)] == NO_PIECE &&
            board_[makeSquare(6, 0)] == NO_PIECE &&
            board_[makeSquare(7, 0)] == W_ROOK &&
            !squareAttacked(makeSquare(5, 0), BLACK) &&
            !squareAttacked(makeSquare(6, 0), BLACK)) {
            moves.push_back(Move{makeSquare(4, 0), makeSquare(6, 0), NO_PIECE_TYPE, MF_CASTLE});
        }
        if ((castling_ & WHITE_OOO) &&
            board_[makeSquare(1, 0)] == NO_PIECE &&
            board_[makeSquare(2, 0)] == NO_PIECE &&
            board_[makeSquare(3, 0)] == NO_PIECE &&
            board_[makeSquare(0, 0)] == W_ROOK &&
            !squareAttacked(makeSquare(3, 0), BLACK) &&
            !squareAttacked(makeSquare(2, 0), BLACK)) {
            moves.push_back(Move{makeSquare(4, 0), makeSquare(2, 0), NO_PIECE_TYPE, MF_CASTLE});
        }
    } else {
        if (isInCheck(BLACK)) return;
        if ((castling_ & BLACK_OO) &&
            board_[makeSquare(5, 7)] == NO_PIECE &&
            board_[makeSquare(6, 7)] == NO_PIECE &&
            board_[makeSquare(7, 7)] == B_ROOK &&
            !squareAttacked(makeSquare(5, 7), WHITE) &&
            !squareAttacked(makeSquare(6, 7), WHITE)) {
            moves.push_back(Move{makeSquare(4, 7), makeSquare(6, 7), NO_PIECE_TYPE, MF_CASTLE});
        }
        if ((castling_ & BLACK_OOO) &&
            board_[makeSquare(1, 7)] == NO_PIECE &&
            board_[makeSquare(2, 7)] == NO_PIECE &&
            board_[makeSquare(3, 7)] == NO_PIECE &&
            board_[makeSquare(0, 7)] == B_ROOK &&
            !squareAttacked(makeSquare(3, 7), WHITE) &&
            !squareAttacked(makeSquare(2, 7), WHITE)) {
            moves.push_back(Move{makeSquare(4, 7), makeSquare(2, 7), NO_PIECE_TYPE, MF_CASTLE});
        }
    }
}

void Position::generateMoves(std::vector<Move>& moves, bool onlyCaptures) const {
    generatePawnMoves(moves, onlyCaptures);
    generateKnightMoves(moves, onlyCaptures);
    generateSlidingMoves(moves, onlyCaptures);
    generateKingMoves(moves, onlyCaptures);
    if (!onlyCaptures) generateCastlingMoves(moves);
}

void Position::doMove(const Move& m) {
    StateInfo st;
    st.castlingRights = castling_;
    st.epSquare = ep_;
    st.fiftyMove = fifty_;
    st.captured = NO_PIECE;
    st.key = key_;

    const Color us = side_;
    const Color them = ~us;

    if (m.hasFlag(MF_ENPASSANT)) {
        int victimSq = m.to + (us == WHITE ? -16 : 16);
        st.captured = board_[victimSq];
        removePiece(victimSq);
        movePiece(m.from, m.to);
    } else if (m.hasFlag(MF_CASTLE)) {
        bool kingSide = fileOf(m.to) == 6;
        int rank = rankOf(m.from);
        if (kingSide) {
            movePiece(makeSquare(7, rank), makeSquare(5, rank));
        } else {
            movePiece(makeSquare(0, rank), makeSquare(3, rank));
        }
        movePiece(m.from, m.to);
    } else {
        if (board_[m.to] != NO_PIECE) st.captured = board_[m.to];
        movePiece(m.from, m.to);
        if (m.hasFlag(MF_PROMOTION)) {
            removePiece(m.to);
            putPiece(makePiece(us, m.promotion), m.to);
        }
    }

    // Update castling rights.
    Piece moved = board_[m.to];
    if (typeOf(moved) == KING) {
        castling_ &= (us == WHITE) ? ~(WHITE_OO | WHITE_OOO) : ~(BLACK_OO | BLACK_OOO);
    }
    // Rook moves / rook captures clear the relevant right.
    auto clearRook = [&](int sq) {
        if (sq == makeSquare(0, 0)) castling_ &= ~WHITE_OOO;
        else if (sq == makeSquare(7, 0)) castling_ &= ~WHITE_OO;
        else if (sq == makeSquare(0, 7)) castling_ &= ~BLACK_OOO;
        else if (sq == makeSquare(7, 7)) castling_ &= ~BLACK_OO;
    };
    if (typeOf(moved) == ROOK) clearRook(m.to);
    if (typeOf(board_[m.from]) == ROOK) clearRook(m.from);
    if (st.captured != NO_PIECE && typeOf(st.captured) == ROOK) {
        // Captured rook square: for en passant not possible; else it's m.to.
        if (!m.hasFlag(MF_ENPASSANT)) clearRook(m.to);
    }

    // En-passant square: only set it when an enemy pawn can actually capture,
    // which keeps positions identical to the standard perft references.
    ep_ = SQ_NONE;
    if (m.hasFlag(MF_DOUBLE_PUSH)) {
        int e = m.from + (us == WHITE ? 16 : -16);
        Piece enemyPawn = makePiece(~us, PAWN);
        int o1 = (us == WHITE) ? 15 : -17;
        int o2 = (us == WHITE) ? 17 : -15;
        if ((safe(e + o1) && board_[e + o1] == enemyPawn) ||
            (safe(e + o2) && board_[e + o2] == enemyPawn))
            ep_ = e;
    }

    // Halfmove clock.
    if (typeOf(moved) == PAWN || st.captured != NO_PIECE) fifty_ = 0;
    else fifty_ = st.fiftyMove + 1;

    if (us == BLACK) ++fullmove_;

    // Incremental Zobrist: swap out the old side/castling/ep terms, swap in the
    // new ones. Piece keys were already handled by movePiece.
    key_ ^= zobrist::sideToMove;
    key_ ^= zobrist::castling[st.castlingRights];
    key_ ^= zobrist::castling[castling_];
    if (st.epSquare != SQ_NONE) key_ ^= zobrist::enPassant[st.epSquare];
    if (ep_ != SQ_NONE) key_ ^= zobrist::enPassant[ep_];

    side_ = them;
    history_.push_back(st);
    keyHistory_.push_back(key_);
}

void Position::undoMove(const Move& m) {
    if (history_.empty()) return;
    StateInfo st = history_.back();
    history_.pop_back();
    if (!keyHistory_.empty()) keyHistory_.pop_back();

    const Color us = ~side_;  // side that made the move
    side_ = us;

    if (m.hasFlag(MF_ENPASSANT)) {
        int victimSq = m.to + (us == WHITE ? -16 : 16);
        movePiece(m.to, m.from);
        putPiece(makePiece(~us, PAWN), victimSq);
    } else if (m.hasFlag(MF_CASTLE)) {
        bool kingSide = fileOf(m.to) == 6;
        int rank = rankOf(m.from);
        movePiece(m.to, m.from);
        if (kingSide) {
            movePiece(makeSquare(5, rank), makeSquare(7, rank));
        } else {
            movePiece(makeSquare(3, rank), makeSquare(0, rank));
        }
    } else {
        // Undo promotion by restoring the pawn before moving back.
        if (m.hasFlag(MF_PROMOTION)) {
            removePiece(m.to);
            putPiece(makePiece(us, PAWN), m.from);
        } else {
            movePiece(m.to, m.from);
        }
        if (st.captured != NO_PIECE) putPiece(st.captured, m.to);
    }

    castling_ = st.castlingRights;
    ep_ = st.epSquare;
    fifty_ = st.fiftyMove;
    if (us == BLACK) --fullmove_;
    key_ = st.key;
}

void Position::makeNullMove() {
    StateInfo st;
    st.castlingRights = castling_;
    st.epSquare = ep_;
    st.fiftyMove = fifty_;
    st.captured = NO_PIECE;
    st.key = key_;

    key_ ^= zobrist::sideToMove;
    if (ep_ != SQ_NONE) {
        key_ ^= zobrist::enPassant[ep_];
        ep_ = SQ_NONE;
    }
    fifty_ = st.fiftyMove + 1;
    side_ = ~side_;

    history_.push_back(st);
    keyHistory_.push_back(key_);
}

void Position::undoNullMove() {
    if (history_.empty()) return;
    StateInfo st = history_.back();
    history_.pop_back();
    if (!keyHistory_.empty()) keyHistory_.pop_back();

    side_ = ~side_;
    castling_ = st.castlingRights;
    ep_ = st.epSquare;
    fifty_ = st.fiftyMove;
    key_ = st.key;
}

bool Position::isRepetition() const {
    // Scan the path (at most to the last irreversible move) for an identical
    // key supplied by an even number of half-moves away.
    int end = fifty_;
    if (end > (int)keyHistory_.size() - 1) end = (int)keyHistory_.size() - 1;
    for (int i = 2; i <= end; i += 2)
        if (keyHistory_[keyHistory_.size() - 1 - i] == key_) return true;
    return false;
}

bool Position::hasInsufficientMaterial() const {
    if (byType_[PAWN] || byType_[ROOK] || byType_[QUEEN]) return false;

    const int knights = __builtin_popcountll(byType_[KNIGHT]);
    const int bishops = __builtin_popcountll(byType_[BISHOP]);
    const int minors = knights + bishops;

    if (minors <= 1) return true;   // K vs K, K+N vs K, K+B vs K
    if (knights > 0) return false;  // any knight can assist a mate

    // Bishops only. If every bishop stands on one square colour (and there are
    // no pawns), no mate can be forced; a bishop on the other colour is needed.
    const uint64_t darkSquares = 0xAA55AA55AA55AA55ULL;
    const uint64_t bishopsBB = byType_[BISHOP];
    return (bishopsBB & darkSquares) == 0 || (bishopsBB & ~darkSquares) == 0;
}

bool Position::doMoveIfLegal(const Move& m) {
    const Color us = side_;
    doMove(m);
    if (isInCheck(us)) {
        undoMove(m);
        return false;
    }
    return true;
}

bool Position::isLegalMove(const Move& m) {
    if (!doMoveIfLegal(m)) return false;
    undoMove(m);
    return true;
}

void Position::generateLegalMoves(std::vector<Move>& moves) {
    std::vector<Move> pseudo;
    pseudo.reserve(64);
    generateMoves(pseudo, false);
    for (const Move& m : pseudo)
        if (isLegalMove(m)) moves.push_back(m);
}

void Position::print() const {
    std::ostream& os = std::cout;
    for (int rank = 7; rank >= 0; --rank) {
        std::string line = std::to_string(rank + 1) + " ";
        for (int file = 0; file < 8; ++file) {
            Piece p = board_[makeSquare(file, rank)];
            char c = '.';
            if (p != NO_PIECE) {
                switch (typeOf(p)) {
                    case PAWN:   c = 'p'; break;
                    case KNIGHT: c = 'n'; break;
                    case BISHOP: c = 'b'; break;
                    case ROOK:   c = 'r'; break;
                    case QUEEN:  c = 'q'; break;
                    case KING:   c = 'k'; break;
                    default: break;
                }
                if (colorOf(p) == WHITE) c = char(c - 'a' + 'A');
            }
            line += c;
            line += ' ';
        }
        os << line << "\n";
    }
    os << "  a b c d e f g h\n";
    os << "FEN: " << fen() << "\n";
}

}  // namespace seger
