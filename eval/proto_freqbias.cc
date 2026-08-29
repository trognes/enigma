/* PROTOTYPE, not part of the program.  Is the 325-plug IC ranking biased by
   the CIPHERTEXT FREQUENCY of the plugged letters, and does correcting for it
   rank true plugs better?

   THE STRUCTURAL CLAIM.  IC reads only the histogram of the decrypt taken
   before the exit board, and cooc_col(c, d) accumulates only over positions
   where ct_i == c -- so that column's total mass IS count(c).  Plugging (a,b)
   on an empty board perturbs the histogram by -ma -mb +qa +qb, whose mass is
   exactly count(a) + count(b).  Writing n = n0 + delta,

       sum n^2 = sum n0^2 + 2<n0, delta> + ||delta||^2

   the cross term averages to ~0 (delta sums to zero, n0 is near-uniform) but
   ||delta||^2 is strictly POSITIVE and proportional to count(a) + count(b).
   So IC should rise with the frequency of the plugged letters whether or not
   the plug is correct, while true plugs are drawn uniformly over letters and
   are indifferent to it.  That is a first-order ranking bias, not merely a
   resolution limit, and --biased-random's per-KEY z-scoring cannot remove it
   because it is per-PLUG.

   WHAT IS MEASURED.  Over many keys: the correlation of the raw IC score with
   m = count(a) + count(b); the enrichment of true plugs under the raw
   ranking; and the same enrichment after two corrections --

     resid   score - (alpha + beta*m)         removes the systematic offset
     stud    resid / sqrt(m)                  also equalises the noise scale
                                              (the cross term is a random-sign
                                              sum over ~m bins, so its spread
                                              grows as sqrt(m))

   If the raw ranking is already the best of the three, the bias is real but
   not exploitable and the idea stops here.

   MEASURED, 400 keys per length, and it stops here.

   The SYSTEMATIC OFFSET DOES NOT EXIST: corr(score, m) reads -0.025 to +0.012
   across L = 40/60/100/167/300, i.e. zero.  The reasoning above is wrong at
   the step where it treats delta as independent of n0, and the reason is worth
   keeping: n0 = sum_c cooc_col(c, c), so ma IS a component of n0, giving
   <n0, ma> >= ||ma||^2 -- and -2<n0, ma> cancels the +||ma||^2 that the
   argument expected to survive in ||delta||^2.

   The FREQUENCY EFFECT IS REAL IN THE RANKING, and largest where the signal is
   scarcest.  Mean rank of a true plug (chance 163.0), split at the key's own
   median count sum:

     L      all   frequent   rare    gap as % of enrichment
      40  141.8      136.2  150.6                       68%
      60  132.7      127.3  140.7                       44%
     100  111.7      103.0  122.4                       38%
     167   89.2       79.4  101.0                       29%
     300   55.6       49.3   63.3                       13%

   At forty letters nearly the whole of IC's single-plug signal is carried by
   the frequent half.

   A FREQUENT-LETTER PLUG DOES NOT IMPROVE THE IC, IT ONLY MOVES IT FURTHER --
   the quintile table answers that directly.  At L = 100, 800 keys, scores
   centred per key and shown x1000:

     quintile   mean m   wrong mu   wrong sd   true mu
            1      4.2      +0.28      11.68     +5.43
            3      7.5      -0.36      15.01     +9.13
            5     11.5      -0.83      18.14    +13.58

   The wrong-plug mean is flat and if anything slightly NEGATIVE (-0.046 sd at
   quintile 5), consistently signed and monotone at L = 60/100/167 alike, so
   there is no upward drift to remove.  The sd grows as sqrt(m) -- 1.55x for a
   2.74x range in m, against sqrt(2.74) = 1.66 -- which is the random-sign sum
   over ~m bins.  The true-plug mean grows as m itself, 2.5x over the same
   range.  So SNR grows as sqrt(m): a correct plug on frequent letters is
   genuinely more detectable, by about the square root of the frequency ratio.

   THAT is why studentising is a wash rather than a small win.  Dividing by
   sqrt(m) equalises the noise, which stops rare-letter true plugs competing
   against high-variance frequent-letter decoys, but it also strips
   frequent-letter true plugs of an advantage they have actually earned.  The
   two effects cancel: it redistributes rank between the halves and leaves the
   total where it was.

   BUT CORRECTING IT BUYS NOTHING: raw / resid / stud land within a point of
   each other at every length (111.7 / 111.3 / 111.9 at L = 100), because the
   SIGNAL scales with m as well as the noise -- a plug on frequent letters
   genuinely has more evidence behind it.  The frequency weighting is the
   correct weighting, not a miscalibration, and studentising discards real
   information.  Nothing here should be applied to --biased-random's kick
   weights or to the crib seeders' IC ranking.

   WHAT SURVIVES is the mechanism, for the seed-diversity question: if IC can
   resolve plugs on frequent letters and essentially cannot on rare ones, a
   cap-4 pre-pass keeps placing its four plugs on the same frequent letters --
   which would explain why ~10^14 distinct kicks collapse into the ~6*10^4
   seed pool of eval/results-seed-richness.txt.  That is a separate claim and
   this probe does not test it.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/fb eval/proto_freqbias.cc $objs
     /tmp/fb [L] [keys]
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
#include <random>
#include <string>
#include <vector>

static const int NPAIRS = asize * (asize - 1) / 2;

struct rankstats
{
  double sum_rank = 0;
  long   n = 0;
  long   t10 = 0, t25 = 0, t50 = 0;
};

static void tally(const std::vector<double> & sc, const unsigned char * tb,
                  rankstats & s)
{
  std::vector<int> ord(NPAIRS);
  for (int i = 0; i < NPAIRS; i++)
    ord[static_cast<size_t>(i)] = i;
  std::sort(ord.begin(), ord.end(),
            [&sc](int x, int y)
            { return sc[static_cast<size_t>(x)]
                     > sc[static_cast<size_t>(y)]; });

  for (size_t j = 0; j < ord.size(); j++)
    {
      const int idx = ord[j];
      /* recover (a, b) from the a<b enumeration index */
      int a = 0, b = 0, run = 0;
      for (a = 0; a < asize; a++)
        {
          const int wide = asize - 1 - a;
          if (idx < run + wide)
            {
              b = a + 1 + (idx - run);
              break;
            }
          run += wide;
        }
      if (tb[a] != b)
        continue;
      const int rank = static_cast<int>(j) + 1;
      s.sum_rank += rank;
      s.n++;
      if (rank <= 10) s.t10++;
      if (rank <= 25) s.t25++;
      if (rank <= 50) s.t50++;
    }
}

static void report(const char * name, const rankstats & s)
{
  printf("  %-10s  %7.1f  %6.1f%%  %6.1f%%  %6.1f%%\n", name,
         s.sum_rank / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t10) / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t25) / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t50) / static_cast<double>(s.n));
}

int main(int argc, char * * argv)
{
  const int L    = (argc > 1) ? atoi(argv[1]) : 100;
  const int KEYS = (argc > 2) ? atoi(argv[2]) : 400;

  opt_language = "wehrmacht";
  opt_datadir  = "ngrams";
  opt_nstages  = 1;
  opt_stages[0].cap = 13;
  init();
  load_table(SCORE_MONO);

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

  std::mt19937_64 rng(20260828);
  char truth[maxlen + 1];
  rankstats raw, res, stu;
  /* true-plug mean rank split by the count sum of its own two letters, to see
     whether the documented enrichment is carried only by the frequent half */
  double lo_rank = 0, hi_rank = 0;
  long   lo_n = 0, hi_n = 0;
  double sum_r = 0;              /* mean correlation of raw score with m */
  long   nkeys = 0;

  /* Does a plug on a frequent letter IMPROVE the IC, or merely INFLUENCE it
     more?  Split the 325 into quintiles by m = count(a) + count(b) -- per key,
     so the quintiles mean the same thing at every length -- and report the
     mean and sd of the key-centred score in each, for wrong and true plugs
     separately.  A systematic improvement shows up as a rising mean on the
     WRONG plugs; a pure influence effect shows up as a rising sd with the
     mean flat, and then the true plugs' mean is the signal riding on it. */
  double q_m[5] = {0}, w_s[5] = {0}, w_q[5] = {0}, t_s[5] = {0}, t_q[5] = {0};
  long   w_n[5] = {0}, t_n[5] = {0};

  std::vector<double> sc(NPAIRS), rs(NPAIRS), st(NPAIRS);
  std::vector<double> mm(NPAIRS);

  for (int key = 0; key < KEYS; key++)
    {
      const size_t off = rng() % (corpus.size() - static_cast<size_t>(L));
      for (int i = 0; i < L; i++)
        truth[i] = corpus[off + static_cast<size_t>(i)];
      truth[L] = 0;

      int w[3];
      { int pool[5] = {0, 1, 2, 3, 4};
        for (int i = 0; i < 3; i++)
          { int j = i + static_cast<int>(rng() % (5 - i));
            std::swap(pool[i], pool[j]); w[i] = pool[i]; } }
      int r[3], g[3];
      for (int i = 0; i < 3; i++)
        { r[i] = static_cast<int>(rng() % asize);
          g[i] = static_cast<int>(rng() % asize); }

      int tl[asize];
      for (int i = 0; i < asize; i++) tl[i] = i;
      for (int i = 0; i < 20; i++)
        { int j = i + static_cast<int>(rng() % (asize - i));
          std::swap(tl[i], tl[j]); }
      char board[64]; int bn = 0;
      for (int i = 0; i < 20; i += 2)
        { board[bn++] = num2char(tl[i]); board[bn++] = num2char(tl[i + 1]); }
      board[bn] = 0;

      init_walzen(m, 1, w[0], w[1], w[2]);
      set_effective_reflector(m);
      precompute(m);
      init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
      init_steckerbrett(m, board);
      textlength = L;
      for (int i = 0; i < L; i++) num_ciphertext[i] = char2num(truth[i]);
      setup_mapping(m, true);
      decode(m);
      for (int i = 0; i < L; i++) num_ciphertext[i] = char2num(m.plaintext[i]);
      unsigned char tb[asize];
      memcpy(tb, m.steckerbrett, asize);
      init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
      setup_mapping(m, true);

      int cnt[asize] = {0};
      for (int i = 0; i < L; i++)
        cnt[num_ciphertext[i]]++;

      cooc_build(m);
      cooc_plug_scores(m, SCORE_IC, sc.data());

      int i = 0;
      for (int a = 0; a < asize; a++)
        for (int b = a + 1; b < asize; b++)
          mm[static_cast<size_t>(i++)] = cnt[a] + cnt[b];

      /* least squares of score on m, over this key's own 325 plugs */
      double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
      for (int j = 0; j < NPAIRS; j++)
        {
          const double x = mm[static_cast<size_t>(j)];
          const double y = sc[static_cast<size_t>(j)];
          sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y;
        }
      const double nn = NPAIRS;
      const double cov = sxy / nn - (sx / nn) * (sy / nn);
      const double vx  = sxx / nn - (sx / nn) * (sx / nn);
      const double vy  = syy / nn - (sy / nn) * (sy / nn);
      const double beta = (vx > 0) ? cov / vx : 0.0;
      const double alpha = sy / nn - beta * (sx / nn);
      if ((vx > 0) && (vy > 0))
        {
          sum_r += cov / sqrt(vx * vy);
          nkeys++;
        }

      for (int j = 0; j < NPAIRS; j++)
        {
          const double x = mm[static_cast<size_t>(j)];
          rs[static_cast<size_t>(j)] = sc[static_cast<size_t>(j)]
                                       - (alpha + beta * x);
          st[static_cast<size_t>(j)] = (x > 0)
            ? rs[static_cast<size_t>(j)] / sqrt(x) : 0.0;
        }

      tally(sc, tb, raw);
      tally(rs, tb, res);
      tally(st, tb, stu);

      /* quintiles of m, this key's own scores centred on this key's mean */
      {
        std::vector<int> bym(NPAIRS);
        for (int j = 0; j < NPAIRS; j++) bym[static_cast<size_t>(j)] = j;
        std::sort(bym.begin(), bym.end(),
                  [&mm](int x, int y)
                  { return mm[static_cast<size_t>(x)]
                           < mm[static_cast<size_t>(y)]; });
        const double mu = sy / nn;
        for (size_t p = 0; p < bym.size(); p++)
          {
            const int j = bym[p];
            int qi = static_cast<int>(p * 5 / bym.size());
            if (qi > 4) qi = 4;
            const double v = sc[static_cast<size_t>(j)] - mu;
            q_m[qi] += mm[static_cast<size_t>(j)];
            /* recover (a, b) to ask whether this plug is a true one */
            int a = 0, b = 0, run = 0;
            for (a = 0; a < asize; a++)
              {
                const int wide = asize - 1 - a;
                if (j < run + wide) { b = a + 1 + (j - run); break; }
                run += wide;
              }
            if (tb[a] == b) { t_s[qi] += v; t_q[qi] += v * v; t_n[qi]++; }
            else            { w_s[qi] += v; w_q[qi] += v * v; w_n[qi]++; }
          }
      }

      /* the split: true plugs whose own count sum is below / above this key's
         median count sum over all 325 pairs */
      std::vector<double> ms(mm);
      std::sort(ms.begin(), ms.end());
      const double med = ms[static_cast<size_t>(NPAIRS / 2)];
      std::vector<int> ord(NPAIRS);
      for (int j = 0; j < NPAIRS; j++) ord[static_cast<size_t>(j)] = j;
      std::sort(ord.begin(), ord.end(),
                [&sc](int x, int y)
                { return sc[static_cast<size_t>(x)]
                         > sc[static_cast<size_t>(y)]; });
      for (size_t j = 0; j < ord.size(); j++)
        {
          const int idx = ord[j];
          int a = 0, b = 0, run = 0;
          for (a = 0; a < asize; a++)
            {
              const int wide = asize - 1 - a;
              if (idx < run + wide) { b = a + 1 + (idx - run); break; }
              run += wide;
            }
          if (tb[a] != b)
            continue;
          if (cnt[a] + cnt[b] >= med) { hi_rank += static_cast<double>(j) + 1;
                                        hi_n++; }
          else                        { lo_rank += static_cast<double>(j) + 1;
                                        lo_n++; }
        }
    }

  printf("FREQUENCY BIAS in the 325 single-plug IC ranking\n");
  printf("L = %d, %d keys, 10-pair board hidden, rotor key given, "
         "wehrmacht\n\n", L, KEYS);
  printf("  mean corr(IC score, count(a)+count(b)) = %+.3f over %ld keys\n\n",
         sum_r / static_cast<double>(nkeys), nkeys);
  printf("  true-plug enrichment, %d plugs of 325 (chance: 163.0 / 3.1%% / "
         "7.7%% / 15.4%%)\n", 10);
  printf("  %-10s  %7s  %7s  %7s  %7s\n",
         "ranking", "meanrk", "top10", "top25", "top50");
  report("raw", raw);
  report("resid", res);
  report("stud", stu);
  printf("\n  IMPROVE or merely INFLUENCE?  325 plugs in quintiles by\n"
         "  m = count(a)+count(b), scores centred per key (x1000):\n");
  printf("  %-9s %6s   %8s %8s   %8s %8s\n",
         "quintile", "mean m", "wrong mu", "wrong sd", "true mu", "true sd");
  for (int q = 0; q < 5; q++)
    {
      const double wn = static_cast<double>(w_n[q]);
      const double tn = static_cast<double>(t_n[q]);
      const double wmu = w_s[q] / wn;
      const double tmu = t_s[q] / tn;
      printf("  %-9d %6.1f   %+8.3f %8.3f   %+8.3f %8.3f\n", q + 1,
             q_m[q] / (wn + tn), 1000.0 * wmu,
             1000.0 * sqrt(w_q[q] / wn - wmu * wmu),
             1000.0 * tmu, 1000.0 * sqrt(t_q[q] / tn - tmu * tmu));
    }

  printf("\n  raw ranking, true plugs split at this key's median count sum:\n");
  printf("    frequent letters  mean rank %6.1f  (n = %ld)\n",
         hi_rank / static_cast<double>(hi_n), hi_n);
  printf("    rare letters      mean rank %6.1f  (n = %ld)\n",
         lo_rank / static_cast<double>(lo_n), lo_n);
  return 0;
}
