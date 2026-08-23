/* PROTOTYPE, not part of the program.  Two questions about ranking the 325
   single-plug boards by IC and sampling from that ranking.

   1. ARE TRUE PLUGS ENRICHED at the top?  That is the precondition for a
      weighted kick -- biasing the draw by the single-plug score can only help
      if a true plug is likelier than chance to sit high in the ranking.

   2. MUST THE DRAW RENORMALISE after each pick?  No: redrawing on a conflict
      IS drawing from the legal set renormalised, so rejection and explicit
      renormalisation are the same distribution and the choice is pure cost.
      Both are run here so the equivalence is checked rather than asserted.

   The end-to-end answer is in eval/results-weighted-kick.txt, and it is a
   null -- this probe measures the mechanism, which works, not the outcome,
   which does not follow.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/rank eval/proto_plugrank.cc $objs
     /tmp/rank [L] [keys] [draws per key]
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

struct ttab
{
  std::vector<uint16_t> v;
  ttab() : v(static_cast<size_t>(asize) * asize * asize, 0) {}
  uint16_t * at(int c, int d)
  { return & v[(static_cast<size_t>(c) * asize + d) * asize]; }
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

int main(int argc, char * * argv)
{
  const int L    = (argc > 1) ? atoi(argv[1]) : 100;
  const int KEYS = (argc > 2) ? atoi(argv[2]) : 200;
  const int DRAWS = (argc > 3) ? atoi(argv[3]) : 4000;

  opt_language = "wehrmacht";
  opt_datadir  = "ngrams";
  opt_scoring  = SCORE_IC;
  opt_nstages  = 1;
  opt_stages[0].model = SCORE_IC;
  opt_stages[0].cap = 13;
  init();

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

  /* temperatures for the softmax over z-scores; 0 marks "uniform" */
  const double TEMP[] = {0.0, 4.0, 2.0, 1.0, 0.5, 0.25};
  const int NT = 6;
  double drawn_true[NT] = {0, 0, 0, 0, 0, 0};
  double drawn_dist[NT] = {0, 0, 0, 0, 0, 0};   /* mean pairwise apartness */
  double rej_true[NT] = {0, 0, 0, 0, 0, 0};     /* same, by REJECTION */
  double rej_draws[NT] = {0, 0, 0, 0, 0, 0};    /* draws per 4-plug kick */
  double rej_fail[NT] = {0, 0, 0, 0, 0, 0};     /* kicks that hit the guard */

  double sum_rank = 0;         /* mean rank of a true plug, 1 = best of 325 */
  long   n_rank = 0;
  long   top10 = 0, top25 = 0, top50 = 0, top100 = 0, total_true = 0;

  char truth[maxlen + 1];
  for (int k = 0; k < KEYS; k++)
    {
      const size_t off = rng() % (corpus.size() - static_cast<size_t>(L));
      for (int i = 0; i < L; i++)
        truth[i] = corpus[off + static_cast<size_t>(i)];
      truth[L] = 0;

      int w[3];
      { int pool[5] = {0,1,2,3,4};
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
        { board[bn++] = num2char(tl[i]); board[bn++] = num2char(tl[i+1]); }
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

      ttab T;
      build_T(m, T);

      /* all 325 single-plug scores, delta from empty */
      int n0[asize] = {0};
      for (int c = 0; c < asize; c++)
        { const uint16_t * col = T.at(c, c);
          for (int y = 0; y < asize; y++) n0[y] += col[y]; }

      struct pl { int a, b; double sc; bool tru; };
      std::vector<pl> P;
      P.reserve(325);
      for (int a = 0; a < asize; a++)
        for (int b = a + 1; b < asize; b++)
          {
            const uint16_t * ma = T.at(a,a); const uint16_t * mb = T.at(b,b);
            const uint16_t * pa = T.at(a,b); const uint16_t * pb = T.at(b,a);
            long c2 = 0;
            for (int y = 0; y < asize; y++)
              { const int nn = n0[y]-ma[y]-mb[y]+pa[y]+pb[y];
                c2 += static_cast<long>(nn) * (nn - 1); }
            pl e;
            e.a = a; e.b = b;
            e.sc = static_cast<double>(c2)
                     / (static_cast<double>(L) * (L - 1));
            e.tru = (tb[a] == b);
            P.push_back(e);
          }

      /* z-scores over the 325 */
      double mu = 0;
      for (const pl & e : P) mu += e.sc;
      mu /= static_cast<double>(P.size());
      double vr = 0;
      for (const pl & e : P) vr += (e.sc - mu) * (e.sc - mu);
      const double sd = sqrt(vr / static_cast<double>(P.size() - 1));

      /* ranks of the true plugs */
      std::vector<pl> Q(P);
      std::sort(Q.begin(), Q.end(),
                [](const pl & x, const pl & y) { return x.sc > y.sc; });
      for (size_t i = 0; i < Q.size(); i++)
        {
          if (! Q[i].tru) continue;
          const int rank = static_cast<int>(i) + 1;
          sum_rank += rank; n_rank++; total_true++;
          if (rank <= 10)  top10++;
          if (rank <= 25)  top25++;
          if (rank <= 50)  top50++;
          if (rank <= 100) top100++;
        }

      /* sample 4-plug kicks at each temperature */
      for (int t = 0; t < NT; t++)
        {
          std::vector<double> wgt(P.size());
          for (size_t i = 0; i < P.size(); i++)
            wgt[i] = (TEMP[t] == 0.0) ? 1.0
                       : exp(((P[i].sc - mu) / sd) / TEMP[t]);
          double sum_apart = 0;
          long npair = 0;
          std::vector<std::vector<int> > drawn;
          for (int d = 0; d < DRAWS; d++)
            {
              bool used[asize] = {false};
              std::vector<int> picked;
              int got = 0, guard = 0;
              while ((got < 4) && (guard++ < 4000))
                {
                  /* renormalise over still-legal plugs */
                  double tot = 0;
                  for (size_t i = 0; i < P.size(); i++)
                    if (! used[P[i].a] && ! used[P[i].b]) tot += wgt[i];
                  if (tot <= 0) break;
                  double x = (static_cast<double>(rng() >> 11)
                              / 9007199254740992.0) * tot;
                  for (size_t i = 0; i < P.size(); i++)
                    {
                      if (used[P[i].a] || used[P[i].b]) continue;
                      x -= wgt[i];
                      if (x <= 0)
                        { used[P[i].a] = used[P[i].b] = true;
                          picked.push_back(static_cast<int>(i));
                          if (P[i].tru) drawn_true[t] += 1.0;
                          got++; break; }
                    }
                }
              if (drawn.size() < 60) drawn.push_back(picked);
            }
          /* apartness among the sampled boards */
          for (size_t i = 0; i < drawn.size(); i++)
            for (size_t j = i + 1; j < drawn.size(); j++)
              {
                unsigned char A[asize], B[asize];
                for (int q = 0; q < asize; q++)
                  { A[q] = static_cast<unsigned char>(q);
                    B[q] = static_cast<unsigned char>(q); }
                for (int q : drawn[i])
                  { A[P[q].a] = static_cast<unsigned char>(P[q].b);
                    A[P[q].b] = static_cast<unsigned char>(P[q].a); }
                for (int q : drawn[j])
                  { B[P[q].a] = static_cast<unsigned char>(P[q].b);
                    B[P[q].b] = static_cast<unsigned char>(P[q].a); }
                int d2 = 0;
                for (int q = 0; q < asize; q++) if (A[q] != B[q]) d2++;
                sum_apart += d2; npair++;
              }
          if (npair) drawn_dist[t] += sum_apart / static_cast<double>(npair);

          /* ---- the same sampler, by REJECTION: draw from the FULL
                  distribution and redraw on a conflict.  No renormalising. */
          std::vector<double> cum(P.size());
          {
            double run = 0;
            for (size_t i = 0; i < P.size(); i++)
              { run += wgt[i]; cum[i] = run; }
          }
          const double tot_all = cum.back();
          for (int d = 0; d < DRAWS; d++)
            {
              bool used[asize] = {false};
              int got = 0, draws = 0;
              while ((got < 4) && (draws < 2000))
                {
                  draws++;
                  const double x = (static_cast<double>(rng() >> 11)
                                    / 9007199254740992.0) * tot_all;
                  const size_t i = static_cast<size_t>(
                    std::lower_bound(cum.begin(), cum.end(), x) - cum.begin());
                  if (i >= P.size()) continue;
                  if (used[P[i].a] || used[P[i].b]) continue;   /* reject */
                  used[P[i].a] = used[P[i].b] = true;
                  if (P[i].tru) rej_true[t] += 1.0;
                  got++;
                }
              rej_draws[t] += draws;
              if (got < 4) rej_fail[t] += 1.0;
            }
        }
    }

  const double kd = KEYS;
  printf("TRUE-PLUG ENRICHMENT in the 325 single-plug IC ranking\n");
  printf("L = %d, %d keys, 10-pair board hidden, rotor key given\n\n", L, KEYS);
  printf("  mean rank of a true plug        %7.1f of 325   (chance 163.0)\n",
         sum_rank / static_cast<double>(n_rank));
  printf("  true plugs in the top 10        %6.1f%%          (chance  3.1%%)\n",
         100.0 * static_cast<double>(top10) / static_cast<double>(total_true));
  printf("  true plugs in the top 25        %6.1f%%          (chance  7.7%%)\n",
         100.0 * static_cast<double>(top25) / static_cast<double>(total_true));
  printf("  true plugs in the top 50        %6.1f%%          (chance 15.4%%)\n",
         100.0 * static_cast<double>(top50) / static_cast<double>(total_true));
  printf("  true plugs in the top 100       %6.1f%%          (chance 30.8%%)\n",
         100.0 * static_cast<double>(top100)/ static_cast<double>(total_true));

  printf("\n  a 4-plug kick, %d draws per key:\n", DRAWS);
  printf("  %-16s %14s %14s %10s %12s %10s %9s\n", "weighting",
         "true plugs/4", "vs uniform", "apart", "REJ true/4", "draws/4",
         "gave up");
  for (int t = 0; t < NT; t++)
    {
      const double per = drawn_true[t] / (kd * DRAWS);
      char lbl[32];
      if (TEMP[t] == 0.0) snprintf(lbl, sizeof lbl, "uniform");
      else snprintf(lbl, sizeof lbl, "softmax T=%.2f", TEMP[t]);
      const double rper = rej_true[t] / (kd * DRAWS);
      printf("  %-16s %14.3f %13.2fx %10.2f %12.3f %10.2f %8.2f%%\n", lbl, per,
             per / (drawn_true[0] / (kd * DRAWS)), drawn_dist[t] / kd,
             rper, rej_draws[t] / (kd * DRAWS),
             100.0 * rej_fail[t] / (kd * DRAWS));
    }
  free(m.subst_array);
  delete mp;
  return 0;
}
