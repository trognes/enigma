/* PROTOTYPE, not part of the program.  Does a WEIGHTED kick beat the uniform
   one?

   The kick that starts every restart draws its plug pairs uniformly.  But the
   325 single-plug boards can all be scored under IC for ~15 us per key on the
   T representation (eval/results-tseed-proto.txt), and a TRUE plug is enriched
   in the top of that ranking -- 12.1% of true plugs land in the top 10 against
   3.1% by chance at L=100.  So the kick can draw from a distribution biased
   toward the plugs that score well, instead of uniformly.

     arm A: perturb_steckerbrett(), the shipped uniform kick.
     arm B: the same number of pairs, drawn with probability proportional to
            exp(z / T) over the 325 single-plug IC z-scores.

   Everything downstream is identical -- the same cap climb under k produces
   the seed, the same hillclimb<false> at cap 10 under -f is the continuation,
   the same keys in the same order.  T -> infinity reproduces arm A exactly,
   which is the control worth running.

   WHY THIS AND NOT THE BEAM.  eval/results-beam-seeds.txt measured a beam over
   the same scores and it lost at every budget past ~8 seeds, because its W
   leaves hang off shared ancestors and converge to one region (apartness 6.1
   against the kick's 17.4).  A weighted kick has NO ancestry: every seed is an
   independent draw, so the bias is a continuous knob from uniform to greedy
   and diversity degrades gracefully rather than collapsing.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/wkick eval/proto_wkick.cc $objs
     /tmp/wkick [keys] [restarts] [L] [temperature] [kick] [-M] [cap] [seed]

   cap = 0 SKIPS the cap climb, handing the kicked board straight to the
   continuation.  That is not a realistic configuration -- it is the ablation
   that says how much of the bias the cap climb erases before the continuation
   ever sees it. */

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

/* ------------------------------------------------------- weighted kick ---- */

struct plugtab
{
  int a[325], b[325];
  double w[325];        /* exp(z / T) */
  double cum[325];      /* prefix sums, for the rejection draw */
  int n;
};

/* Score all 325 single-plug boards under IC by the O(26) delta from the empty
   board, then turn the z-scores into softmax weights.  A z-score rather than
   the raw IC because IC's absolute level wanders by key while its spread is
   what carries the signal -- so one temperature means the same thing on every
   key.  T <= 0 means uniform. */
static void build_plugtab(ttab & T, int L, double temp, plugtab & P)
{
  int n0[asize];
  for (int y = 0; y < asize; y++)
    n0[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * col = T.at(c, c);
      for (int y = 0; y < asize; y++)
        n0[y] += col[y];
    }

  P.n = 0;
  double sc[325];
  for (int a = 0; a < asize; a++)
    {
      for (int b = a + 1; b < asize; b++)
        {
          const uint16_t * ma = T.at(a, a);
          const uint16_t * mb = T.at(b, b);
          const uint16_t * pa = T.at(a, b);
          const uint16_t * pb = T.at(b, a);
          long c2 = 0;
          for (int y = 0; y < asize; y++)
            {
              const int nn = n0[y] - ma[y] - mb[y] + pa[y] + pb[y];
              c2 += static_cast<long>(nn) * (nn - 1);
            }
          P.a[P.n] = a;
          P.b[P.n] = b;
          sc[P.n] = static_cast<double>(c2)
                      / (static_cast<double>(L) * (L - 1));
          P.n++;
        }
    }

  if (temp <= 0)
    {
      for (int i = 0; i < P.n; i++)
        P.w[i] = 1.0;
    }
  else
    {
      double mu = 0;
      for (int i = 0; i < P.n; i++)
        mu += sc[i];
      mu /= P.n;
      double vr = 0;
      for (int i = 0; i < P.n; i++)
        vr += (sc[i] - mu) * (sc[i] - mu);
      const double sd = sqrt(vr / (P.n - 1));
      for (int i = 0; i < P.n; i++)
        P.w[i] = (sd > 0) ? exp(((sc[i] - mu) / sd) / temp) : 1.0;
    }

  double run = 0;
  for (int i = 0; i < P.n; i++)
    {
      run += P.w[i];
      P.cum[i] = run;
    }
}

static double rng01(uint64_t & s)
{
  s = s * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<double>((s >> 11) & 0x1FFFFFFFFFFFFFULL)
           / 9007199254740992.0;
}

/* Draw `k` disjoint pairs.  REJECTION FIRST, then an exact renormalised pass
   as the fallback -- the two are the same distribution (redrawing on a
   conflict IS drawing from the legal set renormalised), so this is a pure
   cost choice.  Rejection is ~6 draws for 4 plugs at T >= 1 and blows up when
   the mass concentrates or the board fills, at which point the linear pass is
   both bounded and exact.  Without the fallback a cold temperature silently
   returns FEWER than k pairs, which would change what is being measured
   rather than how fast it runs. */
static void weighted_kick(machine & m, const plugtab & P, int k, uint64_t & s)
{
  bool used[asize];
  for (int i = 0; i < asize; i++)
    used[i] = false;

  for (int got = 0; got < k; got++)
    {
      int pick = -1;
      for (int tries = 0; tries < 32; tries++)
        {
          const double x = rng01(s) * P.cum[P.n - 1];
          const int i = static_cast<int>(
            std::lower_bound(P.cum, P.cum + P.n, x) - P.cum);
          if (i >= P.n)
            continue;
          if (used[P.a[i]] || used[P.b[i]])
            continue;
          pick = i;
          break;
        }
      if (pick < 0)
        {
          /* exact fallback over the still-legal pairs */
          double tot = 0;
          for (int i = 0; i < P.n; i++)
            {
              if (! used[P.a[i]] && ! used[P.b[i]])
                tot += P.w[i];
            }
          if (tot <= 0)
            return;                     /* no legal pair left */
          double x = rng01(s) * tot;
          for (int i = 0; i < P.n; i++)
            {
              if (used[P.a[i]] || used[P.b[i]])
                continue;
              x -= P.w[i];
              if (x <= 0)
                { pick = i; break; }
            }
          if (pick < 0)
            return;
        }
      used[P.a[pick]] = used[P.b[pick]] = true;
      m.steckerbrett[P.a[pick]] = static_cast<unsigned char>(P.b[pick]);
      m.steckerbrett[P.b[pick]] = static_cast<unsigned char>(P.a[pick]);
    }
}

/* --------------------------------------------------------------------- */

static std::string board_key(const unsigned char * S)
{
  return std::string(reinterpret_cast<const char *>(S), asize);
}

static int correct_letters(const char * a, const char * b, int L)
{
  int c = 0;
  for (int i = 0; i < L; i++)
    {
      if (a[i] == b[i])
        c++;
    }
  return c;
}

int main(int argc, char * * argv)
{
  const int KEYS  = (argc > 1) ? atoi(argv[1]) : 200;
  const int R     = (argc > 2) ? atoi(argv[2]) : 8;
  const int L     = (argc > 3) ? atoi(argv[3]) : 100;
  const double TEMP = (argc > 4) ? atof(argv[4]) : 1.0;
  const int KICK  = (argc > 5) ? atoi(argv[5]) : 10;
  const int CAPM  = (argc > 6) ? atoi(argv[6]) : 0;
  const int CAP   = (argc > 7) ? atoi(argv[7]) : 4;
  /* An INDEPENDENT SEED, because a positive cell found by sweeping is a
     hypothesis and not yet a result -- this harness has been run over enough
     cells that one z = 2 among them is unremarkable. */
  const uint64_t SEED = (argc > 8)
                          ? strtoull(argv[8], nullptr, 10) : 20260824ULL;

  opt_language = "wehrmacht";
  opt_datadir = "ngrams";
  opt_hillclimb = 1;
  opt_firstimprove = 1;
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

  std::string corpus;
  {
    FILE * f = fopen("eval/corpus-hgnord.txt", "r");
    if (f == nullptr)
      { fprintf(stderr, "need eval/corpus-hgnord.txt\n"); return 1; }
    int ch;
    while ((ch = fgetc(f)) != EOF)
      {
        if ((ch >= 'A') && (ch <= 'Z'))
          corpus.push_back(static_cast<char>(ch));
      }
    fclose(f);
  }

  machine * mp = new machine();
  machine & m = *mp;
  m.subst_array = static_cast<subst_table>(
    malloc(sizeof(unsigned char) * asize * asize * asize * asize));
  m.greek = -1; m.greek_offset = 0; m.report = false; m.plugboards_scored = 0;

  std::mt19937_64 rng(SEED);
  char truth[maxlen + 1];

  struct arm { int breaks = 0; double pct = 0; double distinct = 0;
               double apartness = 0; int apairs = 0; double truep = 0;
               double secs = 0; double seedsecs = 0; };
  arm A, B;
  int only_a = 0, only_b = 0;

  for (int k = 0; k < KEYS; k++)
    {
      const size_t off = rng() % (corpus.size() - static_cast<size_t>(L));
      for (int i = 0; i < L; i++)
        truth[i] = corpus[off + static_cast<size_t>(i)];
      truth[L] = 0;

      int w[3];
      {
        int pool[5] = {0, 1, 2, 3, 4};
        for (int i = 0; i < 3; i++)
          {
            const int j = i + static_cast<int>(rng() % (5 - i));
            std::swap(pool[i], pool[j]);
            w[i] = pool[i];
          }
      }
      int r[3], g[3];
      for (int i = 0; i < 3; i++)
        {
          r[i] = static_cast<int>(rng() % asize);
          g[i] = static_cast<int>(rng() % asize);
        }

      int tl[asize];
      for (int i = 0; i < asize; i++)
        tl[i] = i;
      for (int i = 0; i < 20; i++)
        {
          const int j = i + static_cast<int>(rng() % (asize - i));
          std::swap(tl[i], tl[j]);
        }
      char board[64];
      int bn = 0;
      for (int i = 0; i < 20; i += 2)
        {
          board[bn++] = num2char(tl[i]);
          board[bn++] = num2char(tl[i + 1]);
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
      unsigned char tb[asize];
      memcpy(tb, m.steckerbrett, asize);

      init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
      setup_mapping(m, true);

      /* ---- arm A: the shipped uniform kick ---------------------------- */
      std::vector<std::string> seedsA, seedsB;
      auto ta0 = std::chrono::steady_clock::now();
      for (int i = 0; i < R; i++)
        {
          init_steckerbrett(m, "");
          uint64_t s = rng();
          perturb_steckerbrett(m, & s, KICK);
          if (CAP > 0)
            {
              m.scoring = SCORE_MONOIC;
              opt_capmerge = CAPM;
              hillclimb<false>(m, CAP);
              opt_capmerge = 0;
            }
          seedsA.push_back(board_key(m.steckerbrett));
        }
      auto ta1 = std::chrono::steady_clock::now();
      A.seedsecs += std::chrono::duration<double>(ta1 - ta0).count();

      /* ---- arm B: the weighted kick ----------------------------------- */
      auto tb0 = std::chrono::steady_clock::now();
      ttab T;
      build_T(m, T);
      plugtab P;
      build_plugtab(T, L, TEMP, P);
      for (int i = 0; i < R; i++)
        {
          init_steckerbrett(m, "");
          uint64_t s = rng();
          weighted_kick(m, P, KICK, s);
          if (CAP > 0)
            {
              m.scoring = SCORE_MONOIC;
              opt_capmerge = CAPM;
              hillclimb<false>(m, CAP);
              opt_capmerge = 0;
            }
          seedsB.push_back(board_key(m.steckerbrett));
        }
      auto tb1 = std::chrono::steady_clock::now();
      B.seedsecs += std::chrono::duration<double>(tb1 - tb0).count();

      /* ---- both arms: the SAME f10 continuation ----------------------- */
      bool broke[2] = { false, false };
      for (int a = 0; a < 2; a++)
        {
          const std::vector<std::string> & seeds = a ? seedsB : seedsA;
          arm & acc = a ? B : A;

          /* how many TRUE plugs the seeds hold, and how far apart they are */
          {
            double tp = 0, sum = 0;
            long np = 0;
            for (const std::string & sd : seeds)
              {
                for (int q = 0; q < asize; q++)
                  {
                    if ((static_cast<unsigned char>(sd[q]) > q)
                        && (tb[q] == static_cast<unsigned char>(sd[q])))
                      tp += 1.0;
                  }
              }
            acc.truep += tp / static_cast<double>(seeds.size());
            for (size_t i = 0; i < seeds.size(); i++)
              {
                for (size_t j = i + 1; j < seeds.size(); j++)
                  {
                    int d = 0;
                    for (int q = 0; q < asize; q++)
                      {
                        if (seeds[i][q] != seeds[j][q])
                          d++;
                      }
                    sum += d;
                    np++;
                  }
              }
            if (np > 0)
              { acc.apartness += sum / static_cast<double>(np); acc.apairs++; }
          }

          int best = 0;
          std::set<std::string> finals;
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
          acc.pct += 100.0 * best / L;
          acc.distinct += static_cast<double>(finals.size());
          if (best * 2 >= L)
            { acc.breaks++; broke[a] = true; }
        }
      if (broke[0] && ! broke[1]) only_a++;
      if (broke[1] && ! broke[0]) only_b++;
    }

  const double kd = KEYS;
  printf("weighted kick: %d keys, R = %d, L = %d, T = %.2f, kick %d, "
         "-M %s, cap %d\n", KEYS, R, L, TEMP, KICK, CAPM ? "on" : "off", CAP);
  printf("telegraphic German (HG Nord), rotor key given, 10-pair board "
         "hidden\n\n");
  printf("  %-20s %7s %7s %8s %7s %7s %8s %8s\n", "arm", "breaks", "mean%",
         "trueplg", "apart", "finals", "seed ms", "climb ms");
  printf("  %-20s %6d  %6.1f %8.2f %7.2f %7.2f %8.2f %8.2f\n",
         "A  uniform kick", A.breaks, A.pct / kd, A.truep / kd,
         (A.apairs ? A.apartness / A.apairs : 0.0), A.distinct / kd,
         1e3 * A.seedsecs / kd, 1e3 * A.secs / kd);
  printf("  %-20s %6d  %6.1f %8.2f %7.2f %7.2f %8.2f %8.2f\n",
         "B  weighted kick", B.breaks, B.pct / kd, B.truep / kd,
         (B.apairs ? B.apartness / B.apairs : 0.0), B.distinct / kd,
         1e3 * B.seedsecs / kd, 1e3 * B.secs / kd);
  const double za = only_a + only_b;
  printf("\n  paired: %d only A, %d only B", only_a, only_b);
  if (za > 0)
    printf("   z = %+.2f", (only_b - only_a) / sqrt(za));
  printf("\n  total ms/key: A %.2f  B %.2f  (%.3fx)\n",
         1e3 * (A.seedsecs + A.secs) / kd, 1e3 * (B.seedsecs + B.secs) / kd,
         (B.seedsecs + B.secs) / (A.seedsecs + A.secs));

  free(m.subst_array);
  delete mp;
  return 0;
}
