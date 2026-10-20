#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

namespace seger {

// Number of board squares in the 0x88 mailbox (128 entries, half unused).
constexpr int BOARD_SIZE = 128;

// Extra state that a make/unmake cycle must restore.
struct StateInfo {
    int castlingRights;
    int epSquare;        // en-passant target square, or SQ_NONE
    int fiftyMove;       // halfmove clock
    uint64_t key;        // Zobrist key after the move
    Piece captured;      // piece removed from `to` (or en-passant victim)
};

class Position {
public:
    Position();

    // Initialises the standard starting position.
    void setStartpos();

    // Loads a position from a FEN string. Returns false on parse failure.
    bool setFen(const std::string& fen);

    // Returns the FEN representation of the current position.
    std::string fen() const;

    // --- Board accessors ---------------------------------------------------
    Piece pieceOn(int sq) const { return board_[sq]; }
    Color sideToMove() const { return side_; }
    int castlingRights() const { return castling_; }
    int epSquare() const { return ep_; }
    int fiftyMove() const { return fifty_; }
    int fullMoves() const { return fullmove_; }
    uint64_t key() const { return key_; }

    int kingSquare(Color c) const { return kingSq_[c]; }
    bool isInCheck(Color c) const;

    uint64_t colorBB(Color c) const { return byColor_[c]; }
    uint64_t pieceBB(PieceType pt) const { return byType_[pt]; }
    uint64_t occupiedBB() const { return occupied_; }

    // True when the given side still owns a knight, bishop, rook or queen.
    // The non-pawn bitboard is maintained incrementally (putPiece/removePiece),
    // so this is a single load and test on a search hot path.
    bool hasNonPawnMaterial(Color c) const { return (byColor_[c] & nonPawn_) != 0; }

    // True when the current position repeats an earlier one on the game /
    // search path (a "twofold" repetition, scored as a draw).
    bool isRepetition() const;

    // True when neither side can possibly deliver checkmate from here, so the
    // game is a dead draw under FIDE's "insufficient material" rule: king vs
    // king, a single minor piece, or only bishops confined to one square colour.
    bool hasInsufficientMaterial() const;

    // --- Move making -------------------------------------------------------
    // Applies a move to the position; pushes undo information. The caller is
    // responsible for only passing moves that were generated for this
    // position (so `captured` can be derived correctly).
    void doMove(const Move& m);
    void undoMove(const Move& m);

    // Applies `m` only if it is legal (leaves the mover's king safe), and
    // returns whether it was applied. One make/unmake cycle instead of the
    // isLegalMove()+doMove() pair the search used to run, which applied the
    // move twice. Leaves the position unchanged when it returns false.
    bool doMoveIfLegal(const Move& m);

    // Null move: passes the turn without moving a piece (null-move pruning).
    void makeNullMove();
    void undoNullMove();

    // --- Move generation ---------------------------------------------------
    // Generates all pseudo-legal moves for the side to move. When
    // `onlyCaptures` is set, only captures (and promotions) are generated,
    // which the search uses for quiescence.
    void generateMoves(std::vector<Move>& moves, bool onlyCaptures = false) const;

    // Generates fully legal moves.
    void generateLegalMoves(std::vector<Move>& moves);

    // Returns true if the move is legal (leaves the moving side's king out of
    // check). Uses make/unmake internally.
    bool isLegalMove(const Move& m);

    // --- Output ------------------------------------------------------------
    void print() const;

private:
    std::array<Piece, BOARD_SIZE> board_;
    Color side_;
    int castling_;
    int ep_;
    int fifty_;
    int fullmove_;
    int kingSq_[COLOR_NB];
    uint64_t key_;

    std::vector<StateInfo> history_;

    // Bitboards tracking all occupied squares (for quick movegen scans). We
    // keep one per color for efficient "does this square hold an enemy piece"
    // checks in pawn/king move generation, plus one per piece type for the
    // evaluation's material test.
    uint64_t byColor_[COLOR_NB];
    uint64_t byType_[PIECE_TYPE_NB];
    uint64_t occupied_;
    // Knights, bishops, rooks and queens of both colours, maintained in
    // putPiece/removePiece so hasNonPawnMaterial() is a load-and-test.
    uint64_t nonPawn_;

    // Bishops+queens and rooks+queens per colour, also maintained incrementally
    // so squareAttacked() reads its slider sets directly instead of rebuilding
    // them with four ORs and two ANDs on every call (every node's isInCheck).
    uint64_t bishopQueen_[COLOR_NB];
    uint64_t rookQueen_[COLOR_NB];

    // Zobrist keys of positions visited along the current game path, for
    // repetition detection. Position 0 is the key at setFen()/setStartpos().
    std::vector<uint64_t> keyHistory_;

    void putPiece(Piece p, int sq);
    void removePiece(int sq);
    void movePiece(int from, int to);

    void generatePawnMoves(std::vector<Move>& moves, bool onlyCaptures) const;
    void generateKnightMoves(std::vector<Move>& moves, bool onlyCaptures) const;
    void generateSlidingMoves(std::vector<Move>& moves, bool onlyCaptures) const;
    void generateKingMoves(std::vector<Move>& moves, bool onlyCaptures) const;
    void generateCastlingMoves(std::vector<Move>& moves) const;

    bool squareAttacked(int sq, Color by) const;
};

// --- Zobrist keys ----------------------------------------------------------
namespace zobrist {
void init();
extern uint64_t psq[PIECE_NB][BOARD_SIZE];
extern uint64_t sideToMove;
extern uint64_t castling[CASTLING_NB];
extern uint64_t enPassant[BOARD_SIZE];
}  // namespace zobrist

}  // namespace seger
