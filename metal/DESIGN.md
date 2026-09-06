# Metal port: design and plan

Status: **milestone 2 done and verified on a GPU; milestone 3 measured,
and the port is 4-6x SLOWER than the CPU on an M1 -- with a named,
untested suspect (per-lane arrays spilling to thread memory).** This
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
- **Milestone 3 is MEASURED, and the GPU is 4-6x SLOWER than the same
  chip's CPU** at every length and restart count on an M1 mini
  (`eval/results-gpu-throughput-m1.txt`). Two runs: the first starved both
  arms (one key per fixture) and its cells are superseded; the second, at
  26 keys, fills the device and saturates it. **Occupancy is excluded** --
  16x the restarts now buys 1.3x, so ~1700-4700 climbs/s is the ceiling.
  What remains is per-lane speed, and the gap is **~500-750x per ALU
  against a CPU core** where clock and the absence of out-of-order
  execution explain 10-30x. That 20-50x excess is the thing to chase, and
  it has a named suspect: every per-lane array in the kernel (`steck`,
  `freq`, the saved best board) is **dynamically indexed**, which on Metal
  means thread memory rather than registers -- i.e. the decode's two
  lookups are dependent memory round trips. **Section 17 is the analysis
  and the plan**: the decomposition is the fault, not the tuning, and the
  ladder of experiments there says how to establish that before the
  simdgroup-per-climb design it proposes is built.

Decisions recorded (owner's):

1. Byte-identity with the CPU climb is preferred, but not at a substantial
   performance cost.
2. Host language: Objective-C++.
3. Lives in `metal/` with its own macOS-only build; the Linux `make` and CI
   are untouched.
4. **The target is the M1**, and in principle any Apple silicon GPU;
   CUDA (section 15) and OpenCL (section 15b) later. This line used to
   read "an M2 Pro first", which was a note about which machine was
   expected to be to hand rather than a target, and it misled: the port
   was built and measured on an **M1 mini**, and that machine stays the
   instrument. An M2 Pro laptop has served once, as a cross-check on a
   second chip (17.7).
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
only ~20k key-climbs/s and underfills on a 107-letter message. **This
paragraph is REVERSED by section 17.5, on measurement**: 20k on a 2016
card is ten times what this design reached on an M1, and the CPU lesson
the next sentence rests on does not transfer to a GPU. The repo's
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
  are to be CONFIRMED before anything else is built -- milestone 2's
  first check. **Confirmed on the M1**, the target: identity is exact
  (the status header above). If it were unavailable, 32-bit weights would
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

| chip | GPU cores | expected | MEASURED (section 9) |
|---|---:|---:|---|
| **M1 (the target)** | 8 | ~2-4x | **0.17x** |
| M2 Pro | 16-19 | ~3-5x | **0.17x** (one cell, 17.7) |
| M-series Max | ~32-40 | ~5-10x | - |
| M-series Ultra | ~64-80 | ~5-10x, on a CPU already 2x the Max | - |
| RTX 4090-class (CUDA) | 128 SMs | ~20-40x a base M1 GPU (section 15) | - |

**Every row is wrong by ~20x and the last column says so.** They scale a
base-M1 reasoning (2-4x over its 8-core CPU) by core count and ~20-30%
per generation per core -- and the scaling was the sound part: the two
measured chips differ by 1.85x on the GPU and 1.88x on the CPU, so the
RATIO is flat and the per-core scaling holds. What was wrong is the
anchor, because section 4's decomposition cannot use a core (section
17). Read this table as an estimate of what the hardware could give a
kernel that suits it, not of what this one does.

The Neural Accelerators, ray tracing and TFLOPS figures are irrelevant to
an integer-gather workload; memory bandwidth is not the limiter because
the table is cache-resident.

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
| `throughput.py` | section 9's cells, wall time, startup subtracted |
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

**If that refuses with "requires Xcode ... active developer directory
... is a command line tools instance", check `xcode-select` before
assuming Xcode is missing.** On the second machine Xcode was installed
and only the pointer was wrong; one command fixed it:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

If Xcode genuinely is absent the CLT never ships a Metal compiler, and
the way round it is to skip compiling there: a `.metallib` is AIR
bytecode the driver finishes for the specific GPU at load time, so one
built on another Apple silicon machine works. `make -C metal
enigma-metal` builds the host alone with the CLT's clang and frameworks;
copy `climb.metallib` beside it or point `$ENIGMA_METALLIB` at it.

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

## 17. Why milestone 3 disappointed, and what to do about it

Written after the two M1 runs (`eval/results-gpu-throughput-m1.txt`),
before any of it is built. The conclusion is that the port is slow
because of its **decomposition**, not its tuning, and the section ends
with the ladder of experiments that would establish that cheaply before
the bigger change is paid for.

### 17.1 The one number

At saturation the M1's GPU spends **~900-1030 lane-cycles per decoded
character** (1741 climbs/s x 833 497 characters per climb at L=167,
against 8 cores x 128 ALUs x 1.28 GHz). A decode step is ~10-15
instructions on either machine: two board lookups, a row lookup, a table
gather, a histogram increment, window shifts. The CPU spends **3-5
core-cycles** on the same step at IPC 3.3. So a GPU lane is **stalled
about 98% of its cycles**. This is not an arithmetic deficit -- a GPU ALU
and a CPU core retire simple integer ops at similar rates -- it is
latency, unhidden.

A CPU hides latency with out-of-order execution: within one probe the
characters' load chains are independent and the core overlaps dozens. A
GPU lane is in-order and hides nothing by itself; the hardware hides
latency only by switching to OTHER simdgroups, i.e. by occupancy. Section
4's design serialises a ~4 500-probe x L-character chain onto each lane
(the CPU's shape) and loads each lane with enough private state that few
simdgroups can be resident. That is the worst of both: CPU-shaped work on
hardware without OoO, at an occupancy too low to compensate.

### 17.2 Where the cycles go, ranked, each with its experiment

- **(a) Per-lane arrays in thread memory -- the suspected majority.**
  Metal keeps a `thread` array in registers only when the indexing is
  provably static. `steck[26]` is read twice per character with dynamic
  indices, `freq[26]` is read-modify-written once per character, and
  `try_repair`'s `plo/phi/rp/rv` are the same shape. Dynamically indexed
  thread arrays go to thread memory -- device memory behind a cache -- so
  that is three dependent round trips per character at a few hundred
  cycles each, which IS ~900. Experiment: ablation kernels that replace
  `steck` with an arithmetic identity and drop `freq`, as cost probes.
- **(b) Occupancy throttled by per-lane state.** 64-bit accumulators
  everywhere (emulated on Apple GPUs), the arrays above, loop state. The
  free datum is the `GPU:` line the host prints:
  `maxTotalThreadsPerThreadgroup` below 1024 means the compiler's own
  register count is capping residency. `isum <= 255*L` and `coin <= L^2`
  fit 32 bits; accumulate in 32 and widen once per probe.
- **(c) Divergence -- real, ~2x, not the 20-50x.** Lanes converge after
  different pass counts, so a simdgroup runs to its slowest lane
  (section 11 named this); ~1.7x at a mean of ~14 passes. The
  `paired`/`else` branch adds ~1.2x. Experiment: a fixed pass count.
- **(d) The `all8` gather**, 457 KB, random, per character: 6% on the CPU
  because OoO hides it, fully exposed on a lane. Intrinsic; only
  occupancy or memory-level parallelism hides it.
- Minor: `rows_tg[i*26 + x]` read by 32 lanes at the same `i` and random
  `x` spans ~7 banks -- several-way conflicts.

### 17.3 Fixes inside section 4's design: worth doing, not enough

`steck` to a 26-byte slice of threadgroup memory per lane (6.6 KB at 256
lanes beside the 6.9 KB of rows); `freq` to `uint16` slices (13 KB; 26.5
of 32 KB in all, or 128 lanes); 32-bit accumulators. Threadgroup memory
is on-chip. Expect **several x, perhaps 5-10x** -- parity with the CPU,
probably not section 12's 2-4x -- and (c) and (d) are untouched. Its
real value is diagnostic: it separates PLACEMENT from DECOMPOSITION
cheaply, before the bigger change.

### 17.4 The answer: simdgroup = one climb, lane = a slice of characters

enigma-cuda's thread-per-letter idea at simdgroup granularity, with
cross-lane primitives in place of barriers:

- **The board is distributed across the simdgroup**: lane `j` holds
  `steck[j]`, and `steck[x]` is one `simd_shuffle`. No thread memory,
  and no threadgroup memory for boards at all. A probe's <= 4 mutations
  are conditional register writes (`lane == a ? b : mine`); restore is
  the same.
- **The decode is spread, not serial.** Each lane decodes its 2-8
  characters, with the ciphertext preloaded into registers once per
  climb (zero loads). The quad window crosses lane boundaries: three
  `simd_shuffle_up`s fetch the left neighbour's last letters.
- **`isum` is one `simd_sum`** -- an integer sum, exact and
  order-independent, so byte-identity survives.
- **The histogram**: each lane's partial counts packed as 26 bytes in 7
  words (a lane holds <= 8 characters, so a byte never overflows; 32 x 8
  = 256 per bin at L <= 255), reduced with seven `simd_sum`s, then
  `sum n(n-1)`. ~100 ops per probe per lane against the ~150 000 stalled
  cycles a probe costs a lane today.
- **No divergence, by construction**: one climb per simdgroup in
  lockstep, so the convergence tail is gone and the `paired` branch is
  uniform. No barriers anywhere; simdgroup ops need none.
- **`climb_body.h`'s control flow is untouched** -- probe order, the tie
  rule, the cap gate, `try_repair`, the stages. Only `mc_components` and
  the board representation change, so `verify_identity.py` remains the
  oracle and must still read `RESULT: identical`.
- **The one-key tier fills the machine too**: 64 restarts = 64
  simdgroups = 2 048 lanes, twice an M1. Run 1's starvation cannot recur.

Arithmetic, marked as such because section 12's arithmetic was 20x high
for the wrong design: ~160 ops per probe per lane, ~5 independent gathers
in flight per lane, ~4 500 probes per climb -> ~1 ms per climb per
simdgroup; 8 cores x several resident simdgroups -> **20-60k climbs/s at
L=167**, against 1 741 measured and 10 252 for the CPU. That is section
12's range: the estimate may have been right for the design it should
have been paired with.

### 17.5 enigma-cuda, revisited

Section 4 dismissed thread-per-letter with "that is why a GTX 1070
reaches only ~20k key-climbs/s". Twenty thousand on a 2016 card is **ten
times** what section 9 measured on the M1, and ~5x per ALU. The lesson
section 4 borrowed to justify lane-per-climb -- every SIMD attempt inside
the CPU decode measured down -- is about vector lanes in one core feeding
a scalar accumulator across the vector/scalar boundary. It does not
transfer: on a GPU the reduction is a native cross-lane operation and the
letters run on separate ALUs. Section 4 read that evidence backwards.
The modern form is 17.4, simdgroup-level with shuffles rather than
block-level with barriers.

CUDA as a TARGET is friendlier -- native 64-bit integers, 255 registers
per thread, 48-100 KB of shared memory, `__shfl_sync`/
`__reduce_add_sync`, and `nvcc -Xptxas -v` printing spills and register
counts, the diagnostic Metal makes one infer -- but the same
lane-per-climb kernel would die the same death there, since CUDA's local
memory spill is the same phenomenon. Fix the decomposition on Metal,
where the harness exists; CUDA inherits it behind three macros (shuffle,
sum, shuffle-up).

### 17.6 The experiment ladder

In order; each is cheap and rules something in or out before the next is
paid for.

0. **Read the `GPU:` line** from one direct run: free, and it says
   whether registers already cap occupancy. **DONE: `384 threads per
   threadgroup`.** A lean kernel reads 1024, so each lane uses ~2.7x the
   register budget and a core holds at most 12 simdgroups of it. The
   dispatch shape then halves that: a 256-lane threadgroup fits once per
   core (512 > 384), and a 64-lane one fits four times by threadgroup
   memory -- **8 simdgroups per core in every cell**, i.e. two per
   32-wide SIMD unit to interleave, which cannot hide one memory round
   trip, let alone three per character. That is also why the device
   rate was flat across `-R`. Cross-check: 8 x 32 x 8 cores = 2 048
   climbs in flight at 1 741/s is 1.18 s per climb, against the 1.24 s
   inferred independently from the `-R 64` cell. What 384 does NOT
   settle is whether the arrays sit in thread memory (cost = latency) or
   were promoted to registers with select chains (`freq[26]` alone would
   be 26 registers; cost = ALU, and it explains 384 by itself). Step 1
   tells them apart for free by reading this line per variant: if
   dropping the histogram lifts 384 toward 1024, `freq` was in
   registers. One free knob falls out: `lanes_per_tg = 128` fits three
   threadgroups per core under both limits (384 lanes, 20.7 KB) against
   one today -- +50% occupancy for a constant; add it to step 1,
   expecting 20-40% and a confirmation rather than a fix.
1. **RUN ONCE, CONFOUNDED, INSTRUMENT REBUILT.**
   `metal/ablate.py` at one cell (L=107, `-R 256`, `--keys 26`): four
   ablations -- no histogram (`freq[26]`, 17.2(a)/(b)); no board lookups
   in the decode (the two `steck[]`, 17.2(a)); 32-bit accumulators
   (17.2(b)); no `all8` gather (17.2(d)) -- plus the baseline at
   `lanes_per_tg = 128`. Each is its own metallib (`make -C metal
   ablate`), because the register cap is a property of the COMPILED
   pipeline and a runtime branch would allocate for the union and report
   one number for all of them. Results and the full reading:
   `eval/results-gpu-ablation-m1.txt`.

   **Variants 1, 2 and 4 return a WRONG board** -- they delete work the
   answer depends on, which is what a cost probe is -- so the host prints
   a banner under `$ENIGMA_GPU_ABLATE`, skips the CPU/GPU component check
   and exits 0 regardless. **Variant 3 is the exception and is a
   candidate FIX rather than a probe**: 32-bit accumulators are
   answer-preserving, verified on the CPU backend over 1 536 restarts at
   L=60/107/167 (0 differing rows), `isum` and `coin` being bounded by
   `L` times a byte and by `L(L-1)/2` while the blend `A*isum + B*coin`
   is still formed in 64 once per climb. **Measured 1.01x: free, and not
   a speedup.**

   **THE CONFOUND, WHICH IS THE MAIN THING STEP 1 TAUGHT.** An ablation
   that changes the SCORE changes the climb's trajectory, hence the
   number of passes before convergence, and **climbs/s does not normalise
   for that**. Variant 2 makes the decode independent of the board, so no
   move ever improves and the climb exits after ONE pass per stage rather
   than the ~16 it takes naturally; it duly read **20.36x** and was
   nearly all work not done. Its fingerprint was in the output all along
   -- a per-simdgroup spread of exactly 1.00, i.e. every lane computing
   an identical score. Normalised against the measured mean of 15.83
   passes, what is left for the two `steck[]` lookups is **~2.6x**, and
   that is a lower bound, since a 2-pass climb amortises the per-climb
   fixed costs over an eighth as much work. Rows 1 and 4 have the same
   defect; row 3 was the only readable one, precisely because it is
   answer-preserving.

   **The instrument now separates the two axes.** `MC_PASSES` (report the
   pass count) and `MC_FIXED_PASSES` (run exactly N passes, whatever the
   score does) are switches ORTHOGONAL to `MC_ABLATE`, so every variant
   can be read in passes/s and, better, pinned to identical work. Being a
   fifth mutually exclusive *variant* is exactly what stopped the old
   pass counter from catching this: it could only ever report the
   baseline's trajectory, never that of the variant whose number needed
   normalising. `backend_cpu.cc` mirrors the pass-count output, so the
   machinery is verifiable with no GPU -- natural reads mean 15.825 and
   max/mean 1.480, fixed reads 16.000 and 1.000 exactly.

   **What run 1 does establish**, none of it touched by the confound:
   the **register cap is 384 in every variant**, a compile-time property,
   and deleting `freq[26]` outright does not lift it -- so the per-lane
   arrays are in **thread memory and the cost is latency, not ALU**,
   which is what step 0 could not settle. It leaves a new question: if
   none of the four ablations moves the cap, something none of them
   removes is setting it. **Divergence is 1.48x**, not the ~2x guessed in
   17.2(c) -- about a third of lane-cycles masked off, real but not the
   story, and the CPU backend reproduces it to three digits, so the
   remaining divergence questions need no Mac time. And **128 lanes buys
   26% for nothing**, inside step 0's predicted 20-40%.

   **Cost: a few minutes, not the "about an hour" this step used to
   claim.** 63 invocations at ~3.3 s each is under four minutes and the
   estimate was written without multiplying it out. It is recorded
   because it shaped the design: the instrument was kept small to fit an
   hour that was never at risk, and the fixed-pass family that would have
   prevented the confound was dropped from this step for that reason.
   **RUN 2, THE CORRECTED INSTRUMENT, ANSWERS STEP 2 WITHOUT RUNNING
   IT.** Fixed work, so the shares are the measurement:
   board lookups **32.2%** of the scan loop, `all8` gather **22.8%**,
   histogram **8.2%**, accumulator width **4.2%**, everything else
   **32.6%** -- *nothing dominates*, the largest item is a third, and
   deleting all four entirely is 3x. The A-vs-B gap supplies the rest:
   the fixed build does *more* passes (16.00 against 15.60) and is
   **1.94x faster**, of which 1.46x is divergence and 1.33x is
   `try_repair` and the outer loop. In shares of the real kernel that is
   divergence **31.5%**, `try_repair` **16.9%**, board lookups **16.6%**,
   gather **11.8%**, other scan work **16.8%**, histogram **4.2%**,
   accumulator **2.2%**. The two largest items are the two placement
   cannot touch, and `try_repair` at ~17% is its own surprise -- the CPU
   notes call it "~zero cost" because it fires only at convergence, and
   on a lane it is not.
2. **The placement fix (17.3) IS DEAD AS SPECIFIED -- SKIP IT.** This
   step expected 5-10x. It addresses the board lookups and the histogram,
   20.8% of the run, so its **ceiling is 1.26x, measured rather than
   estimated**, and the gather cannot join it: 457 KB does not fit in
   32 KB of threadgroup memory. Against the 4-6x needed merely to reach
   CPU parity that is not a route. The fork is **17.4 or stop**.
3. **Prototype 17.4**, now the only route rather than the ambitious one.
   What run 2 adds to its case is that it attacks the largest item
   **structurally**: with all 32 lanes on one climb they take the same
   branches, so the 31.5% divergence goes to zero by construction, not by
   tuning. Its payoff is still bounded by nothing measured, so 17.7's
   warning against trusting its estimate stands.
   - **One free thing first, needing no kernel change and
     answer-preserving -- SHIPPED as `MC_LANES_DEFAULT = 64`.**
     `lanes_per_tg = 64` is the peak of the sweep at **1.27x** -- not the
     128 run 1 suggested from a single point -- and the curve has a
     mechanism: a threadgroup is one rotor key, so once lanes fall below
     the 256 restarts a key spans several groups and each re-loads its
     own `rows[]`, which is why 32 falls back. `MC_LANES` stays 256 as the
     maximum (it sizes threadgroup memory and bounds `$ENIGMA_GPU_LANES`,
     which now exists for the sweep); the settings line reports the width
     in use and marks an override. The 1.27x is one chip and one cell,
     so the sweep stays in `ablate.py` for the next chip.
   - **The 32-bit accumulators are NOT a second one, and a claim here
     that they were has been retracted.** Table B's row 3 read cap 448
     against 384, and that was written up as the accumulators moving the
     cap, with the prediction that pairing them with 64 lanes could pay
     (`floor(cap/lanes)` going 6 -> 7). Run 3 crossed the two and **both
     arms read 384 at every lane count**: the 448 belongs to the
     *fixed-pass* build of variant 3, where `try_repair` is compiled out,
     and the natural build reads 384 with 32-bit sums in every run --
     including table A's row 3 of the very run the claim came from. The
     pairing measured 1.01-1.02x flat, which is the accumulators' own
     ~1-2% and exactly the "gain where grp does not differ" the table was
     written to catch. What survives is a partial answer to what sets the
     cap: it is a stepped function of register count, the natural kernel
     sits just above a step, and `try_repair`'s inlined body is among
     what holds it there.
   - **Before building it, three things, in cost order.** (i) **DONE:
     the per-probe cost against length** (`ablate.py --scaling`,
     `eval/results-gpu-ablation-m1.txt` run 4). The redesign spreads a
     probe's per-character work across 32 lanes and leaves its per-probe
     work where it is, so the intercept of `t(L) = a + b*L` is the share
     it cannot touch. Measured **1.4 + 0.405*L ns per probe**, residuals
     0.2 ns, intercept **3.0% at L=107**: not intercept-limited, which
     was the go/no-go. **A "17.4 bound" column from the same run is
     withdrawn** -- it was per-probe latency, not throughput. The
     redesign also puts 32x fewer climbs in flight, so with resident
     lanes unchanged its payoff is the per-character **step-latency
     ratio** (shuffle and threadgroup memory against thread memory) times
     freed-register occupancy times 1.46 for divergence, and if the step
     latency did not fall it would be a ~2x *loss* at L=107. Also from
     the fit: at L=107 a lane holds 3.3 characters, so the ~10 cross-lane
     reductions per probe are amortised over very little -- the design
     favours long messages and the operational cell is short. (ii) **BUILT:
     the probe microkernel** (`probe_body.h`, `probe.metal`,
     `probe_host.cc`; `$ENIGMA_GPU_PROBE=1` on either host binary runs it
     instead of the sweep). One toggle probe -- apply a toggle, score the
     board -- repeated `$ENIGMA_GPU_PROBE_N` (default 64) times from the
     same kicked board under the same deterministic toggle sequence, in
     five arms that do identical work: **lane**, today's shape, which is
     `mc_components()` verbatim; and **group**, the 17.4 shape, at K = 32
     with the board spread one entry per lane and read by `simd_shuffle`,
     and at K = 32, 16 and 8 with the board **replicated in every lane as
     five packed words** (26 five-bit entries, six to a word, every access
     a select chain -- pure ALU, K-independent, and the reason K can be a
     knob at all). Each lane decodes L/K characters; `isum` and a
     **nine-word packed histogram** (27 nine-bit fields, so a field holds
     the 256 a bin can reach after the reduction) are reduced across the
     K lanes by an xor butterfly; the three quads straddling a lane
     boundary are scored from the previous lane's last three letters
     fetched by `simd_shuffle_up`, which is why a lane must hold at least
     three characters (L >= 3K; the host says n/a otherwise). **No per-lane
     array anywhere**, so nothing a compiler could send to thread memory.
     The three cross-lane operations are macros in the `climb_body.h`
     style -- `MC_SHFL`, `MC_SHFL_UP`, `MC_SHFL_XOR` -- so the CUDA
     target inherits the body behind `__shfl_sync`.
     **Every unit's two int64 checksums are compared against the tool's
     own `score_components()` replaying the same toggles**, on every run;
     the lane arm is additionally run through `probe_body.h` on the CPU
     before anything is dispatched, separating a body bug from a wrapper
     bug. A shape that is fast and wrong prints FAIL, not a rate, and the
     FAIL path was proven able to fire by linking a deliberately wrong
     lane arm (`-DMC_ABLATE=4`): it reported the mismatch and exited 2.
     The packed board, its toggle operator and the histogram were
     unit-tested against the array forms on 3 000 random boards. Sizing
     is by time (a pilot, then ~0.4 s per arm, min of three), rates are
     in probes per second so K does not enter the comparison, and each
     arm's own register cap is printed since the five kernels are five
     pipelines in one library.
     **RUN ONCE ON THE M1 -- valid, and confounded** (`eval/results-gpu-
     probe-m1.txt`). All five arms compiled at the first attempt and
     every unit checked bit-exact. Every group arm was SLOWER than the
     lane: 0.46x at K=32 (shuffle and packed alike), 0.70x at 16, 0.90x
     at 8; fitting the K trend puts the per-character step at 0.89 of
     the lane's in lane-time, i.e. the shuffles and packed-ALU lookups
     replaced the thread-memory reads and the step barely moved, and the
     reductions are what scale with K. Caps: shuffle 1 024, packed 896,
     lane 512 -- against the climb kernel's 384. **But every unit ran the
     same board**, so the lane arm's 32 lanes read the same `rows` entry
     and gathered the same cell at every character, a broadcast the
     group arms get nothing from; and the lane arm's 71M probes/s
     against the fixed-pass climb's 22M for the same scoring is more
     than the cap explains. So the bias is one-way against the group
     arms, of unknown size, entangled with how much the climb's scan
     machinery itself costs. Fixed: unit u starts from board u % 64, the
     64 kicks for (key 0, restart 0..63), so a simdgroup holds 32 boards
     as a climb does; the oracle is 64 checksums.
     **RUN 2, 64 BOARDS: THE BROADCAST WAS WORTH 6%, AND 17.4'S
     DECOMPOSITION IS DEAD.** The lane arm fell 71.0M -> 66.7M probes/s
     and the group arms did not move: 0.49x at K=32 (both boards),
     0.74x at 16, 0.93x at 8. Refitting the K trend puts the group
     shape's per-character step at **0.94 of the lane's** in lane-time.
     In lane-cycles per character the lane arm's step is ~750 and the
     K=8 packed arm's ~1 300: both shapes are stalled for nearly all of
     a 15-50-instruction step, and **the one with NO thread memory is
     the slower**. Whatever the ~750 cycles are, they are not the
     thread-memory board and histogram 17.2 ranked first -- which table
     B had priced at 32% and 8%, the same fact from the other side.
     **THE FINDING IS THE OTHER NUMBER.** The lane arm -- `mc_components`
     on a toggled board, exactly what the climb does 326 times a pass --
     runs **66.7M probes/s where the fixed-pass climb runs 22.3M**: the
     same scoring, **3.0x slower inside the climb kernel than beside
     it**, confound excluded. The cap covers 512/384 = 1.33x of that at
     most; the remaining ~2.2x is the kernel around the scorer -- the
     scan's kind logic and cap check, the mutate and restore writes to a
     thread-memory board (up to eight a probe against the probe arm's
     two to four), the best-move compare, and whatever the compiler
     makes of `mc_components` inlined inside a function large enough to
     be register-starved. 17.2 ranked four suspects and none was this,
     because every ablation removed part of the *scorer*; the scan
     machinery sat in table B's "everything else" (32.6%) and in the
     per-probe intercept table D put at 3% -- which it cannot be if the
     probe arm is right, and the probe arm is the one checked against
     the tool's scorer. **Two arms added to settle it**, both cheap:
     `lane packed regs` (the group template at K = 1: today's
     decomposition with the board and histogram in registers, no
     shuffles, no reductions -- whether thread memory matters at all
     once the scan is gone) and `climb pass (scan)` (16 passes of the
     real scan loop, `mc_pass()`, split out of `mc_hillclimb()` for the
     purpose and verified byte-identical -- the scan machinery priced in
     the same harness as the scorer; its probes/s should reproduce the
     ablation table's 22.3M, a check on both instruments).
     **RUN 3: THE SCAN MACHINERY IS 74% OF THE CLIMB, AND THE CAUSE IS
     THE SCORER RUN TWICE.** `climb pass (scan)` -- the real loop, one
     fixed-pass climb's work per unit -- runs the scorer at **0.26x** the
     rate the scorer alone runs it (17.7M against 66.7M probes/s, cap
     384 against 512); against the ablation table's 22.3M it is 21%
     slower, which is the stage mix (half of table B's passes were the
     cheap `k4` stage) and not a disagreement. Occupancy covers 1.33x of
     the 3.8x. The rest is the toggle's kind branch: `if (paired)
     {remove; score; restore} else {force; score; restore}` is one
     `mc_key` on the CPU and **two on a GPU** whenever any lane of the
     simdgroup is in each branch -- and with 32 boards of ~10 pairs,
     that is most of the 325 toggles. That also reconciles table D:
     its 3% intercept says the scan adds no *fixed* cost per probe,
     which is true, because the scan's cost is per-*character* -- the
     scorer executed twice -- a multiplier on the slope that a linear
     fit cannot see. **Fixed in `mc_pass()`**: one branch-free
     mutate/score/restore for all four kinds, same writes, same scores,
     same tie rule, `verify_identity.py` byte-identical, one `mc_key`
     per toggle. **Run 4: the pass arm went 17.7M -> 33.0M probes/s,
     1.87x**, every other row unchanged. **AND THE CLIMB DID NOT MOVE:
     2 401 climbs/s before, 2 402 after, rebuilt by hand.** Same change,
     +87% in one kernel and +0% in the other, so the double scoring was
     a property of how the small probe kernel was *compiled* -- `mc_key`
     inlined once per branch and never merged -- and not of the scan
     loop; in the large climb kernel the compiler had already merged
     the two identical calls. The 1.87x was the pass arm becoming
     honest, not the climb becoming faster, and "the climb carries it"
     above was true of the code and false of the speedup. **A
     microkernel measures its own compilation; the transfer to the real
     kernel is a separate measurement**, and it was the owner's plain
     run that made it. The branch-free form stays (byte-identical, and
     what makes the pass arm fair) but it is not a lever.
     What remains between the honest scan and the scorer alone is 2.0x:
     the cap (384 against 512) for 1.33x and ~1.5x for the eight
     mutate-and-restore writes per toggle to a thread-memory board. The
     corrected ledger at this cell, probes/s: scorer in registers
     101.5M, scorer in thread memory 66.5M, the scan loop 33.0M, the
     climb at fixed passes ~25M fused-equivalent, the climb as it runs
     12.2M. **The honest ceiling of the current design on this chip**,
     with measured factors for every layer: if the packed-board scan
     reaches the scorer's ~60M, a climb rebuilt on it projects to ~23M
     natural, ~4 500 climbs/s -- 1.9x today's and a third of the CPU's
     13.1k. Parity would need the natural climb to run at the
     scorer-in-registers rate with every layer above it free.
     **Measured next as `climb pass packed`**: `mc_pass()` transcribed
     onto the five-word board line for line (`mc_pass_pk`, with the K = 1
     decode as its scorer), checked against the *array* pass's oracle so
     the two representations are proven to agree before it becomes the
     climb's body. **Run 5: 48.7M probes/s, 1.48x over the array scan,
     bit-exact on the GPU, and the cap did not move (384).** The gain is
     the per-lane step -- the eight thread-memory writes per toggle
     gone -- not occupancy: the scan's own live state pins its
     registers, not the board. The scan's overhead over its scorer is
     unchanged in ratio, 2.08x against the register-resident scorer
     where it was 2.00x against the thread-memory one. **The ledger is
     complete.** Scorer in registers 101.2M; packed scan 48.7M (x2.08);
     climb at fixed passes ~37M (x1.3, stages and the cap check); climb
     as it runs ~19M (x1.46 divergence, x1.33 `try_repair`), i.e.
     **~3 700 climbs/s: 1.5x today's and 0.28x the CPU's 13.1k** at the
     operational cell. Every factor is measured on a kernel that exists
     and checks against the tool. There is no unmeasured lever left in
     the lane-per-climb design, 17.4's decomposition is dead at
     0.49-0.94x, and the CPU side of every ratio is the steepest-ascent
     tool rather than the recommended `-K`, so the real gap is wider.
     **On Apple silicon this design cannot reach its own CPU.** The
     absolute rate scales with resident lanes times clock, and a
     discrete CUDA part carries ~30x the M1's lanes at ~2x the clock
     against a CPU of perhaps 2x -- the same kernel projects to parity
     or a small multiple there, as arithmetic, with section 12's record
     as the warning and enigma-cuda's 20k key-climbs/s on a GTX 1070 as
     the order-of-magnitude check. The Mac was the instrument, and the
     instrument's job is done.
     **And `lane packed regs` reads 1.52x** (101.2M probes/s, cap 896):
     today's decomposition with the board and histogram in registers,
     no thread memory at all. 896/512 = 1.75x more lanes resident for
     1.52x the throughput, so the per-lane step is ~15% *slower* (the
     select chains cost more ALU than thread memory costs latency --
     the group arms' finding again) and the win is entirely occupancy.
     It is the best per-probe rate of any shape here and beats the best
     group arm (K=8 packed, 62.2M) by 1.63x: **17.4 is dead against the
     fair comparison too.**
     **WHERE THIS LEAVES THE PORT.** The scorer with a register-resident
     board runs 101M probes/s and the climb runs 17.7M. That 5.7x is
     the scan machinery plus the occupancy it costs, both inside the
     current lane-per-climb design, and parity needs ~4.6x. For the
     first time the ceiling is not an estimate but the measured rate of
     a kernel that exists, scoring the same boards to the same integers.
     The levers, in order: one `mc_key` per toggle (done, measured
     next); the packed board in the climb itself, so mutate and restore
     are ALU and the kernel's footprint drops toward the 896 the lane
     arm reads; then whatever the pass arm still shows above the scorer
     alone. Step 3 as written -- prototype 17.4 -- is withdrawn.
     (iii) **No kill number -- the owner's call, and consistent with
     decision 6.** A pre-registered bar (under 4x per probe stops the
     redesign) was proposed and declined: the number from (ii) is judged
     with the rest of the information in hand, not against a threshold
     fixed before it exists. For orientation only, not as a bar: the
     CPU at L=107 is ~8.5-13k climbs/s against the GPU's 2 832 at 64
     lanes, so parity is ~4.6x on the real kernel, of which divergence
     removal supplies 1.46x by construction and `try_repair` supplies
     nothing. And write (ii) against three macros in the `climb_body.h`
     style (shuffle, sum, shuffle-up), which is what keeps the CUDA
     target real at no cost.
4. Only then `--sustained`, and the `k` stage's co-occurrence table on
   chip (`uint8`, 17.6 KB, fits; section 5).

### 17.7 What not to do -- revised after the probes

**Two sentences in the first version of this section were wrong, and
the ladder measured them wrong.** It said a Max or an RTX 4090 "would
show the same ratio to its own CPU", and "do not port to CUDA as-is".
The per-lane pathology *is* chip-independent -- 0.169x on the M1 mini,
0.166x on an M2 Pro, cap 384 on both, and the probes put the same
~750-cycle step on every shape tried -- but the ratio to the CPU is
not, because the absolute rate scales with resident lanes times clock
and the M2 Pro merely scales both sides alike. A discrete CUDA part
carries ~30x the M1's lanes at ~2x the clock against a CPU of perhaps
2x: the same kernel projects to parity or a small multiple there. So
the revised list is:

- Do not tune section 4's design further on Apple silicon: every layer
  is measured (17.6 (ii)), the ceiling is 1.5x today's and a third of
  the CPU's, and 17.4 is dead at 0.49-0.94x.
- Do not build 17.4. It was the ambitious route and it measured slower
  than the shape it was meant to replace, on both the unfair comparison
  and the fair one.
- Do not trust the projection for CUDA more than section 12 deserved --
  the probe should run on a CUDA card *before* any port work, and the
  body is already behind three macros so that it can.
- Do not read a microkernel's gain as the kernel's. The one lever that
  read 1.87x in the probe read 0% in the climb, because the probe's
  small kernel had been compiled differently.

### 17.8 What transfers to the CPU code

Very little of the code, and one of the GPU's wins would be a loss: the
packed-register board measured **8x slower** on the CPU backend, where a
26-byte array in L1 costs a cycle and a select chain a dozen. Three
things do transfer.

1. **The CPU scorer is at its floor, and the GPU says why.** The same
   dependent chain -- plugboard, rotor row, plugboard, table -- costs
   ~750 cycles on a GPU lane and 3-5 on a CPU core, and the entire
   difference is out-of-order execution overlapping consecutive letters.
   That is the fact `CLAUDE.md` records from the other side (half the
   loop's gathers miss L1 and it does not matter, IPC 3.3, every SIMD
   attempt lost). There is no scorer trick left to find; the chain is
   the chain, and the CPU hides it.
2. **A trajectory-confounded benchmark**, now in `CLAUDE.md` beside the
   `score_iter` warning: a scoring change that is not byte-identical
   changes the pass count, and time per climb then measures work not
   done. The bench's climb tiers are exposed to it exactly as the
   ablations were.
3. **`try_repair`'s cost is not zero**, measured once: 16.9% of a
   natural climb on the GPU at L≈105. The CPU share is unmeasured and
   may differ, but it is a reason to run the short-length A/B the
   `--no-repair` flag exists for -- `ENHANCEMENTS.md`, Measurement gaps.
