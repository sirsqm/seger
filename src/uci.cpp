#include "uci.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eval.h"
#include "perft.h"
#include "types.h"

namespace seger {

namespace {

const char* kEngineName = "Seger";
const char* kEngineAuthor = "sqm";

std::vector<std::string> split(const std::string& s) {
    std::istringstream ss(s);
    std::vector<std::string> out;
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// Parses the "position" command body into the board state.
void applyPositionCommand(Position& pos, const std::vector<std::string>& t) {
    size_t i = 1;
    if (i >= t.size()) return;
    if (t[i] == "startpos") {
        pos.setStartpos();
        ++i;
    } else if (t[i] == "fen") {
        std::string fen;
        ++i;
        while (i < t.size() && t[i] != "moves") {
            if (!fen.empty()) fen += ' ';
            fen += t[i++];
        }
        pos.setFen(fen);
    }
    if (i < t.size() && t[i] == "moves") {
        ++i;
        for (; i < t.size(); ++i) {
            // Parse a UCI move against the current position.
            std::string s = t[i];
            if (s.size() < 4) continue;
            int from = stringToSquare(s.substr(0, 2));
            int to = stringToSquare(s.substr(2, 2));
            PieceType promo = NO_PIECE_TYPE;
            if (s.size() >= 5) {
                switch (s[4]) {
                    case 'n': promo = KNIGHT; break;
                    case 'b': promo = BISHOP; break;
                    case 'r': promo = ROOK; break;
                    case 'q': promo = QUEEN; break;
                    default: break;
                }
            }
            std::vector<Move> legal;
            legal.reserve(64);
            pos.generateLegalMoves(legal);
            bool matched = false;
            for (const Move& m : legal) {
                if (m.from == from && m.to == to &&
                    (promo == NO_PIECE_TYPE || m.promotion == promo) &&
                    (promo != NO_PIECE_TYPE || m.promotion == NO_PIECE_TYPE)) {
                    pos.doMove(m);
                    matched = true;
                    break;
                }
                // Allow promoting moves specified without the piece letter.
                if (m.from == from && m.to == to && promo == NO_PIECE_TYPE &&
                    m.promotion != NO_PIECE_TYPE) {
                    pos.doMove(m);
                    matched = true;
                    break;
                }
            }
            if (!matched) break;
        }
    }
}

}  // namespace

void uciLoop() {
    zobrist::init();

    std::string line;
    Position pos;
    pos.setStartpos();
    Search search(pos);

    auto& out = std::cout;

    while (std::getline(std::cin, line)) {
        std::vector<std::string> t = split(line);
        if (t.empty()) continue;

        if (t[0] == "uci") {
            out << "id name " << kEngineName << "\n";
            out << "id author " << kEngineAuthor << "\n";
            out << "option name Hash type spin default 64 min 1 max 4096\n";
            out << "uciok\n";
            out.flush();
        } else if (t[0] == "isready") {
            out << "readyok\n";
            out.flush();
        } else if (t[0] == "ucinewgame") {
            pos.setStartpos();
            search.clearTT();
        } else if (t[0] == "setoption") {
            // setoption name Hash value 64
            for (size_t i = 1; i + 1 < t.size(); ++i) {
                if (t[i] == "name" && t[i + 1] == "Hash" && i + 3 < t.size() &&
                    t[i + 2] == "value") {
                    int mb = std::atoi(t[i + 3].c_str());
                    if (mb >= 1) {
                        search.tt().resize((size_t)mb);
                        search.clearTT();
                    }
                }
            }
        } else if (t[0] == "position") {
            applyPositionCommand(pos, t);
        } else if (t[0] == "go") {
            // Parse go arguments directly here so side-specific clock values
            // (wtime/btime) are applied correctly.
            SearchLimits lim;
            bool perftOnly = false;
            int perftDepth = 1;
            for (size_t i = 1; i < t.size(); ++i) {
                const std::string& tok = t[i];
                auto nextInt = [&](int def) -> int {
                    return (i + 1 < t.size()) ? std::atoi(t[++i].c_str()) : def;
                };
                if (tok == "depth") { lim.depth = nextInt(MAX_PLY); lim.useDepth = true; }
                else if (tok == "movetime") lim.movetime = nextInt(0);
                else if (tok == "wtime") { int v = nextInt(0); if (pos.sideToMove() == WHITE) lim.timeLeft = v; }
                else if (tok == "btime") { int v = nextInt(0); if (pos.sideToMove() == BLACK) lim.timeLeft = v; }
                else if (tok == "winc") { int v = nextInt(0); if (pos.sideToMove() == WHITE) lim.increment = v; }
                else if (tok == "binc") { int v = nextInt(0); if (pos.sideToMove() == BLACK) lim.increment = v; }
                else if (tok == "movestogo") lim.movestogo = nextInt(0);
                else if (tok == "nodes") lim.nodes = (uint64_t)std::strtoull(t[++i].c_str(), nullptr, 10);
                else if (tok == "infinite") lim.infinite = true;
                else if (tok == "perft") { perftOnly = true; perftDepth = nextInt(1); }
            }

            if (perftOnly) {
                Position p = pos;
                uint64_t n = perft(p, perftDepth);
                out << "nodes " << n << "\n";
                out.flush();
                continue;
            }

            search.setInfoCallback(out);
            Move best = MOVE_NONE;
            if (lim.infinite) {
                // Search on a worker thread so the main thread can read "stop".
                std::thread worker([&]() { best = search.think(lim); });
                std::string cmd;
                while (std::getline(std::cin, cmd)) {
                    std::vector<std::string> ct = split(cmd);
                    if (!ct.empty() && ct[0] == "stop") { search.stop(); break; }
                    if (!ct.empty() && ct[0] == "quit") { search.stop(); worker.join(); return; }
                }
                worker.join();
            } else {
                best = search.think(lim);
            }

            out << "bestmove "
                << (best.isNone() ? "0000" : moveToString(best)) << "\n";
            out.flush();
        } else if (t[0] == "d" || t[0] == "print") {
            pos.print();
        } else if (t[0] == "eval") {
            out << "static eval: " << evaluate(pos) << "\n";
            out.flush();
        } else if (t[0] == "quit") {
            break;
        }
        // Unknown commands are ignored, as the UCI spec requires.
    }
}

}  // namespace seger
