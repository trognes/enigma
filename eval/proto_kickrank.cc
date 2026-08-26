/* PROTOTYPE, not part of the program.  Is k (mono + lambda*L*IC) a better
   ranker of SINGLE PLUGS than IC alone -- the question --biased-random's kick
   weights turn on?

   WHY ASK IT AT ALL.  k beats IC decisively as the CAP-STAGE PRE-PASS model
   (+5.01pp of exact recovery at L=167, five seeds), and --biased-random ranks
   on IC only because that is what the prototype happened to use.  The two
   roles are not the same job, though: a pre-pass climbs a board of several
   plugs under a cap, while this ranks 325 boards holding exactly ONE plug.
   Whether the blend helps there is a separate empirical question.

   AND IT IS THE CHEAP ONE.  An end-to-end recovery A/B is ~10 minutes; this
   is seconds, and CLAUDE.md's own note on the mono+IC probe says such a probe
   "predicts the ORDERING and not the magnitude".  Ordering is all that is
   needed to decide whether the expensive run is worth starting.

   It calls the SHIPPED cooc_build/cooc_plug_scores rather than a private
   reimplementation, so what is measured is the code that would ship.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/kr eval/proto_kickrank.cc $objs
     /tmp/kr [L] [keys]
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

struct stats
{
  double sum_rank = 0;
  long   n = 0;
  long   t10 = 0, t25 = 0, t50 = 0, t100 = 0;
  double drawn = 0;          /* true plugs in a 4-plug softmax kick, T = 1 */
  long   draws = 0;
};

static void tally(const double * sc, const unsigned char * tb, stats & s,
                  std::mt19937_64 & rng)
{
  struct pl { int a, b; double sc; bool tru; };
  std::vector<pl> P;
  P.reserve(NPAIRS);
  int i = 0;
  for (int a = 0; a < asize; a++)
    for (int b = a + 1; b < asize; b++)
      {
        pl e;
        e.a = a;
        e.b = b;
        e.sc = sc[i++];
        e.tru = (tb[a] == b);
        P.push_back(e);
      }

  std::vector<pl> Q(P);
  std::sort(Q.begin(), Q.end(),
            [](const pl & x, const pl & y) { return x.sc > y.sc; });
  for (size_t j = 0; j < Q.size(); j++)
    {
      if (! Q[j].tru)
        continue;
      const int rank = static_cast<int>(j) + 1;
      s.sum_rank += rank;
      s.n++;
      if (rank <= 10)  s.t10++;
      if (rank <= 25)  s.t25++;
      if (rank <= 50)  s.t50++;
      if (rank <= 100) s.t100++;
    }

  /* The operational quantity: how many TRUE plugs a 4-pair kick drawn from
     exp(z / 1) actually lands.  Rejection on conflict, exactly as
     biased_perturb does, so this measures the shipped sampler's behaviour and
     not the ranking in the abstract. */
  double mu = 0;
  for (const pl & e : P)
    mu += e.sc;
  mu /= static_cast<double>(P.size());
  double vr = 0;
  for (const pl & e : P)
    vr += (e.sc - mu) * (e.sc - mu);
  const double sd = sqrt(vr / static_cast<double>(P.size() - 1));

  std::vector<double> cum(P.size());
  double run = 0;
  for (size_t j = 0; j < P.size(); j++)
    {
      run += (sd > 0.0) ? exp((P[j].sc - mu) / sd) : 1.0;
      cum[j] = run;
    }

  for (int rep = 0; rep < 40; rep++)
    {
      bool used[asize] = {false};
      int got = 0, placed = 0, guard = 0;
      while ((placed < 4) && (guard++ < 400))
        {
          const double u = (static_cast<double>(rng() >> 11)
                            / 9007199254740992.0) * run;
          const size_t j = static_cast<size_t>(
            std::lower_bound(cum.begin(), cum.end(), u) - cum.begin());
          if (j >= P.size())
            continue;
          if (used[P[j].a] || used[P[j].b])
            continue;
          used[P[j].a] = true;
          used[P[j].b] = true;
          placed++;
          if (P[j].tru)
            got++;
        }
      s.drawn += got;
      s.draws++;
    }
}

static void report(const char * name, const stats & s)
{
  printf("  %-6s  %7.1f  %6.1f%%  %6.1f%%  %6.1f%%  %6.1f%%   %6.3f\n",
         name, s.sum_rank / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t10) / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t25) / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t50) / static_cast<double>(s.n),
         100.0 * static_cast<double>(s.t100) / static_cast<double>(s.n),
         s.drawn / static_cast<double>(s.draws));
}

int main(int argc, char * * argv)
{
  const int L    = (argc > 1) ? atoi(argv[1]) : 100;
  const int KEYS = (argc > 2) ? atoi(argv[2]) : 200;

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

  std::mt19937_64 rng(20260825);
  char truth[maxlen + 1];
  stats sic, sk;
  double sc_ic[NPAIRS], sc_k[NPAIRS];

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

      cooc_build(m);
      cooc_plug_scores(m, SCORE_IC, sc_ic);
      cooc_plug_scores(m, SCORE_MONOIC, sc_k);

      std::mt19937_64 r1(1000 + static_cast<unsigned>(key));
      std::mt19937_64 r2(1000 + static_cast<unsigned>(key));
      tally(sc_ic, tb, sic, r1);
      tally(sc_k,  tb, sk,  r2);   /* same draw stream: paired */
    }

  printf("TRUE-PLUG ENRICHMENT in the 325 single-plug ranking\n");
  printf("L = %d, %d keys, 10-pair board hidden, rotor key given, "
         "wehrmacht\n\n", L, KEYS);
  printf("  %-6s  %7s  %7s  %7s  %7s  %7s   %6s\n",
         "model", "meanrk", "top10", "top25", "top50", "top100", "kick");
  printf("  %-6s  %7.1f  %6.1f%%  %6.1f%%  %6.1f%%  %6.1f%%   %6.3f\n",
         "chance", 163.0, 3.1, 7.7, 15.4, 30.8, 4.0 * 10.0 / 325.0 * 1.0);
  report("IC", sic);
  report("k", sk);
  printf("\n  meanrk: mean rank of a true plug, 1 = best of 325 (lower is\n"
         "  better).  kick: TRUE plugs landed by a 4-pair softmax kick at\n"
         "  T = 1, drawn by rejection exactly as biased_perturb does -- the\n"
         "  operational quantity, and the two models are drawn against the\n"
         "  SAME random stream so the comparison is paired.\n");
  return 0;
}
