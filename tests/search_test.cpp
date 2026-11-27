// Search correctness tests.
//
// The mate tests use an independent brute-force solver (no transposition table,
// no pruning, no evaluation) so they can disagree with the real search if it is
// wrong, rather than merely re-implementing the same assumption.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "position.h"
#include "search.h"

using namespace seger;

static int g_failures = 0;

static void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        printf("[ ok ] %s\n", name.c_str());
    } else {
        printf("[FAIL] %s %s\n", name.c_str(), detail.c_str());
        ++g_failures;
    }
}

// Brute-force mate solver: returns a score from the side to move's perspective,
// with mates encoded as MATE - ply (positive = side to move mates). No pruning,
// no evaluation beyond "no legal moves".
static int bruteForce(Position& pos, int depth, int ply = 0) {
    if (depth == 0) return 0;
    std::vector<Move> moves;
    pos.generateLegalMoves(moves);
    if (moves.empty())
        return pos.isInCheck(pos.sideToMove()) ? -(MATE - ply) : 0;

    int best = -INF;
    for (const Move& m : moves) {
        pos.doMove(m);
        int s = -bruteForce(pos, depth - 1, ply + 1);
        pos.undoMove(m);
        if (s > best) best = s;
    }
    return best;
}

// Recovers the score from the last info line in the captured search output.
static int lastScore(const std::string& info) {
    int score = INF + 1;
    std::istringstream ss(info);
    std::string line;
    while (std::getline(ss, line)) {
        auto p = line.find(" score ");
        if (p == std::string::npos) continue;
        std::istringstream ls(line.substr(p + 7));
        std::string kind;
        int value = 0;
        ls >> kind >> value;
        score = (kind == "mate")
                    ? (value > 0 ? MATE - (2 * value - 1) : -MATE + (2 * -value - 1))
                    : value;
    }
    return score;
}

// Runs the real search to a fixed depth and returns the reported score.
static int searchScore(Position& pos, int depth) {
    Search search(pos);
    std::ostringstream discard;
    search.setInfoCallback(discard);
    SearchLimits lim;
    lim.useDepth = true;
    lim.depth = depth;
    search.think(lim);
    return lastScore(discard.str());
}

static int searchNodes(Position& pos, int depth, bool useTT) {
    Search search(pos);
    std::ostringstream discard;
    search.setInfoCallback(discard);
    search.setTTEnabled(useTT);
    SearchLimits lim;
    lim.useDepth = true;
    lim.depth = depth;
    search.think(lim);
    return (int)search.lastNodes();
}

int main() {
    zobrist::init();

    // --- Forced mates, cross-checked against the brute-force solver ---------
    struct MateCase { const char* fen; int wantPlies; const char* name; };
    const MateCase mates[] = {
        {"6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", 1, "back-rank mate in 1"},
        {"1k6/8/1K6/8/8/8/8/1Q6 w - - 0 1", 3, "queen mate in 2"},
    };
    for (const auto& mc : mates) {
        Position pos;
        pos.setFen(mc.fen);
        int bf = bruteForce(pos, mc.wantPlies + 2);
        int bfPlies = (bf > 0) ? MATE - bf : -1;
        check(bfPlies == mc.wantPlies, std::string("brute force agrees: ") + mc.name,
              "got plies=" + std::to_string(bfPlies));

        Position pos2;
        pos2.setFen(mc.fen);
        int s = searchScore(pos2, mc.wantPlies + 3);
        int sPlies = isMateScore(s) && s > 0 ? MATE - s : -1;
        check(sPlies == mc.wantPlies, std::string("search finds: ") + mc.name,
              "got plies=" + std::to_string(sPlies));
    }

    // --- Transposition table -------------------------------------------------
    // The TT is a lossless optimisation: it may reorder equally-good moves, but
    // it must not change the score the search settles on. (Comparing moves
    // directly is brittle: two moves can tie exactly.) The comparison allows a
    // small tolerance because the search prunes on heuristic margins: a
    // different move order reaches a different set of reduced/pruned nodes, so
    // the score can move by a few centipawns without the search being wrong.
    {
        Position pos;
        pos.setStartpos();
        int nodesWith = searchNodes(pos, 8, true);
        int nodesWithout = searchNodes(pos, 8, false);
        check(nodesWith < nodesWithout, "TT reduces node count",
              "with=" + std::to_string(nodesWith) + " without=" + std::to_string(nodesWithout));

        Search s1(pos), s2(pos);
        std::ostringstream o1, o2;
        s1.setInfoCallback(o1);
        s2.setInfoCallback(o2);
        s1.setTTEnabled(true);
        s2.setTTEnabled(false);
        SearchLimits lim;
        lim.useDepth = true;
        lim.depth = 7;
        s1.think(lim);
        s2.think(lim);
        int scoreWith = lastScore(o1.str());
        int scoreWithout = lastScore(o2.str());
        int diff = scoreWith - scoreWithout;
        if (diff < 0) diff = -diff;
        check(diff <= 10, "TT does not change the score",
              "with=" + std::to_string(scoreWith) + " without=" + std::to_string(scoreWithout));
    }

    // --- Legality from a spread of positions --------------------------------
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
        "8/8/8/8/8/6k1/6p1/6K1 w - - 0 1",
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1",
    };
    for (const char* fen : fens) {
        Position pos;
        pos.setFen(fen);
        Search search(pos);
        std::ostringstream discard;
        search.setInfoCallback(discard);
        SearchLimits lim;
        lim.useDepth = true;
        lim.depth = 5;
        Move best = search.think(lim);

        std::vector<Move> legal;
        pos.generateLegalMoves(legal);
        bool found = false;
        for (const Move& m : legal) if (m == best) found = true;
        check(legal.empty() || found, std::string("search returns a legal move: ") + fen,
              moveToString(best));
    }

    if (g_failures == 0) {
        printf("\nAll search tests passed.\n");
        return 0;
    }
    printf("\n%d search test(s) failed.\n", g_failures);
    return 1;
}
