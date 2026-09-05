# Metal port: design and plan

Status: **design only, no code.** This document records the decisions taken
and the plan they lead to. Nothing here is built; every number marked as an
estimate is one.

Decisions recorded (owner's):

1. Byte-identity with the CPU climb is preferred, but not at a substantial
   performance cost.
2. Host language: Objective-C++.
3. Lives in `metal/` with its own macOS-only build; the Linux `make` and CI
   are untouched.
4. Written here, built and measured on the owner's Mac (an M2 Pro first).
5. This PR is design and planning only.

## 1. Goal and scope

Move the **plugboard climb** -- the per-(key, restart) unit of work that is
~99% of a `-c` run -- onto the Apple GPU, so that the same wall time buys
more restarts. Restarts are the tool's primary quality lever (CLAUDE.md,
"Search playbook"), so throughput on this tier converts directly into
breaks on every message where the climb, not discrimination, is the limit.

In scope, in order: the plugboard-recovery tier (rotor key given, board
hidden -- the tier every tuning number in the repo is measured on), then
the keys x restarts sweep with the CPU merging results. Out of scope and
staying on the CPU: the sweep driver and key-space collapses, the
`--confidence` null (though its samples are climbs and could follow later),
`--polish` (one finisher per sweep), the doubling report, all reporting,
and every seeded path (`--crib`, `--self-crib-seeds`, `--exhaust`).

## 2. The contract: what one restart is on the CPU

Read from `src/`. The port must reproduce this; where it cannot, section 3
says how far it falls short and how that is measured.

- **Start board**: `init_steckerbrett()` (empty unless `-s`), then the kick:
  `restart_seed(key_index, restart)` (a splitmix64 finaliser over
  `opt_seed`, the flat key index and the restart index) seeds a per-restart
  stream; `perturb_steckerbrett()` pairs `2k` letters chosen by a partial
  Fisher-Yates over the unplugged, unfixed letters (`plugboard.cc:1326`).
- **Stages**: `run_stages()` runs each `--score` stage in order, each a
  `hillclimb()` capped at that stage's pair count. The recommended
  telegraphic recipe is `k4f10`: mono+IC capped at 4, then fused all-order
  + IC capped at 10.
- **The climb** (`plugboard.cc:939`): steepest ascent. Each pass scores the
  current board, then every toggle a<b over the 325 pairs -- ADD when both
  free, MOVE when one is plugged, MERGE when both are plugged elsewhere,
  REMOVE when a-b is already a pair -- skipping ADDs at the cap (and MOVEs
  under `-M`), and takes the single best improving move, with a stated tie
  rule: on an equal score a switch beats a removal. Repeat while the score
  improves. Then `try_repair()` tries every re-pairing of two existing plugs
  (both alternatives) and applies the best if it strictly improves, in which
  case the cheap climb resumes. `-J`/`-K` first-improvement is a different
  rule and is not ported (section 4).
- **Scoring**: every model accumulates **integers**. Fused (`-f`):
  `isum` over the quad-shaped `all8` table plus a 26-bin histogram from
  which `coin = sum n(n-1)`; the score is
  `isum/scale + (L-3)*bias`, divided by `L-3`, plus `30 * coin/(L(L-1))`
  (`scoring.cc:867,954`). The `k` pre-pass: `isum` over `mono8` and the same
  histogram; `(isum/scale + L*bias)/L + 0.1*L * coin/(L(L-1))`
  (`scoring.cc:251`). On the CPU the `k` stage takes the O(26) histogram
  path, which is proven byte-identical to decoding; the GPU may decode.
- **Fixed plugs**: `plug_fixed[]` (from `-s`/`--no-plug`) is read in the
  move loop and by the kick. The prototype assumes none; the sweep stage
  passes the mask.

## 3. The one hard constraint: Metal has no `double`

Metal Shading Language has no double-precision type. The CPU climb's
decisions -- which toggle wins a pass, whether a re-pair strictly improves,
the tie rule -- are comparisons of **doubles** assembled from integer
components. So:

- Every score **component** (`isum`, `coin`, per stage) can be computed
  bit-exactly on the GPU: they are integer sums, order-independent.
- The **comparison** cannot be done in double there. In float, two boards
  whose double scores differ by less than ~1e-6 (at a magnitude of ~10) may
  compare the other way, and an exact double tie may not be an exact float
  tie. The minimum non-zero score difference between two boards is
  typically ~1e-4 (one table byte or one coincidence count), so this bites
  only in near-ties -- rare, not impossible.

Consequences, and the plan for decision 1:

- **Reporting stays exactly the CPU's.** The kernel returns each lane's
  final board and integer components; the host recomputes the double score
  with `score_iter()`'s own arithmetic. Every printed score, `--dump-all`
  row and merge decision is therefore computed in double on the CPU from
  exact integers. Only the argmax *inside* the climb is float.
- **Identity is a measured rate, not a guarantee** (section 8). The
  expectation is that well over 99.9% of restarts converge to the identical
  board. Divergent restarts are not wrong, they are a different local
  optimum of equal standing; the end-to-end test is that recovery is
  statistically indistinguishable.
- Two ways to tighten it, in order of cost: (a) assemble the score in
  double-float (two floats, ~48-bit mantissa) -- a handful of extra
  operations per score, which reproduces double comparisons except in
  pathological cases; (b) change the CPU to compare in a scaled-integer
  domain, which would make CPU and GPU provably identical to each other but
  NOT to today's CPU, and would re-open every tuning measurement. (a) is in
  the plan; (b) is recorded as an option and not recommended.

## 4. Decomposition: threadgroup = rotor key, lane = restart

The opposite of enigma-cuda, which spends one thread per ciphertext letter
and runs the climb loop serially per block behind a barrier reduction per
score (`eval/results-*` survey, PR #265). That is why a GTX 1070 reaches
only ~20k key-climbs/s and underfills on a 107-letter message. The repo's
own CPU history points the same way: every attempt to vectorise *inside*
the decode measured down (six SIMD levers, +30..40%), because the decoded
letters must reach a scalar accumulator every group regardless.

So each **lane runs one complete climb**, exactly as a CPU thread does:

- A threadgroup handles one rotor key; its `rows[L][26]` table is shared
  by every restart of that key and lives in threadgroup memory.
- Each lane owns a board (26 bytes), a histogram, the best board and the
  loop state, and executes the staged steepest-ascent climb on its own
  start board.
- **Control flow is uniform** across lanes by construction: every lane runs
  the same 325-toggle scan per pass. Only the number of passes to
  convergence differs, so divergence is confined to lanes idling while the
  slowest lane in their simdgroup finishes. `-K` first-improvement accepts
  at a lane-dependent move index and is inherently divergent; it stays a
  CPU option. The GPU form is the default steepest ascent, which is also
  what the tie rule and byte-identity are defined against.
- The **kick is generated on the CPU** and uploaded as the lane's start
  board. It costs ~20 RNG draws per restart, needs 64-bit integer modulo,
  and is the one place where a GPU re-implementation could silently drift;
  keeping it on the CPU removes both the RNG and the drift from the kernel
  and makes `--biased-random` free.

## 5. Data layout and memory budget

Apple GPUs allow 32 KB of threadgroup memory per threadgroup; a simdgroup
is 32 lanes; a threadgroup may hold up to 1024 threads. Per key:

| item | size at L=107 | where |
|---|---:|---|
| `rows[L][26]` | 2.8 KB (L*26) | threadgroup memory |
| ciphertext `ct[L]` | 107 B | threadgroup or constant |
| `all8` (26^4 bytes) | 457 KB | device memory, shared, cache-resident |
| `mono8`, scale/bias/lambda | ~60 B | constant |
| per lane: board, best board, histogram, state | ~100 B | registers / threadgroup |
| 128 lanes of the above | ~13 KB | threadgroup |

So 128 restarts per threadgroup fit comfortably at operational length;
256 fit at L <= ~200. Messages above 256 letters take the CPU path in the
prototype. A later optimisation can bring the CPU's O(26) co-occurrence
table for the `k` stage on chip -- 26^3 counts fit in 17.6 KB as bytes when
L < 256 -- which is what makes that stage flat in L on the CPU; the
prototype decodes instead, which is byte-identical and simpler.

## 6. The kernel's climb

Described, not coded. Per lane, for each stage in the schedule:

1. Score the current board: decode all L positions
   (`steck[rows[i][steck[ct[i]]]]`), accumulate `isum` from the stage's
   table and the 26-bin histogram, form `coin`, assemble the float score.
2. For every pair a<b: apply the toggle (remove, or force with both
   partners ejected -- the same mutation `toggle_plan()` produces), score,
   restore; keep the best under the CPU's tie rule. Skip ADDs at the cap.
3. If the best improves, commit it and go to 1; otherwise run `try_repair`
   over the existing plugs and, if a re-pair strictly improves, commit it
   and go to 1.
4. On convergence, record the stage's board; the last stage's board and
   integer components are the lane's output.

Cost per pass is 325 x L decodes plus gathers, the same work the CPU does
per pass; the GPU wins only by running thousands of lanes at once against
a latency-bound loop, so occupancy is the number to protect.

## 7. Host: Objective-C++ linking the repo's objects

The host links every `src/*.o` except `main.o`, so it reuses -- and does
not re-implement -- option parsing, the ciphertext reader, `init()`, the
key space, `key_to_machine()` and `setup_mapping()`, the table loader and
its quantisation, `restart_seed()`/`perturb_steckerbrett()`, and
`score_iter()` for the final double. Its job per batch: build the machine
for each key, upload rows, ciphertext, the kicked start boards and the
tables; dispatch; download boards and components; hand each (key, restart)
result to the CPU's existing merge with its work index, so the
lowest-work-index tie-break and `-T`-independence are preserved verbatim.

## 8. Verification plan

Run on real fixtures (authentic HG Nord decrypts, random keys and
10-pair boards, as `eval/prepass_ab.py` draws them), at L = 60, 107, 167.

1. **Component exactness**: for fixed boards, the GPU's `isum` and `coin`
   equal the CPU's. Pure integers; **100% required**.
2. **Converged-board identity**: over N >= 1000 restarts x 20 fixtures per
   length, the fraction of (key, restart) items whose final board and
   components match `--dump-all`. Reported as a rate, with and without the
   double-float assembly. Divergent items are listed with both boards and
   both double scores so the near-tie explanation can be checked rather than
   assumed.
3. **Recovery equivalence**: paired `break50` GPU vs CPU, 300 trials per
   length, McNemar on the discordants. Must be indistinguishable; this is
   the test decision 1 actually cares about.
4. **Determinism**: two GPU runs of the same batch are identical.

## 9. Measurement plan and go/no-go

Throughput in **key-climbs per second, on wall time**, never on counters
(the repo's standing lesson: `score_iter` prices neither the gain scan nor
uploads). Cells: L = 60 / 107 / 167 x restarts per key 64 / 256 / 1024, 24
fixtures per cell (fixtures buy the interval; repetitions sharpen one
fixture's own estimate), upload and download included, against `-T 12` on
the same fixtures, plus a 10-minute sustained run for thermal throttling.

Go/no-go after milestone 3: **>= 2x** the CPU-only figure proceeds to the
sweep integration; **< 1.5x** stops, and the result is recorded like every
other measured-down lever.

## 10. Milestones

1. This document.
2. Kernel and host for the plugboard tier; component exactness (8.1).
3. Identity rate (8.2), recovery equivalence (8.3), throughput (9).
4. Go/no-go on the numbers.
5. Keys x restarts on the GPU, CPU merge, `--dump-all` and the doubling
   report fed from the downloaded boards; `plug_fixed` mask passed through.
6. Optional: `--confidence` samples on the GPU (they are the same unit);
   the on-chip co-occurrence table for the `k` stage; `--seed-dedup`
   (needs the stage-0 board returned too).

Steps 2 onward alternate: written here, built and measured on the Mac.

## 11. Risks

- **No double** (section 3): mitigated by integer components, CPU-side
  assembly for all reporting, double-float for the in-climb argmax, and a
  measured identity rate rather than an assumed one.
- **Fast-math**: the Metal compiler enables it by default. It must be off
  (`-fno-fast-math`) or the score assembly is exactly the arm64 FMA
  contraction that hung the CPU climb before `-ffp-contract=off`.
- **Divergence at convergence tails**: lanes idle while the slowest in the
  simdgroup finishes. Measure it (9); if it is large, sort restarts by the
  previous stage's plug count before the next stage, nothing more exotic.
- **Threadgroup memory** at long messages: L > 256 falls back to the CPU.
- **Gather latency** on the 457 KB table: the whole design bet is that
  occupancy hides it. If key-climbs/s scales poorly with lanes, that is
  the diagnosis.
- **Apple-silicon layout sensitivity** is a CPU finding (clang, struct
  offsets) and does not carry over, but the host's per-batch setup runs the
  same `setup_mapping` path and must not regress the CPU tool: `make
  bench` on the Mac before and after.

## 12. Expected gains (estimates, to be replaced by section 9)

Relative to the same chip's own CPU running today's tool on all cores:

| chip | GPU cores | expected |
|---|---:|---:|
| M2 Pro | 16-19 | ~3-5x |
| M-series Max | ~32-40 | ~5-10x |
| M-series Ultra | ~64-80 | ~5-10x, on a CPU already 2x the Max |

These scale my base-M1 reasoning (2-4x over its 8-core CPU) by core count
and ~20-30% per generation per core. The Neural Accelerators, ray tracing
and TFLOPS figures are irrelevant to an integer-gather workload; memory
bandwidth is not the limiter because the table is cache-resident.

Compute converts into **climb success**, not discrimination: on a message
whose true-key z sits at the best-of-K bar (RXPSB at 107 letters), no
throughput helps; on one above the crossover (BYQMZ at 167) it saturates
the climb at any restart count and makes the non-standard-kit contingencies
hours instead of days.

## 13. Build and layout

`metal/` holds `DESIGN.md` (this), and later `climb.metal`, `host.mm` and a
`Makefile` that is only ever invoked on macOS: `xcrun metal -fno-fast-math`
to compile the kernel to a `.metallib`, and `clang++ -ObjC++` linking
`src/*.o` (built by the top-level `make` first) with the Metal and
Foundation frameworks. Nothing under `metal/` is reached by the top-level
`make`, `make test` or CI; the Python and shell gates do not see `.mm` or
`.metal` files.

## 14. Open questions

- Lanes per threadgroup (128 vs 256) and threadgroups per key when
  `-R` exceeds it: measure, do not guess.
- Whether the `k` stage's histogram form is worth bringing on chip before
  or after the go/no-go.
- Whether to return the stage-0 board so `--seed-dedup` can keep working
  unchanged on the CPU side.
