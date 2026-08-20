/* The Enigma machine itself: the per-search state, the rotor stepping, and the
   substitution that falls out of it.

   WHAT IS INLINE HERE IS INLINE ON PURPOSE. step26/diff26/add26 and decode_at
   are the innermost arithmetic of the scan and of every scorer; they must be
   visible to the modules that call them, not behind a call. decode() likewise
   -- the scorers fuse decoding into their own loops, but everything else that
   needs a plaintext goes through it.

   struct machine is what makes the search reentrant: each worker thread owns
   one, while the wiring tables, the n-gram statistics and the ciphertext stay
   shared and read-only. Its layout is performance-critical -- see CLAUDE.md on
   why subst_array is reached through a pointer rather than stored inline, and
   why the hot loops hoist member base pointers into __restrict locals. */

#ifndef ENIGMA_MACHINE_H
#define ENIGMA_MACHINE_H

#include "common.h"
#include "options.h"
#include "text.h"
#include "wiring.h"

/* The rotor-stack substitution for every (start - ring) triple of one wheel
   order, ring fixed at 0: 26^3 rows of 26 bytes, 457 KB. Built once per
   reflector x wheel order by precompute(), then read by every ring/start of
   that order through the start-ring offset. */
typedef unsigned char (* subst_table)[asize][asize][asize];

/* Derived from the wiring by init(). Only these two are read outside this
   module -- by the key-space collapses, which simulate the stepping to find
   which (ring, start) pairs are indistinguishable. rotor_fwd/rotor_rev,
   notch_gap and reflector are private to machine.cc, which is where every
   read of them lives. */
extern unsigned char notch[rotor_count][asize];
extern unsigned char notch_halfperiod[rotor_count];

/* Per-search mutable machine state. Grouping it into one object (rather than
   file-scope globals) makes the search reentrant: a future worker thread can own
   its own machine while the read-only wiring tables, n-gram statistics and
   ciphertext stay shared. */
struct machine
{
  unsigned char steckerbrett[asize];    /* plugboard permutation */
  /* Per character position, the 26-byte rotor-stack substitution row. The
     scorers read rows[i][k]; rows[i] either points straight into the shared
     subst_array (the brute-force scan -- no copy) or into the contiguous
     mapping[] below (hill-climbing, which re-reads each row hundreds of times
     and benefits from the locality). */
  const unsigned char * rows[maxlen];
  unsigned char mapping[maxlen][asize]; /* hill-climb's contiguous row copies */
  char plaintext[maxlen+1];             /* candidate / result */

  int ukw;                              /* reflector index (thin UKW-b/c in M4) */
  int walzenlage[wheels];               /* wheel order (rotor indices) */
  unsigned char grundstellung[wheels];  /* start positions */
  unsigned char ringstellung[wheels];   /* ring positions */

  /* effective reflector actually applied by subst_rotors: a plain copy of
     reflector[ukw] normally, or greek-folded thin reflector in M4 mode (set once
     per task by set_effective_reflector, before precompute -- never in the hot
     loop, which reads the precomputed subst_array). */
  unsigned char reflector_eff[asize];
  int greek;          /* M4 Greek rotor index (Beta/Gamma), else -1 */
  int greek_offset;   /* M4 (Greek start - ring) mod 26, else 0 */
  int scoring;        /* active scoring model (= opt_scoring; varied per stage by
                         the staged plugboard climb, so it is per-machine not the
                         global -- a shared global would race across worker threads) */

  /* The 457 KB rotor-stack table (rebuilt once per wheel order by precompute,
     read per ring/start by setup_mapping) is heap-allocated separately and
     reached through its own base pointer. Keeping it out of the struct leaves
     every member above at a small offset -- so both the hot decode tables and
     subst_array itself get tight addressing on every compiler/target, rather
     than whichever one lands past the big array paying for large offsets (which
     ARM in particular handles poorly). */
  unsigned char (* subst_array)[asize][asize][asize];

  /* Diagnostic counter: number of plugboards scored (score_iter calls) by this
     worker. Bumped once per whole-message score -- not per character -- so it is
     out of the hot per-character loop, and placed last so it never pushes the hot
     tables above to large struct offsets. Summed across workers for the final line. */
  uint64_t plugboards_scored;

  /* Echo intermediate climb improvements as progress lines? Set by the workers:
     true in the main search and the -F tier-2 climb, false in the -F tier-1 filter
     (whose IC scores are not comparable with the ranking model). Cold -- read only
     on ACCEPTED climb moves (see report_climb_progress), never in the scoring scans. */
  bool report;

  /* Per-worker scratch for --exhaust (see the PLUG_FIXED_EX note below): a copy of the global
     plug_fixed (-s seed) plus this leaf's forced-pair letters, read ONLY by the EX=true climb
     instantiations. Under g++ this lives here, in the machine (a thread_local shifts g++'s
     whole-TU codegen); under clang it lives in a thread_local instead (a struct member shifts
     clang's climb codegen), so it is compiled out of the struct there. */
#if !defined(__clang__)
  bool plug_fixed_ex[asize];
#endif
};

/* Modular arithmetic on alphabet positions. Everything the machine holds -- a
   grundstellung, a ringstellung, a rotor-table entry, a letter -- lives in [0, 25], so
   these three narrow forms cover every site in the program except the three noted on
   mod26_full() below. Each compiles to a compare and a conditional move, where a general
   `% 26` compiles to a magic-constant division.

   That distinction was worth a lot more than it looks. setup_mapping() used a general
   form SIX times per character (three step increments, three ring offsets) and
   rotor_l/rotor_r twice each per rotor stage; narrowing them measured setup_mapping
   -39% and precompute -40% in instructions, and `make bench` search -33% under g++ /
   -41% under clang. The reason it went unnoticed is that it is invisible under `-c`
   (setup_mapping is under 0.1% of a hill-climb) and only bites the plain scan, where it
   was 53.8% of instructions at 300 characters. */
inline int step26(int x)          /* x + 1 mod 26, for x in [0, 25] */
{
  return (x == asize - 1) ? 0 : x + 1;
}

inline int diff26(int a, int b)   /* a - b mod 26, for a, b in [0, 25] */
{
  const int d = a - b;
  return (d < 0) ? d + asize : d;
}

inline int add26(int a, int b)    /* a + b mod 26, for a, b in [0, 25] */
{
  const int s = a + b;
  return (s >= asize) ? s - asize : s;
}

/* The only general form left, and it has exactly three callers -- all in the
   --ring-stride refinement, which derives ring1/ring0/start0 from step-count DIFFERENCES
   rather than from positions. A candidate start1 far from the coarse winner's can differ
   by several steps, and that difference is subtracted from a position, so the result is
   bounded by nothing and none of the narrow forms above will do. An earlier version used
   a single-alphabet `(x + 26) % 26`, which is correct only for x >= -26; UBSan caught it
   as a negative subst_array index that then read out of bounds. */
inline int mod26_full(int x)
{
  return ((x % asize) + asize) % asize;
}

/* Decode one ciphertext position: plugboard -> per-position rotor-stack row ->
   plugboard. A tiny inline so decode() and the scorers share one copy of the
   formula. rows[i] is the position's substitution row (in subst_array for the
   scan, or the contiguous mapping[] for hill-climb). The scorers fuse this into
   their loops (see below) so the decoded text is never materialised in a scratch
   array. The base pointers are __restrict locals the callers have hoisted out of
   struct machine. */
inline int decode_at(const unsigned char * __restrict steck,
                     const unsigned char * const * __restrict rows,
                     const unsigned char * __restrict ct,
                     int i)
{
  return steck[rows[i][steck[ct[i]]]];
}

inline void decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  char * __restrict pt = m.plaintext;
  for (int i = 0; i < textlength; i++)
    pt[i] = num2char(decode_at(steck, rows, ct, i));
  pt[textlength] = 0;
}

void init();
void init_walzen(machine & m, int u, int a, int b, int c);
void init_ring_grund(machine & m, int a, int b, int c, int x, int y, int z);
void set_effective_reflector(machine & m);
void precompute(machine & m);
void setup_mapping(machine & m, bool copy_rows);

/* Cumulative step counts of the middle and left wheels per character position,
   and the tools the --ring-stride refinement uses to turn a difference between
   two such schedules into the ring settings that produce it. */
void step_counts(int w1, int w2, int g1, int g2,
                 unsigned short * mid, unsigned short * left);
int step_deltas(const unsigned short * a, const unsigned short * b,
                int * out, int cap);
int widen_deltas(int * out, int n, int band, int cap);

#endif
