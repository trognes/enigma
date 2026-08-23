/* PROTOTYPE, not part of the program.  Does a BEAM over the T representation
   produce seeds as good as the kicked-and-climbed ones it would replace?

   Arm A (what ships): W random 10-plug kicks, each climbed to cap 4 under the
   k model -- the real perturb_steckerbrett and the real hillclimb<false>.
   Arm B (the proposal): a width-W beam from the EMPTY board, adding one plug
   at a time to 4, scored on the T representation (eval/proto_tseed.cc) so no
   candidate is ever decoded.

   Both arms then hand every seed to the SAME continuation -- the repo's
   hillclimb<false> at cap 10 under -f -- and the trial is scored by letters
   recovered against the known plaintext.  So the arms differ only in how the
   seeds were produced.

   THE RISK THE MEASUREMENT IS FOR.  The beam's seeds are all good by the k
   score and may all sit in one basin, while kicked seeds are diverse by
   construction.  eval/results-rarefaction-restarts.txt showed a small kick
   collapsing to 21.9 distinct seeds of 40 and BREAKING FEWER MESSAGES despite
   a higher per-trial rate, so "better seeds, less diverse" is a real way to
   lose.  Hence this reports distinct continuations, not just breaks.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/beam eval/proto_beam.cc $objs
     /tmp/beam [keys] [kicked W] [beam W] [L] [apart]
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
#include <set>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ T ---- */

struct ttab
{
  std::vector<uint16_t> v;
  ttab() : v(static_cast<size_t>(asize) * asize * asize, 0) {}
  uint16_t * at(int c, int d)
  {
    return & v[(static_cast<size_t>(c) * asize + d) * asize];
  }
};

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

/* The k model (mono + lambda*L*IC) from the histogram and the board. */
static double k_from_n(const int * n, const unsigned char * S,
                       const uint8_t * lp, int L, double lambda)
{
  long isum = 0, coin = 0;
  for (int y = 0; y < asize; y++)
    {
      isum += static_cast<long>(n[y]) * lp[S[y]];
      coin += static_cast<long>(n[y]) * (n[y] - 1);
    }
  const double mono = static_cast<double>(isum) / ngram_scale[SCORE_MONO]
                      + L * ngram_bias[SCORE_MONO];
  const double ic = (L > 1) ? static_cast<double>(coin)
                              / (static_cast<double>(L) * (L - 1)) : 0.0;
  return mono / L + lambda * L * ic;
}

/* --------------------------------------------------------------- beam ---- */

struct cand
{
  unsigned char S[asize];
  double score;
  /* best first */
  bool operator<(const cand & o) const { return score > o.score; }
};

/* A board's identity for de-duplication: the involution as 26 bytes. */
static std::string board_key(const unsigned char * S)
{
  return std::string(reinterpret_cast<const char *>(S), asize);
}

/* Width-W beam from the EMPTY board, adding one plug per level to `plugs`.
   Every candidate is scored on T, so nothing is decoded.

   THE BOOKKEEPING IS THE HOT PART, not the scoring, and a first version got
   this wrong: a std::set<std::string> per level allocated a 26-byte string for
   each of the ~6000 candidates and made the beam cost the same as the kicked
   climbs it was meant to undercut -- an artefact of the harness that would
   have been reported as a property of the method.  The kept set is at most W,
   so the common path is ONE double compare against the worst kept, and the
   duplicate check runs only for a candidate good enough to enter. */
struct beamset
{
  std::vector<cand> keep;
  int W;
  int apart;      /* 0 = plain beam; else min differing letters kept apart */
  beamset(int w, int a) : W(w), apart(a) {}

  static int letters_apart(const cand & x, const cand & y)
  {
    int d = 0;
    for (int i = 0; i < asize; i++)
      if (x.S[i] != y.S[i])
        d++;
    return d;
  }

  void offer(const cand & c)
  {
    if (static_cast<int>(keep.size()) == W && c.score <= keep.back().score)
      return;                                   /* the common path */

    /* THE SEPARATION CHECK REPLACES, IT DOES NOT DROP.  A first version
       filtered the finished beam, which could only SHRINK the seed count and
       so could never help -- it lost breaks simply by returning fewer seeds.
       Rejecting a near neighbour here, or evicting a worse one, keeps W
       entries and makes the constraint a real alternative rather than a
       smaller budget in disguise. */
    for (size_t i = 0; i < keep.size(); i++)
      {
        const int d = letters_apart(keep[i], c);
        if ((d == 0) || ((apart > 0) && (d < apart)))
          {
            if (keep[i].score >= c.score)
              return;                           /* the neighbour is better */
            keep.erase(keep.begin() + static_cast<long>(i));
            break;                              /* evict it, insert below */
          }
      }
    keep.push_back(c);
    std::sort(keep.begin(), keep.end());
    if (static_cast<int>(keep.size()) > W)
      keep.pop_back();
  }
};

/* `min_apart`: require a kept board to differ from every other in at least
   this many plugged letters.  0 = the plain beam.  The point of the knob is
   that a beam optimises SCORE and may return W boards from one basin, which
   is the failure mode the rarefaction work documents for a small kick. */
static void beam_seeds(ttab & T, const uint8_t * lp, int L, double lambda,
                       int W, int plugs, int min_apart,
                       std::vector<cand> & out, uint64_t * evals)
{
  std::vector<cand> level(1);
  for (int i = 0; i < asize; i++)
    level[0].S[i] = static_cast<unsigned char>(i);
  int n0[asize];
  hist_from_T(T, level[0].S, n0);
  level[0].score = k_from_n(n0, level[0].S, lp, L, lambda);

  for (int step = 0; step < plugs; step++)
    {
      beamset next(W, min_apart);
      for (const cand & base : level)
        {
          int n[asize];
          hist_from_T(T, base.S, n);
          for (int a = 0; a < asize; a++)
            {
              if (base.S[a] != a)
                continue;                       /* already plugged */
              for (int b = a + 1; b < asize; b++)
                {
                  if (base.S[b] != b)
                    continue;
                  /* a and b were self-steckered, so the removed columns are
                     T[a][a] and T[b][b] */
                  const uint16_t * ma = T.at(a, a);
                  const uint16_t * mb = T.at(b, b);
                  const uint16_t * pa = T.at(a, b);
                  const uint16_t * pb = T.at(b, a);
                  int nn[asize];
                  for (int y = 0; y < asize; y++)
                    nn[y] = n[y] - ma[y] - mb[y] + pa[y] + pb[y];

                  cand c;
                  memcpy(c.S, base.S, asize);
                  c.S[a] = static_cast<unsigned char>(b);
                  c.S[b] = static_cast<unsigned char>(a);
                  c.score = k_from_n(nn, c.S, lp, L, lambda);
                  (*evals)++;
                  next.offer(c);
                }
            }
        }
      level.swap(next.keep);
    }

  out = level;
}

/* --------------------------------------------------------------------- */

static int correct_letters(const char * a, const char * b, int L)
{
  int c = 0;
  for (int i = 0; i < L; i++)
    if (a[i] == b[i])
      c++;
  return c;
}

int main(int argc, char * * argv)
{
  const int KEYS = (argc > 1) ? atoi(argv[1]) : 200;
  const int WA   = (argc > 2) ? atoi(argv[2]) : 8;    /* kicked seeds */
  const int WB   = (argc > 3) ? atoi(argv[3]) : WA;   /* beam width */
  const int L    = (argc > 4) ? atoi(argv[4]) : 100;
  const int APART = (argc > 5) ? atoi(argv[5]) : 0;   /* diversity filter */

  opt_language = "wehrmacht";
  opt_datadir = "ngrams";
  opt_hillclimb = 1;
  opt_firstimprove = 1;          /* -J, the recommended climb rule */
  opt_dynorder = 1;
  opt_nstages = 1;
  opt_scoring = SCORE_FUSED;
  opt_stages[0].model = SCORE_FUSED;
  opt_stages[0].cap = 13;

  init();
  load_table(SCORE_MONO);
  load_table(SCORE_MONOIC);
  load_table(SCORE_FUSED);
  ic_blend_init();
  static uint8_t lp8[asize];
  {
    double b, sc;
    ngrams_read(1, lp8, & b, & sc, opt_datadir, opt_language, "monograms");
  }
  const double lambda = 0.1;

  /* the corpus: authentic decrypts, as the other harnesses use */
  std::string corpus;
  {
    FILE * f = fopen("eval/corpus-tune-phase-ab.txt", "r");
    if (f == nullptr)
      { fprintf(stderr, "need eval/corpus-tune-phase-ab.txt\n"); return 1; }
    int ch;
    while ((ch = fgetc(f)) != EOF)
      if ((ch >= 'A') && (ch <= 'Z'))
        corpus.push_back(static_cast<char>(ch));
    fclose(f);
  }

  machine * mp = new machine();
  machine & m = *mp;
  m.subst_array = static_cast<subst_table>(
    malloc(sizeof(unsigned char) * asize * asize * asize * asize));
  m.greek = -1; m.greek_offset = 0; m.report = false; m.plugboards_scored = 0;

  std::mt19937_64 rng(20260823);
  char truth[maxlen + 1];

  struct arm { int breaks = 0; double pct = 0; double distinct = 0;
               uint64_t iters = 0; double secs = 0; };
  arm A, B;
  double t_seed_A = 0, t_seed_B = 0;
  int only_a = 0, only_b = 0;          /* paired discordants, for McNemar */

  for (int k = 0; k < KEYS; k++)
    {
      /* ---- a trial: excerpt, rotor key, hidden 10-pair board ---------- */
      const size_t off = rng() % (corpus.size() - L);
      for (int i = 0; i < L; i++)
        truth[i] = corpus[off + i];
      truth[L] = 0;

      int w[3];
      {
        int pool[5] = {0, 1, 2, 3, 4};
        for (int i = 0; i < 3; i++)
          {
            int j = i + static_cast<int>(rng() % (5 - i));
            std::swap(pool[i], pool[j]);
            w[i] = pool[i];
          }
      }
      int r[3], g[3];
      for (int i = 0; i < 3; i++)
        { r[i] = static_cast<int>(rng() % asize);
          g[i] = static_cast<int>(rng() % asize); }

      char board[64]; int bn = 0;
      {
        int letters[asize];
        for (int i = 0; i < asize; i++) letters[i] = i;
        for (int i = 0; i < 20; i++)
          {
            int j = i + static_cast<int>(rng() % (asize - i));
            std::swap(letters[i], letters[j]);
          }
        for (int i = 0; i < 20; i += 2)
          {
            board[bn++] = num2char(letters[i]);
            board[bn++] = num2char(letters[i + 1]);
          }
      }
      board[bn] = 0;

      init_walzen(m, 1, w[0], w[1], w[2]);
      set_effective_reflector(m);
      precompute(m);
      textlength = L;
      for (int i = 0; i < L; i++)
        num_ciphertext[i] = char2num(truth[i]);
      init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
      init_steckerbrett(m, board);
      setup_mapping(m, true);
      decode(m);
      for (int i = 0; i < L; i++)
        num_ciphertext[i] = char2num(m.plaintext[i]);

      /* the rotor key is GIVEN (plugboard tier); only the board is hidden */
      init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
      setup_mapping(m, true);

      /* ---- arm A: W kicked cap-4 climbs, the shipping method ---------- */
      std::vector<std::string> seedsA, seedsB;
      auto ta0 = std::chrono::steady_clock::now();
      for (int i = 0; i < WA; i++)
        {
          init_steckerbrett(m, "");
          uint64_t s = rng();
          perturb_steckerbrett(m, & s, 10);
          m.scoring = SCORE_MONOIC;
          hillclimb<false>(m, 4);
          seedsA.push_back(board_key(m.steckerbrett));
        }
      auto ta1 = std::chrono::steady_clock::now();
      t_seed_A += std::chrono::duration<double>(ta1 - ta0).count();

      /* ---- arm B: width-W beam on T, nothing decoded ------------------ */
      auto tb0 = std::chrono::steady_clock::now();
      ttab T;
      build_T(m, T);
      std::vector<cand> beam;
      uint64_t bevals = 0;
      beam_seeds(T, lp8, L, lambda, WB, 4, APART, beam, & bevals);
      for (const cand & c : beam)
        seedsB.push_back(board_key(c.S));
      auto tb1 = std::chrono::steady_clock::now();
      t_seed_B += std::chrono::duration<double>(tb1 - tb0).count();

      /* ---- both arms: the SAME f10 continuation ----------------------- */
      bool broke[2] = { false, false };
      for (int a = 0; a < 2; a++)
        {
          const std::vector<std::string> & seeds = a ? seedsB : seedsA;
          arm & acc = a ? B : A;
          int best = 0;
          std::set<std::string> finals;
          const uint64_t it0 = m.plugboards_scored;
          auto tc0 = std::chrono::steady_clock::now();
          for (const std::string & sd : seeds)
            {
              memcpy(m.steckerbrett, sd.data(), asize);
              m.scoring = SCORE_FUSED;
              hillclimb<false>(m, 10);
              decode(m);
              finals.insert(board_key(m.steckerbrett));
              const int c = correct_letters(m.plaintext, truth, L);
              if (c > best)
                best = c;
            }
          auto tc1 = std::chrono::steady_clock::now();
          acc.secs += std::chrono::duration<double>(tc1 - tc0).count();
          acc.iters += m.plugboards_scored - it0;
          acc.pct += 100.0 * best / L;
          acc.distinct += static_cast<double>(finals.size());
          if (best * 2 >= L)
            { acc.breaks++; broke[a] = true; }
        }
      if (broke[0] && ! broke[1]) only_a++;
      if (broke[1] && ! broke[0]) only_b++;
    }

  printf("beam vs kicked seeds: %d keys, %d kicked / %d beam, L = %d%s\n",
         KEYS, WA, WB, L,
         (APART > 0) ? ", diversity filter on" : "");
  printf("telegraphic German, rotor key given, 10-pair board hidden,\n");
  printf("seeds -> the SAME hillclimb<false> at cap 10 under -f\n\n");
  printf("  %-22s %8s %8s %10s %10s %9s\n",
         "arm", "breaks", "mean%", "distinct", "seed ms", "climb ms");
  const double kd = KEYS;
  printf("  %-22s %6d   %7.1f %9.2f %10.2f %9.2f\n", "A  kicked + cap-4 climb",
         A.breaks, A.pct / kd, A.distinct / kd, 1e3 * t_seed_A / kd,
         1e3 * A.secs / kd);
  printf("  %-22s %6d   %7.1f %9.2f %10.2f %9.2f\n", "B  beam on T",
         B.breaks, B.pct / kd, B.distinct / kd, 1e3 * t_seed_B / kd,
         1e3 * B.secs / kd);
  printf("\n  seed generation %.1fx cheaper in B; total ms/key %.2f vs %.2f"
         " (%.2fx)\n",
         t_seed_A / t_seed_B,
         1e3 * (t_seed_A + A.secs) / kd, 1e3 * (t_seed_B + B.secs) / kd,
         (t_seed_A + A.secs) / (t_seed_B + B.secs));
  printf("  paired: %d keys broken only by A, %d only by B\n", only_a, only_b);
  free(m.subst_array);
  delete mp;
  return 0;
}
