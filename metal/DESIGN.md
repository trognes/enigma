# Metal port: design and plan

Status: **milestone 2 is DONE and verified on a GPU; milestone 3 (§9's
throughput) is next.** This
document records the decisions taken and the plan they lead to; every
number marked as an estimate is one. What exists (section 13 has the
build):

- `--int` on the CPU (milestone 1b, PR #267): identity 0 of 5 760 restarts
  differing, zero discordant recovery trials, bench inside every floor.
- `climb_body.h`, the one kernel body; `climb.metal` and `backend_metal.mm`,
  the Metal wrapper and host; `backend_cpu.cc`, a reference backend that
  runs the same body per lane on the CPU; `host_common.cc`, the host that
  both share; `verify_identity.py`, section 8's identity test as a `diff`
  of `--dump-all` rows. On Linux, against the CPU's `--int` climb: **0
  differing rows of 3 840** at L = 60/107/167 (20 fixtures x `-R 64` per
  length), every board's components exact, and the wildcarded sweeps
  (17 576 starts, a two-notch right wheel, an M4 order) agreeing key for
  key with `search_worker()`.
- **RUN ON A GPU, an M1 mini, and identity holds there.**
  `verify_identity.py --host metal/enigma-metal --sweeps` reads **RESULT:
  identical**: 0 differing rows of 3 840 at L = 60/107/167, 0 differing
  decrypts, every board's components exact, and all nine wildcarded sweeps
  agreeing key for key. **That closes section 11's first risk** -- the
  64-bit integer multiply-add section 3a rests on is available and exact on
  Apple silicon, which was the thing to confirm before anything else was
  built. Identity is therefore by construction *and* measured on the
  target, so section 3's float-comparison worry never applies under
  `--int`.
- Two things had to be fixed to get there, both platform facts rather than
  design errors: the Metal compiler is a separate download since Xcode 16
  (section 13), and a dispatch had to be capped by WORK rather than by
  bytes or the GPU watchdog resets the machine (section 11).
- **Next: section 9's throughput**, milestone 3 -- and read the occupancy
  note in section 11 first, because at `-R 1` the kernel uses one lane of
  every 32 and that is the first number section 9 will report.

Decisions recorded (owner's):

1. Byte-identity with the CPU climb is preferred, but not at a substantial
   performance cost.
2. Host language: Objective-C++.
3. Lives in `metal/` with its own macOS-only build; the Linux `make` and CI
   are untouched.
4. Written here, built and measured on the owner's Mac (an M2 Pro first).
5. This PR is design and planning only.
6. **No pre-set throughput bar** for milestone 3: produce section
   9's numbers, then decide. The earlier ">= 2x proceeds, < 1.5x
   stops" is withdrawn.

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
- **This constraint is Metal's, not the design's.** NVIDIA GPUs have
  `double`, so the CUDA target (section 15) can compare exactly as the CPU
  does and reach strict byte-identity. The kernel body is written so the
  score type is a compile-time choice.

### 3a. The preferred route: integer scores on the CPU (`--int`)

Rather than make the GPU approximate the CPU's double, let the CPU offer
an arithmetic both sides can perform **exactly**: keep the climb's
decisions entirely in 64-bit integers. Then identity is by construction,
not measured, and no floating-point number is touched inside the climb.

- **The arithmetic.** Within a run `L`, `nterms`, `scale`, `bias` and
  `lambda` are constants. The fused score is
  `S = isum/(scale*nterms) + bias + lambda*coin/(L(L-1))`. `bias` is the
  same for every board and never affects a comparison; multiplying the
  rest by the positive constant `scale*nterms*L(L-1)*M` preserves the
  ordering exactly, leaving one linear form

      I = A*isum + B*coin,   A = L(L-1)*M,   B = round(lambda*scale*nterms*M)

  with `M` a power of two chosen once. The `k` pre-pass has the same
  shape, `A = (L-1)*M`, `B = round(lambda_k*scale*L*M)`. Two integer
  weights per model per run, computed on the CPU at start-up; the climb
  compares `I` in `int64`.
- **Precision.** With `M = 2^20`, `A*isum <~ 3e17` and `B*coin <~ 1e17`,
  inside `int64` with headroom. The only rounding anywhere is `B`'s, about
  5e-12 relative -- four orders finer than double's own rounding of the
  same expression and five finer than float. Decisions can differ from
  today's double ordering only for near-ties inside that band, which a
  real pair of boards essentially never produces; genuine score steps are
  ~1e-4. Exact ties (`I` equal) then mean identical components in
  practice, and the tie rule settles them exactly as now.
- **Why it beats float.** `int64` add and multiply are identical on the
  CPU, Metal and CUDA: no correctly-rounded-division question, no
  fast-math, no contraction. The whole FMA / `-ffp-contract` hazard class
  leaves the climb; `hist_probe` and `score_iter` agree because they
  produce the same integers and the same `I`. It is also cheaper -- two
  64-bit multiply-adds per score against ~400 loads, no FP division
  (Metal emulates 64-bit multiply; two per score is nothing).
- **One choke point.** The double expression is written out today in
  each `*_score_decode` and again in `hist_probe`. Under `--int` they all
  produce `(isum, coin)` and one `compare()` on `I` decides; the double is
  reconstructed from the same pair by today's formula only for reporting.
  So `--dump-all`, `--confidence`, the merge and every progress line read
  exactly as now -- they are per restart, not per probe, and they are what
  a user reads.
- **Why an option, not a replacement.** The default stays the double
  ordering, byte-identical to today's binary and to every measurement in
  the repo. `--int` is the reference the GPU targets reproduce exactly,
  the GPU host turns it on (or refuses without it), and the two are
  compared in the repo's preferred form: one binary, two flags. It is
  echoed by `show_settings()` like any other flag that changes results.
- **It is still a behaviour change and is measured as one**, in its own
  CPU-only PR before any kernel: `make test` (an exact-output fixture
  would need updating only if a 1e-12 near-tie happened to flip), a paired
  `break50` A/B of `--int` against the default at L = 60/107/167, ~2000
  trials each -- the expectation is ZERO discordant trials, and that is
  the measurement, not the claim -- and `make bench`, expected neutral or
  slightly faster. If it holds, section 8.2's identity rate becomes a hard
  100% requirement on Metal as on CUDA. Decision 1's "substantially"
  applied to `--int` itself: **any measurable recovery loss rejects it**,
  and `make bench` must read inside its own floor.
- **Scan order is part of the contract.** Under `--int` ties are exact.
  On an exact tie between two SWITCH moves the CPU keeps the first found,
  because its comparison is strict (`score > move_score`) and the scan
  runs `a < b` ascending over the 325 pairs; only the switch-over-removal
  rule breaks a tie the other way. A kernel that scans in any other order
  reproduces every score and still diverges on ties, so the order is
  specified, not left to the implementer: `a` outer, `b` inner, both
  ascending, exactly as `hillclimb()` has them.

### 3b. Fallbacks

If `--int` is declined or measures down: a `--float` assembly (the same
choke point computing the final expression in float and widening to
double; provably identical across targets too, but only with
correctly-rounded division verified on each, and with near-ties at ~1e-6
rather than ~1e-12), or a double-float assembly on the GPU alone (two
floats, ~48-bit mantissa), which leaves the CPU untouched but makes
identity a measured rate.

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
  slowest lane in their simdgroup finishes. The GPU form is therefore the
  default **steepest ascent** first -- also the rule the tie rule,
  `--dump-all` and byte-identity are defined against, so identity can be
  tested against today's default output with nothing else changed.
- **`-J`/`-K` first-improvement is reproducible but divergent, and is
  measured second, not excluded.** SIMT executes divergent lanes
  correctly by masking them, so a lane running `-K` produces exactly the
  CPU's `-K` result; what divergence costs is utilisation. On the CPU,
  first-improvement wins at matched compute because it is ~2.8x cheaper
  per climb and buys more restarts. On a 32-lane group a step ends when
  every lane has found an improvement, so its cost is the slowest lane's
  scan, which late in a climb tends back toward the 325 evaluations
  steepest pays anyway. Whether `-K` keeps enough of its 2.8x to beat
  steepest at matched wall time is a milestone-6 A/B; its ordering scan
  needs the co-occurrence table on chip (17.6 KB), the same optimisation
  the `k` stage wants. `-A` is uniform by construction but measured worse
  than greedy on telegraphic traffic and is not planned.
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

Three consequences of that arrangement are worth stating rather than
leaving to be rediscovered:

- **The `--confidence` null stays valid unchanged.** The repo requires the
  null to be calibrated by the SAME unit the sweep runs (`climb_unit()`
  routes both through one helper for exactly this reason). Under `--int`
  the GPU climb IS the CPU climb bit for bit, so CPU-climbed null samples
  calibrate a GPU sweep with nothing to re-derive. Under the fallbacks
  (3b) that argument weakens to "the same distribution up to near-ties",
  which is one more reason to prefer `--int`.
- **Ordering changes, and so does what the progress line means.** The CPU
  sweep is restart-major so that the answer is front-loaded and a watcher
  can kill a long sweep early. The GPU runs ALL of a key's restarts at
  once, so its natural order is key-major: the front-loading property
  survives in a different form -- every finished key is complete at its
  full `-R` -- and the live line's "pass" field stops meaning anything and
  is replaced by keys done of keys total. The final answer, the merge and
  `--dump-all` are unaffected; only the order in which candidates appear
  in the log differs.
- **Batching and pipelining are unspecified for the prototype** and needed
  for milestone 5: keys per dispatch (enough to keep several thousand lanes
  resident, section 5), and overlapping the next batch's upload of rows
  and start boards with the current batch's compute. Uploads are
  kilobytes per key and the table goes up once, so this is scheduling,
  not bandwidth.

## 8. Verification plan

Run on real fixtures (authentic HG Nord decrypts, random keys and
10-pair boards, as `eval/prepass_ab.py` draws them), at L = 60, 107, 167.

1. **Component exactness**: for fixed boards, the GPU's `isum` and `coin`
   equal the CPU's. Pure integers; **100% required**.
2. **Converged-board identity**: over N >= 1000 restarts x 20 fixtures per
   length, the fraction of (key, restart) items whose final board and
   components match `--dump-all` under `--int`. **100% required** on
   both targets once section 3a is in; any divergent item is a bug and is
   listed with both boards and both scores. Under the fallbacks (section
   3b) it is reported as a rate. **Mechanism**: the host prints its results
   in `--dump-all`'s own row format, so the test is a `diff` against the
   CPU's `--dump-all --int` output on the same fixtures -- no bespoke
   comparer to get subtly wrong.
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

**There is NO pre-set bar** (owner's decision 6). This used to read ">= 2x
proceeds, < 1.5x stops"; the numbers are to be produced first and judged
then. Two reasons that is the better order here. The measurement is not of
one thing: the cells span L and `-R` by an order of magnitude each, and a
port that wins at `-R 1024` and loses at `-R 64` fails a single threshold
while being exactly what the search playbook wants, since restarts are the
lever compute is meant to buy. And the first numbers will understate the
hardware by a known, fixable factor -- at small `-R` the kernel uses one
lane of every 32 (section 16's second open question), so a bar applied
before that is answered would be judging the mapping rather than the
design.

Recording a number and then deciding is also what this repo does
everywhere else: `--seed-dedup`, `-K` and the `k4f10` pre-pass were each
measured, written down, and weighed against their cost afterwards -- and
the one place a bar WAS pre-registered, the `-S k` entry in CLAUDE.md, it
failed on a tie at L = 60 and the token was adopted anyway on judgment.
The value of pre-registration is that it stops a claim drifting to fit its
evidence; that is served by publishing the cells, which section 9 requires
regardless.

## 10. Milestones

Rough effort, to set expectations rather than commit to them:

1. This document.
1b. **`--int` on the CPU** (section 3a), its own PR: the `(isum, coin)`
   components with one `compare()` choke point, the per-run weights, the
   flag and its echo, `make test`, the paired `break50` A/B in one binary,
   `make bench`. Establishes the reference the GPU must match exactly.
   ~2 days plus the measurement.
2. Kernel and host for the plugboard tier; component exactness (8.1).
   First thing on the Mac: confirm 64-bit integer multiply in the kernel
   (section 11). ~1 week.
3. Identity rate (8.2), recovery equivalence (8.3), throughput (9).
   Days, dominated by the measurement runs.
4. Judge it on the numbers (decision 6: no pre-set bar).
5. Keys x restarts on the GPU, CPU merge, `--dump-all` and the doubling
   report fed from the downloaded boards; `plug_fixed` mask passed through;
   batching and pipelining (section 7). ~1-2 weeks.
6. Optional: `--confidence` samples on the GPU (they are the same unit);
   the on-chip co-occurrence table for the `k` stage; a matched-wall-time
   A/B of `-K` first-improvement against steepest ascent on the GPU
   (section 4); `--seed-dedup` (needs the stage-0 board returned too).

Steps 2 onward alternate: written here, built and measured on the Mac.

## 11. Risks

- **No double** (section 3): removed rather than mitigated by keeping the
  climb's decisions in `int64` (section 3a), with the double reconstructed
  from the same integers for reporting only; the float and double-float
  fallbacks (3b) remain if `--int` is declined.
- **64-bit integers on Metal**: the 1e-12 precision claim in section 3a
  rests on `int64` multiply-add in the kernel. Apple GPUs support 64-bit
  integer types, but multiply is emulated and its availability and cost
  are to be CONFIRMED on the M2 Pro before anything else is built --
  milestone 2's first check. If it were unavailable, 32-bit weights would
  drop the precision to float level and `--int` would lose its edge over
  the 3b fallbacks, though not its exactness across targets.
- **Fast-math**: the Metal compiler enables it by default. Under `--int`
  the climb contains no floating-point operation at all, so this is now
  defence in depth for the fallbacks and for any FP left in reporting; it
  is still set off (`-fno-fast-math`), because the cost is nothing and the
  failure it guards against is the arm64 FMA contraction that hung the CPU
  climb before `-ffp-contract=off`.
- **The GPU watchdog, and it is no longer hypothetical.** macOS resets the
  GPU when one command buffer runs too long, and the reset takes the
  desktop with it: on an M1 mini the first `--sweeps` run froze the
  machine, and the second appeared to hang at the same case. A batch was
  bounded only by BYTES (~64 MB of rows), which does not bound duration --
  and the case that broke it, 228 488 keys at `-R 1`, is the kernel's
  worst shape, since `lanes_per_tg = restarts` puts ONE thread in each
  threadgroup. `MC_ITEMS_PER_DISPATCH` (4 096, `$ENIGMA_GPU_BATCH_ITEMS`)
  now caps the climbs in a dispatch; the two sweeps that survived that
  machine dispatched 8 788 and 17 576, so the default sits below the
  smaller rather than beside the larger. Splitting is result-neutral by
  construction and measured so -- 8 788 keys at 1, 138, 1 256 and 8 788
  dispatches give byte-identical `--dump-all` rows. **Treat raising it as
  a change whose failure mode is the user's desktop, not an error line**,
  and note the milestone-3 lever underneath it: at `-R 1` the kernel wastes
  31 of 32 SIMD lanes, so a key-per-lane mapping would be both faster and
  shorter-running.
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
| RTX 4090-class (CUDA) | 128 SMs | ~20-40x a base M1 GPU (section 15) |

These scale my base-M1 reasoning (2-4x over its 8-core CPU) by core count
and ~20-30% per generation per core. The Neural Accelerators, ray tracing
and TFLOPS figures are irrelevant to an integer-gather workload; memory
bandwidth is not the limiter because the table is cache-resident.

In absolute terms, with a base M1 GPU at an estimated 30-60k key-climbs/s,
a full exact `-r A..` sweep (230M keys) at 128 restarts per key is 6-11
days there and roughly 6-16 hours on an RTX 4090-class card.

Compute converts into **climb success**, not discrimination: on a message
whose true-key z sits at the best-of-K bar (RXPSB at 107 letters), no
throughput helps; on one above the crossover (BYQMZ at 167) it saturates
the climb at any restart count and makes the non-standard-kit contingencies
hours instead of days.

## 13. Build and layout

`metal/` holds `DESIGN.md` (this) and **one kernel body and thin
wrappers**: the climb itself is plain integer C in `climb_body.h`, shared
by every target, and each wrapper supplies only the address-space
keywords, the thread index, the barrier and the integer type.

| file | what |
|---|---|
| `climb_body.h` | the climb: components, key, toggle scan, `try_repair`, stages |
| `climb.metal` | the Metal kernel `enigma_climb` around it |
| `host_common.cc` / `.h` | `main()`: keys, rows, kicks, batches, reporting, merge |
| `backend_metal.mm` | uploads a batch and dispatches the kernel (macOS) |
| `backend_cpu.cc` | runs the body per lane on the CPU (any platform) |
| `verify_identity.py` | section 8.2 as a `diff` of `--dump-all` rows |
| `Makefile` | `make -C metal` (reference), `make -C metal metal` (Metal) |

Both hosts link every object under `src/` except `main.o`, built by the
top-level `make` first, so the tool's own option parsing, key space,
kicks, tables and reporting are reused rather than re-implemented
(section 7). `enigma-metal` needs `xcrun metal -fno-fast-math` for the
`.metallib`, loaded from beside the executable or `$ENIGMA_METALLIB`, and
`clang++ -ObjC++` with the Metal and Foundation frameworks. **The Metal
compiler is not part of Xcode's default install since Xcode 16** — it is a
downloadable component, and until it is fetched `xcrun metal` fails with
"cannot execute tool 'metal' due to missing Metal Toolchain". One command
fixes it, once per machine (the first Mac build stopped here):

```sh
xcodebuild -downloadComponent MetalToolchain
```

`enigma-ref`
builds anywhere and is the same program with the body run on the CPU: it
is how the body was verified on Linux, and how a GPU disagreement is
split into a wrapper bug or a body bug. Both take the tool's own command
line and print its own output (`--dump-all` rows included), plus a
`Components:` line for section 8.1 and a device-time line for section 9;
they refuse, by name, every option this milestone does not port. `climb.cu`
+ `host_cuda.cc` will build with `nvcc -fmad=false` on a machine with an
NVIDIA GPU (section 15). Nothing under `metal/` is reached by the
top-level `make`, `make test` or CI; the Python and shell gates do not
see `.mm`, `.metal` or `.cu` files.

## 14. Metal-first, then CUDA

Metal is built and measured first because it runs on the hardware to hand;
the CUDA wrapper is a near-free second target of the same kernel body and
the friendlier one for exactness and raw parallelism. Milestones 2-4 are
run on Metal; the CUDA wrapper is added at milestone 5 or as soon as an
NVIDIA machine (a rented cloud GPU is enough) is available to measure it.

## 15. The CUDA target

The design maps one-to-one; nothing in it is Apple-specific.

| design element | Metal | CUDA |
|---|---|---|
| one rotor key | threadgroup | block |
| one restart running a full climb | lane | thread |
| SIMD width | simdgroup, 32 | warp, 32 |
| `rows[]` for the key | threadgroup memory, 32 KB | shared memory, 48-228 KB |
| `all8` | device memory | global memory, L2-resident |
| kicks | CPU-generated, uploaded | same |
| host | Objective-C++ linking `src/*.o` | C++ under nvcc linking `src/*.o` |

Three things CUDA does better:

1. **`double` exists**, so CUDA can match either CPU setting exactly: the
   default double ordering, or `--int` through the same integer path the
   Metal target uses (section 3a). FP64 runs at 1/32-1/64 rate on consumer
   cards, but the assembly is three operations per score against ~400
   loads, so it is invisible either way. On CUDA, decision 1 is met in
   full with or without `--int`.
2. **More shared memory**: the `k` stage's 17.6 KB co-occurrence table fits
   beside `rows[]` with room to spare, and 256+ restarts per block fit at
   any message length.
3. **Far more lanes in flight**: a base M1 holds ~8k resident lanes, an
   RTX 4090 ~260k (128 SMs x 2048). The design's whole bet is hiding gather
   latency with independent lanes, so this scales almost directly.

One trap carried over from the repo's own history: **`-fmad=false` is
load-bearing.** nvcc contracts multiply-adds into FMAs by default even
with fast-math off, and the CPU is built with `-ffp-contract=off`; without
it the double assembly can differ from the CPU's in the last bit, which is
the arm64 hang in another coat and would silently forfeit the
byte-identity CUDA otherwise makes possible. Verification on CUDA is
therefore the simplest of all: `--dump-all` must match **exactly**, and
section 8.2's identity rate must read 100%.

Discrete cards copy over PCIe rather than sharing memory; the per-batch
traffic (rows, boards, a 457 KB table once) is kilobytes to megabytes and
does not matter. Where enigma-cuda's per-letter design would still be the
better one -- very long messages with few restarts per key -- is not the
operational regime here, and this design is independent of L.

## 15b. A third target: OpenCL, for an AMD card on Windows

Not planned; recorded as an option because the question came up (an RX
7700 XT, RDNA 3, `gfx1101`, in a Windows 11 machine) and the answer is
cheap to keep.

- **The route is OpenCL, not HIP.** OpenCL C is C99 with address-space
  qualifiers -- `__global const` for the tables, `__local` for the key's
  `rows[]`, `__private` for the board -- which is exactly what
  `climb_body.h`'s macros were built to supply, so the wrapper is smaller
  than `climb.metal`. 64-bit integers are core OpenCL, the kernel compiles
  from source at run time (no offline toolchain), and the AMD Adrenalin
  driver ships the runtime on Windows. HIP would be the CUDA twin, but
  AMD's Windows HIP SDK lists the 7900 series and `gfx1101` support could
  not be confirmed; WSL2 does not help either, since AMD's compute support
  inside it is limited to listed cards. The same OpenCL wrapper would also
  run on NVIDIA and Intel GPUs, and on a CPU through PoCL -- which is how
  it could be verified on Linux before touching the card, exactly as the
  reference backend verified the body.
- **The hardware fits the design.** RDNA 3 runs 32-wide wavefronts, the
  width section 4's uniform-control-flow argument assumes; 64 KB of local
  memory per workgroup holds `rows[]` with room for the `k` stage's
  co-occurrence table; 64-bit multiply is not native, but as on Metal it
  is two operations per score against ~400 loads.
- **The obstacle is the repository, not the GPU.** Nothing here has ever
  been built on Windows: `src/` uses `getopt_long`, `unistd.h`,
  `getrusage` (the peak-memory line), `isatty` (the progress line) and
  pthreads, and the host links every object under `src/`. A plain MSVC
  build is therefore out; MSYS2's GCC toolchain provides those headers,
  and its OpenCL headers and loader packages link against the Windows
  `OpenCL.dll` that the AMD driver answers. The first step on such a
  machine is `make` and `make test` under MSYS2, before any GPU work.
- **Files it would add**: `climb.cl` (the wrapper), `backend_opencl.cc`
  (context, queue, run-time compile, upload, read-back -- the shape of
  `backend_metal.mm`), an `opencl` target in `metal/Makefile`, and a note
  of the MSYS2 packages. The body, the host and the harness are unchanged.
- **Expected gain, an estimate in section 12's sense only**: 54 compute
  units against the M2 Pro's 19 GPU cores puts the card between the
  Max-class Apple rows and the RTX 4090 row, if the gather latency hides
  as the design bets. The measurement is section 9 on that machine,
  judged the same way -- which is now on the numbers rather than
  against a pre-set bar (decision 6).

## 16. Open questions

- Lanes per threadgroup (128 vs 256) and threadgroups per key when
  `-R` exceeds it: measure, do not guess.
- Restarts per key below 32 waste lanes in a simdgroup/warp; whether to
  pack several keys' `rows[]` tables into one threadgroup for broad sweeps
  at small `-R`, or simply require `-R >= 32` on the GPU path.
- Whether the `k` stage's histogram form is worth bringing on chip before
  or after the go/no-go (on CUDA the memory is there from the start).
- Whether to return the stage-0 board so `--seed-dedup` can keep working
  unchanged on the CPU side.
- When an NVIDIA machine becomes available for the CUDA measurement.
