/* PROTOTYPE, not part of the program.  Builds against the repo's own objects
   so the rotor core, the n-gram tables and the reference scorers are the real
   ones -- correctness is then exact by construction rather than by a
   re-implementation that might differ.

   THE IDEA.  decode_at is steck[rows[i][steck[ct[i]]]].  Write q_i for the
   decrypt BEFORE the exit plugboard, q_i = core_i[S[c_i]].  Then

     IC   is blind to the exit board: it is a function of the MULTISET of
          letter counts, and the exit board is a bijection over the whole
          sequence, so it permutes the histogram and leaves that multiset
          alone.
     mono is not blind, but needs no decode either: the exit board relabels
          the COEFFICIENTS.  mono = sum_z n_z(q) * logp[S[z]].

   Both are therefore functions of one 26-vector,

     n(q) = sum_c T[c][S[c]],  T[c][d][y] = #{i : c_i == c, core_i[d] == y}

   which is built once per key in L*26 increments.  After that every score
   costs O(26^2), every single-plug toggle O(26), and the message LENGTH has
   dropped out.  This measures whether that is true in practice, and what it
   costs for mono, IC and the k blend.

   Build (from the repo root, after `make`):

     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/proto eval/proto_tseed.cc \
         $(ls src/*.o | grep -v main.o)
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
#include <chrono>
#include <random>
#include <vector>

/* ------------------------------------------------------------------ T ---- */

/* T[c][d][y], flattened.  uint16 is enough: an entry counts positions carrying
   one ciphertext letter, so it is bounded by that letter's frequency. */
struct ttab
{
  std::vector<uint16_t> v;
  ttab() : v(static_cast<size_t>(asize) * asize * asize, 0) {}
  uint16_t * at(int c, int d)
  {
    return & v[(static_cast<size_t>(c) * asize + d) * asize];
  }
};

/* One pass over the message per ciphertext letter value.  L*26 increments. */
static void build_T(machine & m, ttab & T)
{
  std::fill(T.v.begin(), T.v.end(), static_cast<uint16_t>(0));
  for (int i = 0; i < textlength; i++)
    {
      const int c = num_ciphertext[i];
      const unsigned char * row = m.rows[i];
      uint16_t * base = T.at(c, 0);
      for (int d = 0; d < asize; d++)
        base[d * asize + row[d]]++;
    }
}

/* n(q) from scratch: 26 vector adds. */
static void hist_from_T(ttab & T, const unsigned char * S, int * n)
{
  for (int y = 0; y < asize; y++)
    n[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * col = T.at(c, S[c]);
      for (int y = 0; y < asize; y++)
        n[y] += col[y];
    }
}

/* The three scores, from n and (for mono) the board. */
static long ic_coin(const int * n)
{
  long s = 0;
  for (int y = 0; y < asize; y++)
    s += static_cast<long>(n[y]) * (n[y] - 1);
  return s;
}

static long mono_sum(const int * n, const unsigned char * S,
                     const uint8_t * lp)
{
  long s = 0;
  for (int z = 0; z < asize; z++)
    s += static_cast<long>(n[z]) * lp[S[z]];
  return s;
}

/* ------------------------------------------------------- reference check -- */

/* The repo's own scorers, reached through score_iter, need m.scoring set and
   the board installed.  Used only to verify the T forms; never timed as the
   T path. */
static double ref_score(machine & m, int model)
{
  m.scoring = model;
  return score_iter(m);
}

int main(int argc, char * * argv)
{
  const int L = (argc > 1) ? atoi(argv[1]) : 130;
  const int TRIALS = (argc > 2) ? atoi(argv[2]) : 200000;

  /* --- minimal option state, as parse_args would leave it ---------------- */
  opt_language = "english";
  opt_datadir = "ngrams";
  opt_scoring = SCORE_IC;
  opt_hillclimb = 1;
  opt_nstages = 1;
  opt_stages[0].model = SCORE_IC;
  opt_stages[0].cap = 13;

  /* a plaintext of the requested length, enciphered under a known key */
  static char pt[maxlen + 1];
  {
    FILE * f = fopen("eval/corpus-tune-phase-ab.txt", "r");
    if (f == nullptr)
      { fprintf(stderr, "need eval/corpus-tune-phase-ab.txt\n"); return 1; }
    int n = 0, ch;
    while ((n < L) && ((ch = fgetc(f)) != EOF))
      if ((ch >= 'A') && (ch <= 'Z'))
        pt[n++] = static_cast<char>(ch);
    fclose(f);
    pt[n] = 0;
    if (n < L)
      { fprintf(stderr, "corpus too short\n"); return 1; }
  }

  init();
  load_table(SCORE_MONO);      /* fills the repo's own mono8, for reference */
  load_table(SCORE_MONOIC);
  ic_blend_init();

  /* The prototype's OWN copy of the monogram table, loaded through the same
     ngrams_read with the same arguments, so the quantisation is identical --
     mono8 itself is private to scoring.cc and stays that way. */
  static uint8_t lp8[asize];
  {
    double b, sc;
    ngrams_read(1, lp8, & b, & sc, opt_datadir, opt_language, "monograms");
    if ((b != ngram_bias[SCORE_MONO]) || (sc != ngram_scale[SCORE_MONO]))
      { fprintf(stderr, "table affine differs from the repo's\n"); return 1; }
  }
  /* -S k's lambda: 0.1 * L, the documented default (ENIGMA_MONOIC_BLEND is
     internal to scoring.cc, so the prototype must not be run with it set). */
  if (getenv("ENIGMA_MONOIC_BLEND") != nullptr)
    {
      fprintf(stderr, "unset ENIGMA_MONOIC_BLEND for the prototype\n");
      return 1;
    }
  const double lambda = 0.1;

  machine * mp = new machine();
  machine & m = *mp;
  m.subst_array = static_cast<subst_table>(
    malloc(sizeof(unsigned char) * asize * asize * asize * asize));
  m.greek = -1;
  m.greek_offset = 0;
  m.report = false;
  m.plugboards_scored = 0;

  init_walzen(m, 1, 2, 3, 1);            /* reflector B, wheels 231 */
  set_effective_reflector(m);
  precompute(m);

  /* encipher: Enigma is reciprocal, so decoding the plaintext gives the
     ciphertext under the same settings and board */
  {
    for (int i = 0; i < L; i++)
      num_ciphertext[i] = char2num(pt[i]);
    textlength = L;
    init_ring_grund(m, 0, 0, 2, 16, 22, 4);        /* ring AAC, start QWE */
    static const char * seedboard = "ABCDEFGHIJKLMNOPQRST";
    init_steckerbrett(m, seedboard);
    setup_mapping(m, true);
    decode(m);
    for (int i = 0; i < L; i++)
      num_ciphertext[i] = char2num(m.plaintext[i]);
  }
  init_ring_grund(m, 0, 0, 2, 16, 22, 4);
  setup_mapping(m, true);

  ttab T;
  {
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 1000; rep++)
      build_T(m, T);
    auto t1 = std::chrono::steady_clock::now();
    printf("L = %d, %d toggle evaluations per arm\n\n", L, TRIALS);
    printf("build T once per key: %.1f us\n\n",
           1e6 * std::chrono::duration<double>(t1 - t0).count() / 1000);
  }

  const uint8_t * lp = lp8;

  /* ------------------------------------------------ correctness first ---- */
  std::mt19937_64 rng(12345);
  double worst_ic = 0, worst_mono = 0, worst_k = 0;
  for (int t = 0; t < 400; t++)
    {
      unsigned char S[asize];
      for (int i = 0; i < asize; i++)
        S[i] = static_cast<unsigned char>(i);
      int npairs = static_cast<int>(rng() % 14);
      std::vector<int> free_;
      for (int i = 0; i < asize; i++)
        free_.push_back(i);
      for (int p = 0; p < npairs; p++)
        {
          int i = static_cast<int>(rng() % free_.size());
          int a = free_[i]; free_.erase(free_.begin() + i);
          int j = static_cast<int>(rng() % free_.size());
          int b = free_[j]; free_.erase(free_.begin() + j);
          S[a] = static_cast<unsigned char>(b);
          S[b] = static_cast<unsigned char>(a);
        }
      memcpy(m.steckerbrett, S, asize);

      int n[asize];
      hist_from_T(T, S, n);

      const double ic_T = static_cast<double>(ic_coin(n))
                          / (static_cast<double>(L) * (L - 1));
      const double mono_T = static_cast<double>(mono_sum(n, S, lp))
                            / ngram_scale[SCORE_MONO]
                            + L * ngram_bias[SCORE_MONO];

      const double ic_ref = ref_score(m, SCORE_IC);
      /* the scorer reports per symbol; T's mono_sum is the whole message */
      const double mono_ref = ref_score(m, SCORE_MONO) * L;
      /* monoic normalises mono PER SYMBOL and then adds lambda * L * IC */
      const double k_ref = ref_score(m, SCORE_MONOIC);
      const double k_T = mono_T / L + lambda * L * ic_T;

      if (fabs(ic_T - ic_ref) > worst_ic)     worst_ic = fabs(ic_T - ic_ref);
      if (fabs(mono_T - mono_ref) > worst_mono)
        worst_mono = fabs(mono_T - mono_ref);
      if (fabs(k_T - k_ref) > worst_k)        worst_k = fabs(k_T - k_ref);
    }
  printf("correctness against the repo's own scorers, 400 random boards:\n");
  printf("  IC    max |T - reference|  %.3e\n", worst_ic);
  printf("  mono  max |T - reference|  %.3e\n", worst_mono);
  printf("  k     max |T - reference|  %.3e\n\n", worst_k);

  /* ---------------------------------------------------------- timing ----- */
  /* Toggle a random pair, score, toggle back -- the climb's inner operation.
     Arm A rescores through the repo's scorer (O(L)); arm B updates n
     incrementally from T (O(26)) and rescores from n. */
  struct pair { unsigned char a, b; };
  std::vector<pair> moves(TRIALS);
  for (int i = 0; i < TRIALS; i++)
    {
      int a = static_cast<int>(rng() % asize);
      int b = static_cast<int>(rng() % (asize - 1));
      if (b >= a) b++;
      moves[i].a = static_cast<unsigned char>(a);
      moves[i].b = static_cast<unsigned char>(b);
    }

  const char * names[3] = { "mono", "IC  ", "k   " };
  const int models[3] = { SCORE_MONO, SCORE_IC, SCORE_MONOIC };
  double ref_ns[3], t_ns[3];
  volatile double sink = 0;

  for (int mi = 0; mi < 3; mi++)
    {
      unsigned char S[asize];
      for (int i = 0; i < asize; i++) S[i] = static_cast<unsigned char>(i);
      memcpy(m.steckerbrett, S, asize);
      m.scoring = models[mi];

      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < TRIALS; i++)
        {
          const int a = moves[i].a, b = moves[i].b;
          const unsigned char sa = m.steckerbrett[a], sb = m.steckerbrett[b];
          m.steckerbrett[a] = static_cast<unsigned char>(b);
          m.steckerbrett[b] = static_cast<unsigned char>(a);
          sink += score_iter(m);
          m.steckerbrett[a] = sa;
          m.steckerbrett[b] = sb;
        }
      auto t1 = std::chrono::steady_clock::now();
      ref_ns[mi] =
        1e9 * std::chrono::duration<double>(t1 - t0).count() / TRIALS;
    }

  for (int mi = 0; mi < 3; mi++)
    {
      unsigned char S[asize];
      for (int i = 0; i < asize; i++) S[i] = static_cast<unsigned char>(i);
      int n[asize];
      hist_from_T(T, S, n);

      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < TRIALS; i++)
        {
          const int a = moves[i].a, b = moves[i].b;
          const int sa = S[a], sb = S[b];
          /* delta = -T[a][sa] -T[b][sb] +T[a][b] +T[b][a] */
          const uint16_t * ma = T.at(a, sa);
          const uint16_t * mb = T.at(b, sb);
          const uint16_t * pa = T.at(a, b);
          const uint16_t * pb = T.at(b, a);
          int nn[asize];
          for (int y = 0; y < asize; y++)
            nn[y] = n[y] - ma[y] - mb[y] + pa[y] + pb[y];

          double v;
          if (models[mi] == SCORE_IC)
            v = static_cast<double>(ic_coin(nn));
          else
            {
              S[a] = static_cast<unsigned char>(b);
              S[b] = static_cast<unsigned char>(a);
              v = static_cast<double>(mono_sum(nn, S, lp));
              if (models[mi] == SCORE_MONOIC)
                v += lambda * static_cast<double>(ic_coin(nn)) / (L - 1);
              S[a] = static_cast<unsigned char>(sa);
              S[b] = static_cast<unsigned char>(sb);
            }
          sink += v;
        }
      auto t1 = std::chrono::steady_clock::now();
      t_ns[mi] = 1e9 * std::chrono::duration<double>(t1 - t0).count() / TRIALS;
    }

  printf("cost of ONE plug toggle, ns per evaluation:\n");
  printf("  %-6s %12s %12s %10s\n", "model", "current", "from T", "speedup");
  for (int mi = 0; mi < 3; mi++)
    printf("  %-6s %11.1f %12.1f %9.2fx\n",
           names[mi], ref_ns[mi], t_ns[mi], ref_ns[mi] / t_ns[mi]);
  if (sink == 12345.0) printf(" ");   /* keep the loops */
  free(m.subst_array);
  delete mp;
  return 0;
}
