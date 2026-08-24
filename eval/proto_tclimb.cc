/* PROTOTYPE, not part of the program.  Can the CAP-4 PRE-PASS CLIMB (m4/i4/k4)
   be run on the T representation instead of by decoding?

   eval/results-tseed-proto.txt showed a single-plug score costs O(26) on T
   rather than O(L) by decoding, and measured 4-51x per toggle.  But it scored
   only the SIMPLE case -- one plug added to an EMPTY board, which moves two
   board entries and so touches four columns of T.  The climb's toggle operator
   has four cases and the expensive ones dominate a cap-4 pre-pass:

     case                       entries changed   T columns
     add     (both free)              2               4
     remove  (already paired)         2               4
     move    (one end plugged)        3               6
     merge   (both plugged)           4               8

   Starting from a 10-plug kick under a cap of 4, ADDS ARE BLOCKED and most of
   the 325 pairs are merges, so the realistic delta is roughly twice the
   prototype's.  This measures the real mix rather than assuming it, and checks
   BYTE-IDENTITY, which is the precondition for using it at all: a climb that
   scores differently makes different decisions, and the whole point is to make
   the same ones faster.

   WHY BYTE-IDENTITY IS EVEN POSSIBLE.  decode_at is
   steck[rows[i][steck[ct[i]]]].  Write q_i = rows[i][steck[ct[i]]] for the
   decrypt BEFORE the exit board, and n(q) for its histogram.  The scorers
   build freq[] over the decrypt AFTER it -- but steck is an involution, hence
   a bijection, so freq[steck[y]] == n_y and freq is n PERMUTED.  Therefore

     IC    sum_z freq[z](freq[z]-1)  ==  sum_y n_y(n_y-1)      (permutation)
     mono  sum_z freq[z]*mono8[z]    ==  sum_y n_y*mono8[S[y]]  (relabelling)

   and both scorers accumulate in INTEGERS (long isum, int coin), so the T form
   produces the same integers and the identical double falls out.  Not "close
   to" -- the same bits.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/tclimb eval/proto_tclimb.cc $objs
     /tmp/tclimb [L] [trials] [reps]
*/

#include "common.h"
#include "machine.h"
#include "ngrams.h"
#include "options.h"
#include "plugboard.h"
#include "scoring.h"
#include "text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

static const int TSIZE = asize * asize * asize;

static uint16_t Tt[TSIZE];
static uint8_t lp8[asize];              /* the same bytes load_table gives */
static double lp_bias, lp_scale;
static double lambda = 0.1;             /* monoic_lambda_default */

static void build_T(machine & m)
{
  memset(Tt, 0, sizeof Tt);
  for (int i = 0; i < textlength; i++)
    {
      const int c = num_ciphertext[i];
      const unsigned char * row = m.rows[i];
      uint16_t * base = Tt + static_cast<size_t>(c) * asize * asize;
      for (int d = 0; d < asize; d++)
        base[d * asize + row[d]]++;
    }
}

static inline const uint16_t * Tcol(int c, int d)
{
  return Tt + (static_cast<size_t>(c) * asize + d) * asize;
}

/* n from scratch: O(26^2), used to seed the incremental state and to re-sync
   in the correctness check. */
static void nvec(const unsigned char * S, int * n)
{
  for (int y = 0; y < asize; y++)
    n[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * col = Tcol(c, S[c]);
      for (int y = 0; y < asize; y++)
        n[y] += col[y];
    }
}

/* The three scores from (n, S).  These must reproduce scoring.cc EXACTLY --
   same accumulator types, same order of the final float operations. */
static double sc_ic(const int * n)
{
  int coin = 0;
  for (int j = 0; j < asize; j++)
    coin += n[j] * (n[j] - 1);
  return (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
}

static double sc_mono(const int * n, const unsigned char * S)
{
  long isum = 0;
  for (int j = 0; j < asize; j++)
    isum += static_cast<long>(n[j]) * lp8[S[j]];
  /* score_iter divides SCORE_MONO by nterms = textlength (SCORE_MONOIC is
     already per-symbol and is left alone), so the /L belongs here too. */
  return (static_cast<double>(isum) / lp_scale + textlength * lp_bias)
         / textlength;
}

static double sc_monoic(const int * n, const unsigned char * S)
{
  long isum = 0;
  int coin = 0;
  for (int j = 0; j < asize; j++)
    {
      isum += static_cast<long>(n[j]) * lp8[S[j]];
      coin += n[j] * (n[j] - 1);
    }
  const double mono = static_cast<double>(isum) / lp_scale
                      + textlength * lp_bias;
  const double ic = (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
  return mono / (textlength > 0 ? textlength : 1) + lambda * textlength * ic;
}

/* ------------------------------------------------------------- toggle ---- */

enum { CASE_ADD, CASE_REMOVE, CASE_MOVE, CASE_MERGE, NCASES };
static const char * casename[NCASES] = { "add", "remove", "move", "merge" };

/* What toggle(a,b) does to the board, without applying it: the changed
   positions and their new values.  Returns the case and fills `pos`/`val`
   with `cnt` entries. */
static inline int toggle_plan(const unsigned char * S, int a, int b,
                              int * pos, int * val, int * cnt)
{
  const int x = S[a], y = S[b];
  pos[0] = a; pos[1] = b;
  if (x == b)                                  /* already paired: remove */
    { val[0] = a; val[1] = b; *cnt = 2; return CASE_REMOVE; }
  val[0] = b; val[1] = a;
  if ((x == a) && (y == b))                    /* both free: add */
    { *cnt = 2; return CASE_ADD; }
  if (x == a)                                  /* a free, b plugged: move */
    { pos[2] = y; val[2] = y; *cnt = 3; return CASE_MOVE; }
  if (y == b)                                  /* b free, a plugged: move */
    { pos[2] = x; val[2] = x; *cnt = 3; return CASE_MOVE; }
  pos[2] = x; val[2] = x;                      /* both plugged: merge */
  pos[3] = y; val[3] = y; *cnt = 4;
  return CASE_MERGE;
}

/* n' for the planned toggle, without touching n or S.  This is the whole
   proposal: 2-4 column subtractions and the same number of additions. */
static inline void delta_n(const int * n, const unsigned char * S,
                           const int * pos, const int * val, int cnt,
                           int * out)
{
  memcpy(out, n, sizeof(int) * asize);
  for (int k = 0; k < cnt; k++)
    {
      const uint16_t * rm = Tcol(pos[k], S[pos[k]]);
      const uint16_t * ad = Tcol(pos[k], val[k]);
      for (int y = 0; y < asize; y++)
        out[y] += ad[y] - rm[y];
    }
}

/* ---------------------------------------------------------------------- */

static void rand_board(unsigned char * S, int plugs, std::mt19937_64 & rng)
{
  int l[asize];
  for (int i = 0; i < asize; i++)
    l[i] = i;
  for (int i = 0; i < 2 * plugs; i++)
    std::swap(l[i], l[i + static_cast<int>(rng() % (asize - i))]);
  for (int i = 0; i < asize; i++)
    S[i] = static_cast<unsigned char>(i);
  for (int i = 0; i < 2 * plugs; i += 2)
    {
      S[l[i]] = static_cast<unsigned char>(l[i + 1]);
      S[l[i + 1]] = static_cast<unsigned char>(l[i]);
    }
}

int main(int argc, char * * argv)
{
  const int L      = (argc > 1) ? atoi(argv[1]) : 100;
  const int TRIALS = (argc > 2) ? atoi(argv[2]) : 200;
  const int REPS   = (argc > 3) ? atoi(argv[3]) : 400;

  opt_language = "wehrmacht";
  opt_datadir  = "ngrams";
  opt_nstages  = 1;
  opt_stages[0].cap = 13;
  init();
  load_table(SCORE_MONO);
  load_table(SCORE_MONOIC);
  ngrams_read(1, lp8, & lp_bias, & lp_scale, opt_datadir, opt_language,
              "monograms");

  std::string corpus;
  {
    FILE * f = fopen("eval/corpus-hgnord.txt", "r");
    if (f == nullptr) { fprintf(stderr, "need corpus\n"); return 1; }
    int ch;
    while ((ch = fgetc(f)) != EOF)
      if ((ch >= 'A') && (ch <= 'Z')) corpus.push_back(static_cast<char>(ch));
    fclose(f);
  }

  machine * mp = new machine();
  machine & m = *mp;
  m.subst_array = static_cast<subst_table>(
    malloc(sizeof(unsigned char) * asize * asize * asize * asize));
  m.greek = -1; m.greek_offset = 0; m.report = false; m.plugboards_scored = 0;

  std::mt19937_64 rng(20260824);
  char pt[maxlen + 1];

  /* one key, fixed, for the whole run */
  const size_t off = rng() % (corpus.size() - static_cast<size_t>(L));
  for (int i = 0; i < L; i++) pt[i] = corpus[off + static_cast<size_t>(i)];
  pt[L] = 0;
  init_walzen(m, 1, 1, 2, 0);
  set_effective_reflector(m);
  precompute(m);
  init_ring_grund(m, 0, 3, 7, 4, 11, 19);
  init_steckerbrett(m, "AHBRCMDEFJNZPXQUSTVW");
  textlength = L;
  for (int i = 0; i < L; i++) num_ciphertext[i] = char2num(pt[i]);
  setup_mapping(m, true);
  decode(m);
  for (int i = 0; i < L; i++) num_ciphertext[i] = char2num(m.plaintext[i]);
  init_ring_grund(m, 0, 3, 7, 4, 11, 19);
  setup_mapping(m, true);
  build_T(m);

  /* ---- 1. BYTE-IDENTITY, over the real four-case toggle --------------- */
  long checked = 0, bad_ic = 0, bad_mono = 0, bad_k = 0;
  long casecount[NCASES] = {0, 0, 0, 0};
  unsigned char S[asize], S2[asize];
  int n[asize], n2[asize];
  for (int t = 0; t < TRIALS; t++)
    {
      rand_board(S, static_cast<int>(rng() % 11), rng);
      nvec(S, n);
      for (int rep = 0; rep < 40; rep++)
        {
          const int a = static_cast<int>(rng() % asize);
          int b = static_cast<int>(rng() % asize);
          if (b == a) continue;
          int pos[4], val[4], cnt;
          const int cs = toggle_plan(S, a, b, pos, val, & cnt);
          casecount[cs]++;
          delta_n(n, S, pos, val, cnt, n2);
          memcpy(S2, S, asize);
          for (int k = 0; k < cnt; k++)
            S2[pos[k]] = static_cast<unsigned char>(val[k]);

          /* the reference: put the toggled board on the machine and score */
          memcpy(m.steckerbrett, S2, asize);
          m.scoring = SCORE_IC;
          const double r_ic = score_iter(m);
          m.scoring = SCORE_MONO;
          const double r_mo = score_iter(m);
          m.scoring = SCORE_MONOIC;
          const double r_k = score_iter(m);

          if (sc_ic(n2) != r_ic) bad_ic++;
          if (sc_mono(n2, S2) != r_mo) bad_mono++;
          if (sc_monoic(n2, S2) != r_k) bad_k++;
          checked++;

          /* walk: accept the toggle so later boards are reachable states */
          memcpy(S, S2, asize);
          memcpy(n, n2, sizeof n);
        }
    }

  /* ---- 1b. try_repair's 2-plug re-pair, the other move in the climb ----
     Not reachable from toggle_plan: it rewires TWO existing plugs into the
     other pairing, so it changes S at four positions with the plug count
     unchanged.  It fires at every convergence (it is on unless --no-repair),
     and it already carries the delta shape -- rp_pos[4]/rp_val[4]. */
  long rep_checked = 0, rep_bad = 0;
  for (int t = 0; t < TRIALS; t++)
    {
      rand_board(S, 4 + static_cast<int>(rng() % 7), rng);
      nvec(S, n);
      int plo[asize / 2], phi[asize / 2], np = 0;
      for (int a = 0; a < asize; a++)
        if (S[a] > a) { plo[np] = a; phi[np] = S[a]; np++; }
      if (np < 2) continue;
      for (int rep = 0; rep < 20; rep++)
        {
          const int i = static_cast<int>(rng() % static_cast<unsigned>(np));
          int j = static_cast<int>(rng() % static_cast<unsigned>(np));
          if (j == i) continue;
          const int a = plo[i], x = phi[i], b = plo[j], y = phi[j];
          int pos[4], val[4];
          if (rng() & 1)          /* M1: {a-b, x-y} */
            {
              pos[0] = a; val[0] = b; pos[1] = b; val[1] = a;
              pos[2] = x; val[2] = y; pos[3] = y; val[3] = x;
            }
          else                    /* M2: {a-y, x-b} */
            {
              pos[0] = a; val[0] = y; pos[1] = y; val[1] = a;
              pos[2] = x; val[2] = b; pos[3] = b; val[3] = x;
            }
          delta_n(n, S, pos, val, 4, n2);
          memcpy(S2, S, asize);
          for (int k = 0; k < 4; k++)
            S2[pos[k]] = static_cast<unsigned char>(val[k]);
          memcpy(m.steckerbrett, S2, asize);
          m.scoring = SCORE_IC;
          const double r_ic = score_iter(m);
          m.scoring = SCORE_MONO;
          const double r_mo = score_iter(m);
          m.scoring = SCORE_MONOIC;
          const double r_k = score_iter(m);
          if ((sc_ic(n2) != r_ic) || (sc_mono(n2, S2) != r_mo)
              || (sc_monoic(n2, S2) != r_k)) rep_bad++;
          rep_checked++;
        }
    }
  printf("BYTE-IDENTITY over %ld toggles of the real four-case operator\n",
         checked);
  printf("  mismatches:  IC %ld   mono %ld   k %ld   %s\n",
         bad_ic, bad_mono, bad_k,
         (bad_ic + bad_mono + bad_k) ? "*** FAIL ***" : "(exact)");
  printf("  re-pair (try_repair M1/M2, 4 positions): %ld checked, "
         "%ld mismatches %s\n", rep_checked, rep_bad,
         rep_bad ? "*** FAIL ***" : "(exact)");
  printf("  case mix in the check: ");
  for (int c = 0; c < NCASES; c++)
    printf("%s %.0f%%  ", casename[c],
           100.0 * static_cast<double>(casecount[c])
           / static_cast<double>(checked));
  printf("\n\n");

  /* ---- 2. THE REAL CASE MIX of a cap-4 pass over 325 pairs ------------ */
  printf("CASE MIX of a full 325-pair scan, by plug count on the board\n");
  printf("  %6s %7s %8s %7s %8s   %s\n", "plugs", "add", "remove", "move",
         "merge", "mean columns touched");
  for (int plugs = 10; plugs >= 2; plugs -= 2)
    {
      long cc[NCASES] = {0, 0, 0, 0};
      for (int t = 0; t < 200; t++)
        {
          rand_board(S, plugs, rng);
          for (int a = 0; a < asize; a++)
            for (int b = a + 1; b < asize; b++)
              {
                int pos[4], val[4], cnt;
                cc[toggle_plan(S, a, b, pos, val, & cnt)]++;
              }
        }
      const double tot = static_cast<double>(cc[0] + cc[1] + cc[2] + cc[3]);
      const double cols = (4.0 * cc[CASE_ADD] + 4.0 * cc[CASE_REMOVE]
                           + 6.0 * cc[CASE_MOVE] + 8.0 * cc[CASE_MERGE]) / tot;
      printf("  %6d %6.1f%% %7.1f%% %6.1f%% %7.1f%%   %.2f\n", plugs,
             100 * cc[CASE_ADD] / tot, 100 * cc[CASE_REMOVE] / tot,
             100 * cc[CASE_MOVE] / tot, 100 * cc[CASE_MERGE] / tot, cols);
    }
  printf("\n");

  /* ---- 3. COST PER TOGGLE, T form against the shipped scorers --------- */
  typedef std::chrono::steady_clock clk;
  double sink = 0;
  printf("COST PER TOGGLE at L = %d, over a 10-plug board (the cap-4 start)\n",
         L);
  printf("  %-10s %12s %12s %8s\n", "model", "decode ns", "T-form ns", "x");

  for (int model = 0; model < 3; model++)
    {
      rand_board(S, 10, rng);
      nvec(S, n);

      /* shipped scorer: set the board and score */
      auto t0 = clk::now();
      for (int r = 0; r < REPS; r++)
        for (int a = 0; a < asize; a++)
          for (int b = a + 1; b < asize; b++)
            {
              int pos[4], val[4], cnt;
              toggle_plan(S, a, b, pos, val, & cnt);
              memcpy(S2, S, asize);
              for (int k = 0; k < cnt; k++)
                S2[pos[k]] = static_cast<unsigned char>(val[k]);
              memcpy(m.steckerbrett, S2, asize);
              m.scoring = (model == 0) ? SCORE_MONO
                        : (model == 1) ? SCORE_IC : SCORE_MONOIC;
              sink += score_iter(m);
            }
      auto t1 = clk::now();

      /* T form: plan, delta, score */
      auto t2 = clk::now();
      for (int r = 0; r < REPS; r++)
        for (int a = 0; a < asize; a++)
          for (int b = a + 1; b < asize; b++)
            {
              int pos[4], val[4], cnt;
              toggle_plan(S, a, b, pos, val, & cnt);
              delta_n(n, S, pos, val, cnt, n2);
              if (model == 1)
                sink += sc_ic(n2);
              else
                {
                  memcpy(S2, S, asize);
                  for (int k = 0; k < cnt; k++)
                    S2[pos[k]] = static_cast<unsigned char>(val[k]);
                  sink += (model == 0) ? sc_mono(n2, S2) : sc_monoic(n2, S2);
                }
            }
      auto t3 = clk::now();

      const double per = 325.0 * REPS;
      const double a_ns =
        1e9 * std::chrono::duration<double>(t1 - t0).count() / per;
      const double b_ns =
        1e9 * std::chrono::duration<double>(t3 - t2).count() / per;
      printf("  %-10s %12.1f %12.1f %8.2f\n",
             (model == 0) ? "mono" : (model == 1) ? "IC" : "k (mono+IC)",
             a_ns, b_ns, a_ns / b_ns);
    }

  {
    auto t0 = clk::now();
    for (int r = 0; r < REPS; r++) build_T(m);
    auto t1 = clk::now();
    printf("\n  build T once per work item: %.2f us "
           "(amortised over ~3200 toggles/restart)\n",
           1e6 * std::chrono::duration<double>(t1 - t0).count() / REPS);
  }

  if (sink == 12345.6789) printf(" ");
  free(m.subst_array);
  delete mp;
  return 0;
}
