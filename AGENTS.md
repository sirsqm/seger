# AGENTS.md

Repository-specific guidance for agents working on Seger.

## What this is

Seger is a UCI chess engine in C++17. It uses a 0x88 mailbox board, incremental
Zobrist hashing, a transposition table, and an alpha-beta / principal-variation
search with null-move pruning and late-move reductions.

## Build and test

```sh
make          # builds build/seger
make test     # builds the engine + runs position, perft, search, uci and eval tests
make divide   # builds build/perft_divide for move-generation debugging
make bench    # quick fixed-depth benchmark (startpos, depth 9)
```

There is no cmake; the project builds with plain `make` and `g++`. Objects carry
generated `-MMD` dependencies (`build/*.d`), so changing a header rebuilds every
translation unit that includes it. If you ever build without them — or
`CXXFLAGS` fails to pass `-MMD` — a header/struct change will silently leave
stale `.o` files with mismatched layouts and crash at `-O2` with no other hint.
When in doubt, `make clean && make`.

`tools/selfplay.py` plays two engine binaries against each other
(`--a build/seger --b /path/to/other --games 8 --depth 5`) and reports the
score and a rough Elo delta. Use it to check whether an evaluation or search
change is actually an improvement before keeping it. `--parallel N` runs N
games at once across cores (roughly N× faster wall-clock), `--movetime`
selects a time control instead of a fixed depth, and `--max-seconds S` stops
the match after S seconds and still reports the partial score over the games
that finished, so a scheduled run can never hang on a long match.

CI (`.github/workflows/ci.yml`) runs `make`, `make test`, a UCI smoke test and
`make bench` on every push to `main` and on pull requests. Keep it green.

`python3 tools/preflight.py` wraps the same steps with a hard wall-clock timeout
per step (`make`, `make test`, `make bench`, and optionally a capped self-play
match via `--selfplay`), kills the whole process group on overrun, and exits
nonzero on any failure, timeout or exhausted budget. `make preflight` runs the
build/test/bench form and `make verify` adds a short capped match. Run these —
not the raw commands — inside anything that has its own time limit
(automations, CI jobs, long tuning sessions). A single unbounded `make bench` or
a self-play match with no `--movetime`/cap can otherwise hold a run past its
budget and get it killed; this is what happened to the daily-improvement
automation on 2026-10-10. `--max-seconds` on `selfplay.py` and the per-step
timeouts in `preflight.py` are the two guards; keep both.

Bench/signature baseline: depth 9 from startpos currently settles at
**982,914 nodes** (~1.37 Mnps) after the aspiration, eval and ordering changes.
The node count moves whenever the evaluation or search changes, so treat it as a
regression tripwire rather than a constant to preserve (an older build reported
140,502,299 nodes before the move-ordering fix cut the tree ~13x).

Measured strength of the tapered evaluation against the earlier one, at depth 6
with `tools/selfplay.py` (200 games, mixed openings, colours alternated):
**A 162 wins, 17 draws, 21 losses — score 0.853, +305 Elo (+/- 68 at 95%)**. The
interval is wide because the gap is large; the lower bound is still above +200.
Re-run the same command against a `build/seger` from commit `ee816e7` to
reproduce it.

Absolute strength via `tools/vs_stockfish.py` (Stockfish strength-limited with
`UCI_LimitStrength`/`UCI_Elo`, 100 ms/move, one SF thread, colours alternated).
With everything below in, a 40-games-per-anchor run scored 0.525 vs SF-Elo 2400
(draw), 0.463 vs 2500 and 0.425 vs 2600, and the anchors bracket Seger at
2417/2474/2547, i.e. roughly **2450-2500 Elo**. The starting baseline this
session was 0.662 / 0.400 / 0.475 vs 2100 / 2200 / 2300, about **2200-2250 Elo**
— so the session added on the order of +250 Elo absolute, most of it from the
move-ordering fix. (An even earlier 40-game run on a loaded machine gave 2075;
that was contention, not a real difference — run these measurements on an idle
CPU.) This is Seger against Stockfish's own UCI_Elo calibration at a short time
control, not an official rating-list number; treat it as a ballpark. The
`vs_stockfish.py` tool needs a Stockfish binary (`--stockfish`), not vendored.

- **`hasNonPawnMaterial` reads an incremental bitboard.** The helper rebuilt
  `knights|bishops|rooks|queens` from four piece bitboards and ANDed with the
  colour board on every call; the search calls it on every node with a non-pawn
  side to move (twice: reverse-futility and null-move guards). A `nonPawn_`
  bitboard is now updated in `putPiece`/`removePiece` and the helper is a single
  load-and-test. Strictly behaviour-preserving: the depth-9 bench is unchanged at
  exactly **798,364 nodes** and depth-11 at **5,245,109**, and a 60-game A/B at
  depth 5 scored exactly **0.500** against the pre-change build, as a same-tree
  change must. Speed: interleaved depth-11 runs on an idle CPU show
  **~1.587M -> ~1.592M nps**; the effect is small and at the edge of run-to-run
  noise, so treat it as a cheap removal of work on a hot path rather than a
  measured gain.

## Tried and rejected

These were measured with `tools/selfplay.py` and did **not** help; do not
re-introduce them without a fresh match that beats the baseline:

- A second, larger evaluation rewrite (king-safety term, mobility areas, threat
  detection, passed-pawn table, 7th-rank rook bonus) scored **0.417 over 60
  games at depth 5**, i.e. about -58 Elo. The existing tapered evaluation is
  already well tuned.
- Reverse-futility pruning, futility pruning and late-move pruning, added on top
  of the current search, cost roughly 300 Elo: the margins were too loose and the
  engine started cutting good lines. The lesson stands for loose margins, but
  tight margins *do* pay now — see the Wins section (a first attempt with these
  constants also broke the mate-in-2 search test until mate-score guards were
  added). If you add pruning, tune the margins with a match, do not paste in
  textbook constants.
- Enabling late-move reductions more aggressively (the old `isCapture` bug had
  left LMR effectively dead) also lost: 0.500 at depth 5 and 0.125 at depth 7.
  A conservative 1-ply LMR with TT/killer exclusions was still below baseline
  (0.375 at depth 7). This was true *while the move-ordering bug was present*;
  see the Wins section — after the fix, LMR is a large gain.

## Wins (measured)

- **Move ordering is now lazy, not a full sort.** `scoreMoves` ranked every move
  at a node and then insertion-sorted the whole list before the move loop, so
  every node paid to order moves it would never reach because it cut off first.
  `rankMoves` still assigns the same rank to each move (TT > captures > killers >
  history), but the two move loops in `search` and `quiescence` now pull the best
  remaining entry to the front on demand, shifting rather than swapping so ties
  keep their original order. This is strictly behaviour-preserving: the depth-9
  bench is unchanged at exactly **798,364 nodes** (depth-11 at 5,245,109), and
  the per-ply ranked buffers (`rankedScratch_[MAX_PLY]`, one per ply so a node's
  ranking survives its children) are reserved once in `think`. Speed: depth-12
  bench nps **~1.60M -> ~1.62M (~1.6%)** over five interleaved runs on an idle
  CPU, which shows up as extra depth under a time control; a 60-game A/B at
  80 ms/move scored **0.525 (+17 Elo, +/- 88)**, consistent with a gain this
  small. The root keeps an eager `std::stable_sort` because it runs once per
  iteration, not once per node.

- **Per-node scratch buffers and an allocation-free move sort.** `Search::search`
  and `Search::quiescence` each built a fresh `std::vector<Move>` with a
  `reserve()` on every interior and every capture node, and `scoreMoves` called
  `std::stable_sort`, which allocates a temporary buffer per call (about a
  million calls in a depth-11 bench). Both move lists now come from a per-ply
  `moveScratch_[MAX_PLY]` member (one buffer per ply, so a node's list survives
  the child calls that run while it is iterated; `quiescence` also returns
  `evaluate()` at `ply >= MAX_PLY - 1` so a long capture chain cannot index past
  the array), and `scoreMoves` uses a hand-written stable insertion sort, which
  is fine for these short lists and allocates nothing. Strictly
  behaviour-preserving: the depth-9 bench is unchanged at exactly **798,364
  nodes**. Speed: nps **~1.387M -> ~1.450M (+4.6%)** over three interleaved runs
  each on an idle CPU at depth 12, which shows up as extra depth under a time
  control. The 60-game A/B at 80 ms/move is underpowered for a gain this small
  (0.508), so treat the node-count-neutral nps measurement as the evidence.

- **`Position::squareAttacked` uses bitboards instead of mailbox rays.**
  `squareAttacked` was the hottest shared helper (every node entry via
  `isInCheck`, every legality check, castling rights). It walked the 0x88 board
  one square at a time from the target square outward — four bishop rays plus
  four rook rays, each a dependent-load chain — and probed `board_[]` for
  knights and kings on eight offsets. It now reads precomputed attack sets from
  a file-local `AttackTables` struct (`AT`): pawn attackers keyed by colour and
  dense square index, knight/king sets, and an 8-direction `ray[64][8]` table.
  Knights, kings and pawns are a table lookup ANDed with the enemy bitboards;
  each slider direction masks the ray against `occupied_` and picks the nearest
  blocker with `ctz`/`clz`, which is one branch-free probe instead of a walk.
  Strictly behaviour-preserving: the depth-9 bench is unchanged at exactly
  **798,364 nodes** (and depth-11 at 2,702,670). Speed: depth-11 bench nps
  **~1.51M -> ~1.61M (+6.5%)** over five runs on an idle CPU, which shows up as
  extra depth under a time control. A 60-game A/B at 80 ms/move is exactly even
  (0.508), as it must be for a same-tree change; the nps measurement is the
  evidence.

- **Quiescence now probes the transposition table.** `Search::quiescence` did
  not touch the table at all, so it had no TT cutoff in the capture search and
  no TT move to order captures by, and it re-searched positions the main search
  had already resolved. It now makes a read-only probe (accepting any depth)
  and stores its result at depth 0, the standard convention that keeps the main
  search — which only accepts `e->depth >= depth` and always searches at depth
  >= 1 — from treating a capture-only score as a full-width node score. The
  probe's move is passed to `scoreMoves` so the TT capture is ordered first.
  Depth-9 bench nodes fall **982,914 -> 798,364** (~19%). Strength: **0.550
  over 60 games at depth 5 (+35 Elo, +/- 88 at 95%)** against the pre-change
  build. All unit, perft, search, UCI and eval tests still pass.

- **Move generation and move ordering allocate less per node.** Two
  behaviour-preserving changes remove per-node overhead. `Position::generatePawnMoves`,
  `generateKnightMoves` and `generateSlidingMoves` now iterate the relevant piece
  bitboards (`byType_[PAWN]`, `byType_[KNIGHT]`, bishops/rooks/queens) instead of
  scanning every friendly piece four times and rejecting the wrong types inside
  the loop. `Search::scoreMoves` reuses a member `rankedScratch_` buffer instead
  of constructing a fresh `std::vector<std::pair<int, Move>>` on every call
  (which is once per interior node and once per quiescence node). Both changes
  are strictly tree-preserving — the depth-9 bench is unchanged at exactly
  **982,914 nodes** — but they cut wall time **~3.5%** at the same node count
  (depth-11 bench: old 2156-2206 ms / ~1.42 Mnps, new 2089-2126 ms / ~1.47 Mnps,
  four runs each on an idle CPU). The gain shows up as extra depth under a time
  control. Perft and all unit tests are unchanged.

- **Legality check no longer applies the move twice.** `search` and
  `quiescence` used to call `isLegalMove(m)` (a make + unmake) and then
  `doMove(m)` on the same move, so every node paid two make/unmake cycles for
  one searched move. A new `Position::doMoveIfLegal` makes the move once,
  returns whether the mover's king is safe, and undoes it itself on the
  illegal path; `isLegalMove` is now a thin wrapper over it. The change is
  strictly behaviour-preserving — the depth-9 bench is unchanged at exactly
  **982,914 nodes** — but it removes one full make/unmake per searched move, so
  the engine runs ~8% faster at the same node count (nps **1.28M -> 1.37M**
  over five runs each on an idle CPU) and searches slightly deeper under a time
  control. A fixed-depth A/B match is exactly even, as it must be for a
  same-tree change (**0.500 over 60 games at depth 5**); at 80 ms/move the
  extra depth is worth **0.542 over 60 games (+29 Elo, +/- 88)**. The futility
  test in the move loop was also hoisted above the make so a skipped move still
  costs no make/unmake.

- **Aspiration-window re-search was firing on every iteration.** In
  `Search::think`, the fail-low/high test compared the returned score against
  `alpha`/`beta` *after* the root loop had already raised them to the best
  score, so `bestScore <= alpha` was true even on a successful pass and the root
  re-searched the whole position up to four times per depth from depth 5 on. The
  searched window is now captured before the loop mutates it
  (`winAlpha`/`winBeta`), so a pass that finishes inside its window is accepted.
  Strength: **0.562 over 200 games at 80 ms/move (+43.7 Elo)** against the
  pre-change build, and the effect compounds with the eval speedup below.

- **Faster evaluation and move ordering (same play, deeper search).** Two
  behaviour-preserving rewrites cut engine time ~35% at the same node count,
  which turns into depth and strength under a time control. `evaluate()` now
  walks only the occupied squares instead of all 64, using a bitboard-driven
  mobility count built from precomputed ray tables (one `popcount` per slider
  direction). `Search::scoreMoves` ranks each move once into a `(score, move)`
  array and stable-sorts that, instead of calling `captureScore` for both
  operands on every comparison (previously O(n log n) board probes per node).
  `-O3` replaces `-O2` as the default optimisation level (~20%), with an opt-in
  `make NATIVE=1` for `-march=native` (~10% more, non-portable). Depth-9 bench
  nodes are unchanged at **982,914** (the startpos signature moved from the
  140.5M of the older, buggy-ordering build); nps rises **1.06M -> 1.33M**.
  Combined strength: **0.681 over 400 games at 100 ms/move (+132 Elo)** and
  **0.629 over a second 400 with rotated openings (+92 Elo)**, pooling to
  **0.650 over 1400 games, +107 Elo (+/- 38 at 95%)** against the pre-change
  build.

- **Internal iterative deepening.** When a node at `depth >= 6` reaches the move
  loop with no transposition-table move to order by, a `depth-2` search runs
  first and its move is picked up from the table, so the real search no longer
  starts from board order. Depth-9 bench nodes barely move (~1%), but the root
  ordering matters more than node count at a short time control: **0.600 over 200
  games at 100 ms/move (+70 Elo)**, lower bound about +21.

- **Reverse-futility and futility pruning (tight margins).** Reverse futility:
  `staticEval - 120*depth >= beta` for `depth <= 5`. Futility: skip a quiet move
  when `staticEval + 120*depth + 80 <= alpha` for `depth <= 3` (never the first
  move or in check). Both are disabled whenever the window is in mate range — the
  first attempt broke the mate-in-2 search test until those guards went in.
  Depth-9 bench nodes fall **2.4M -> 1.0M** (2.3s -> 1.0s). Strength: **0.595
  over 200 games at 100 ms/move (+67 Elo)**, lower bound about +18.

- **Late move reductions (now that ordering works).** With the TT move excluded,
  quiet non-check moves from index 4 are reduced 1 ply (2 plies from index 8) and
  re-searched on a fail-high. Depth-9 bench nodes fall **10.9M -> 2.4M** again
  (17s -> 2.3s). Strength: **0.729 over 120 games at 100 ms/move (+172 Elo)**
  against the pre-LMR build (a preliminary 60-game run gave 0.783). The earlier
  rejections in "Tried and rejected" were all made while ordering was broken;
  this is the first LMR that survives a real match.

- **Move-ordering fix (killer/history were silently dead).** `isCapture` was
  computed *after* `doMove`, so it was always true and neither killer moves nor
  the history table was ever updated for quiet moves. Classifying the move
  before it is applied restored those mechanisms: benign total nodes at depth 9
  fell **140.5M -> 10.9M** (13x) and wall time 107s -> 17s. Strength: **0.562 over
  120 games at depth 6 (+44 Elo)**, and **0.658 over 60 games at 100 ms/move
  (+114 Elo)** — the gain is larger under a time control because the node cut
  lets it search deeper. LMR was removed in the same change: it is a separate
  idea and should only return behind a fresh match.
- **Passed-pawn king proximity (endgame).** Added to `eval.cpp`: the passer's
  own king scores for escorting it, the enemy king for catching it, weighted by
  rank and only when `phase <= 12`. Measured **0.550 over 60 games at depth 6
  (+35 Elo)** and **0.562 over 40 games at depth 7 (+44 Elo)** against the
  pre-change build. The effect grows with depth because deeper search reaches
  more king-and-pawn endings.
- **Insufficient-material draw detection.** `Position::hasInsufficientMaterial()`
  now reports K vs K, a lone minor, and all-bishops-on-one-colour as draws, and
  the search returns DRAW for them (in both `search` and `quiescence`). This is a
  correctness fix rather than a strength term; a same-binary control match
  confirms the harness's side-A bias is about -47 Elo, and the change is neutral
  against it. Covered by cases in `tests/position_test.cpp`.
- A counter-move heuristic (score the quiet reply to the opponent's previous
  move just below the killers, table indexed by the moved piece) scored **0.407
  over 200 games at 100 ms/move, about -65 Elo**, while cutting only ~1% of
  nodes. It is a clear loss here; do not re-add it without a fresh match.
- A full-ply check extension (search checking moves one ply deeper) blew up the
  search: `search_test` did not finish in minutes on a forced-check position
  because every check extends again. If you want extensions, add them with a
  cap (e.g. extend only above a depth threshold, or limit total extensions).

## Verifying move generation

`make test` must pass in full. `perft_test` checks six standard positions
against published node counts; any change to `position.cpp` move generation or
make/unmake must keep all of them passing. When a perft count is wrong, use
`./build/perft_divide "<fen>" <depth>` to find the offending root move, and
cross-check with python-chess (`chess.Board`), which is installed.

## Invariants and gotchas

- **0x88 vs bitboards.** Squares are 0x88 indices (rank*16+file). Bitboards use
  a dense 0..63 index. Never shift `1ULL` by a raw 0x88 square index: the shift
  exceeds 63 and is undefined behaviour on x86. Use `bitOf`, `bitIndexOf`,
  `testBit`, and `squareOfBitIndex` from `types.h`.
- **Zobrist keys are incremental.** `putPiece`/`removePiece` XOR the piece key,
  and `doMove`/`undoMove`/null move swap the side/castling/en-passant terms.
  Never rebuild the key by rescanning the board on every move. `undoMove`
  restores the key from `StateInfo::key`, so anything that leaves the board in a
  changed state without pushing a `StateInfo` will corrupt the key.
- **`movePiece` clears the destination.** It removes any captured piece (and its
  bitboard bits) before moving. Keep it that way; skipping the destination
  removal leaves phantom enemy pieces in the occupancy bitboards.
- **En passant squares are only set when a capture is available.** The standard
  perft references assume this; always setting the ep square breaks perft.
- **Piece-square tables mirror by rank for Black**, via the table index helpers.
  A symmetric position (startpos) must evaluate to 0 for the side to move.
- **`evaluate()` returns from the side to move's point of view already.** Every
  term is signed by `(colour == sideToMove)`, so an extra "flip if Black" at the
  end negates the score for the wrong side and makes the engine play garbage.
  `eval_test` mirrors positions vertically to catch exactly this; keep that test
  passing when adding terms.
- **Transposition-table mate scores are ply-relative.** Store/search through
  `scoreToTT`/`scoreFromTT` so a mate found on one path is not reported at the
  wrong distance on another. `search_test` guards this.
- **Null-move pruning needs non-pawn material.** Do not null-move the side to
  move when it only has a king and pawns: those positions are full of zugzwang.
- Move generation is pseudo-legal plus a make/undo legality filter
  (`isLegalMove`). Do not "optimise" the filter away without perft
  revalidation.
- `captureScore` returns 0 for quiet moves, and callers (notably
  `scoreMoves`) depend on that. It is called twice per element by the
  move-ordering comparator, so `scoreMoves` now caches the value in a local
  instead of calling it twice; the function itself short-circuits the quiet
  case first. This is strictly behaviour-preserving (same rank values, same
  stable-sort order): the depth-9 bench still reports 140,502,299 nodes and
  the depth-8 bench 10,523,028, matching the pre-change baseline.

- **`squareAttacked` reads incremental slider bitboards.** The helper rebuilt
  the bishop+queen and rook+queen sets for the attacking colour on every call
  (`(byType_[BISHOP] | byType_[QUEEN]) & enemy`, etc.) — two ORs and two ANDs on
  a path taken at every node entry via `isInCheck`, every legality check and
  every castling-rights test. `bishopQueen_[COLOR_NB]` and `rookQueen_[COLOR_NB]`
  are now maintained in `putPiece`/`removePiece` (and reset in the constructor
  and `setFen`), so the helper is two loads. Strictly behaviour-preserving: the
  depth-9 bench is unchanged at exactly **798,364 nodes** and all perft, search,
  UCI and eval tests pass. A small, safe removal of work on a hot path rather
  than a measured strength change.

## Wins (measured)

- **History malus for quiet moves that fail low.** `search` only ever *added*
  `depth*depth` to the history entry of the move that caused a cutoff; every
  quiet move that was searched and did not raise alpha left its entry unchanged,
  so the ordering never learned from failures. A quiet move that fails low now
  subtracts `depth*depth` (clamped to `-HISTORY_MAX`, `1 << 14`), which makes the
  table decay stale and bad moves instead of only remembering good ones. Node
  count moves (depth-9 bench `763,949 -> 818,182`; a few of the bench positions
  search slightly wider because the changed ordering reaches different cutoffs),
  which is why it needs a match rather than the bench. Strength: **0.575 over 60
  games at depth 5 (+52.5 Elo, +/- 89 at 95%)** against the pre-change build. All
  unit, perft, search, UCI and eval tests still pass.

- **Logarithmic late-move-reduction schedule.** The reduction was a fixed
  1 ply from move 4 and 2 plies from move 8, regardless of the remaining depth.
  It now follows the standard `R = 0.77 + log(depth)*log(moveCount)/2.36` form,
  clamped to `[0, depth-2]` so a reduced search still runs at least one ply: a
  shallow node barely reduces while a deep node can drop a very late quiet move
  several plies, which is where the cost actually is. Strength: **0.535 over 100
  games at depth 6 (+24 Elo, +/- 68 at 95%)** against the pre-change build, so
  the gain is at the edge of the resolution of that match rather than a
  comfortable one. All unit, perft, search, UCI and eval tests still pass.

- **`generateCastlingMoves` bails out when no rights remain.** The generator ran
  on every interior node (`generateMoves` calls it whenever `onlyCaptures` is
  false) and, before looking at the rights, called `isInCheck(side_)` -- a full
  `squareAttacked` probe of the king -- which duplicates the check test the
  search had already made on entry to the node. Positions with no castling
  rights left (most of the middlegame and every endgame) now return before that
  probe. Strictly behaviour-preserving: no rights means no castling moves, and
  the depth-9 bench is unchanged at exactly **798,364 nodes** (depth-11 at
  2,702,670). Speed: depth-11 bench nps **~1.61M -> ~1.64M (~1.3%)** over three
  interleaved runs on an idle CPU, a small but free removal of work from every
  interior node.

- **The slider reach set is a bit-index range, not a second ray load.**
  `squareAttacked` and `eval.cpp`'s `mobilityFor` both stopped the slider walk at
  the nearest blocker `b` (already a dense bit index from `ctz`/`clz`) and then
  rebuilt the reachable set with `r ^ ray[b][d]` / `r & ~ray[b][d]` -- a second,
  randomly-indexed 8-byte table load per direction. A ray's bits are ordered
  along its direction, so the same set is the range of bit indices `<= b` for an
  increasing direction and `>= b` for a decreasing one: `r & (~0ULL >> (63 - b))`
  and `r & (~0ULL << b)`, no table lookup at all. Strictly behaviour-preserving:
  the depth-9 bench is unchanged at exactly **849,506 nodes** (depth-11 at
  2,656,300). Speed: depth-11 bench nps **~1.659M -> ~1.679M (~1.2%)** over five
  interleaved runs on an idle CPU -- a small, free removal of work from a hot
  path (every node's `isInCheck`, every legality check, every mobility count).
  A 60-game fixed-depth A/B read exactly 0.500, as it must for a same-tree
  change. A 60-game 80 ms/move A/B read 0.342 (about -114), but that is noise:
  a same-binary control match read 0.491 and the fixed-depth match was dead even,
  so treat the node-count-neutral nps measurement as the evidence, not the
  movetime match.

- **`squareAttacked` builds the slider reach set with a XOR and a shift.**
  The slider loop already had the nearest blocker as a dense bit index `b` from
  `ctz`/`clz`, but then reconstructed the blocker bit with
  `bitOf(squareOfBitIndex(b))` — two conversions that recompute `1ULL << b` — and
  cleared the squares beyond the blocker with `r & ~ray[b][d]`. `r ^ ray[b][d]`
  is the same mask (the blocker and every square before it), and `1ULL << b` is
  the blocker bit directly, so the reach set is unchanged. Strictly
  behaviour-preserving: the depth-9 bench is unchanged at exactly **798,364
  nodes**. This is a hot path (every node's `isInCheck`, every legality check and
  castling-rights test), so it is a small, safe removal of work per call rather
  than a measured strength change.


- **Killer moves are excluded from late-move reduction.** The LMR guard skipped
  only the TT move and captures, so a quiet move that had just caused a cutoff
  at the same ply (a killer) was still reduced like any other late quiet move,
  even though it is exactly the kind of move the reduction is meant to leave
  alone. `m != killers_[ply][0] && m != killers_[ply][1]` is now part of the
  reduction test. Node count moves slightly (depth-9 bench `818,182 ->
  849,506`), so this needs a match rather than the bench. Strength: **0.510 over
  100 games at depth 6 (+7 Elo, +/- 68 at 95%)** against the pre-change build,
  i.e. at the edge of that match's resolution rather than a comfortable gain.
  All unit, perft, search, UCI and eval tests still pass.

## Conventions

- C++17, no external dependencies, no cmake.
- Keep `-Wall -Wextra -Wpedantic` clean.
- Identifiers use `camelCase` (see `types.h`); search `namespace seger`.

## Playing / local UI

`tools/play.py` plays in a terminal and `tools/server.py` serves a
self-contained browser board (`tools/web/index.html`). Both drive the engine
through `python-chess`'s UCI bridge, so they are the quickest way to exercise a
change end to end. The web UI takes no external assets and needs no network.

Both HTTP endpoints return the same shape (`fen`, `turn`, `legal[]`, `status`),
including `legal` in the `/api/engine` reply. Keep it that way: the browser
client assumes it can move again immediately after the engine replies, and a
response that drops `legal` strands the player after their first move.

The board pieces are the Cburnett set vendored in `tools/web/pieces/`
(CC BY-SA 3.0; attribution in `PIECES-LICENSE`). Do not re-download them at
runtime.

`tools/lichess_bot.py` runs the same engine as a Lichess bot: it streams
`/api/stream/event`, accepts or declines challenges per its flags, and plays
each game by driving the UCI binary (no `python-chess` dependency). It must run
on a machine that stays up, with a `bot:play` token, and needs only outbound
HTTPS. See the README for the account-upgrade steps.
