/* PROTOTYPE, not part of the program.  What does the SEED SCORE LANDSCAPE at
   four plugs actually look like on one key?

   The beam (eval/proto_beam.cc) picks four plugs by climbing the k score, and
   the seed-threshold work (eval/results-seed-threshold.txt) selects among
   converged seeds by score, so both assume the score at four plugs SAYS
   something.  This prints the distribution it is reading: one key, one hidden
   10-pair board, the index of coincidence over random four-plug boards, with
   the reference points that give the spread a meaning --

     - the EMPTY board, which is where every seed search starts;
     - random four-plug boards, the null the search is trying to beat;
     - four-plug boards drawn from the TRUE board, the best a four-plug seed
       could possibly be;
     - the beam's own picks, to see where its optimisation lands.

   IC is used rather than the k blend because it needs no language and is the
   half of k that the seed ranking in --self-crib-seeds and --crib-seeds
   already runs on.  Scores come from the T representation
   (eval/results-tseed-proto.txt), which is exact for IC.

   Build (from the repo root, after `make`):

     objs=$(ls src/[a-z]*.o | grep -vE 'main.o|enigma.o')
     g++ -std=c++17 -O3 -pthread -Isrc -o /tmp/dist eval/proto_seeddist.cc $objs
     /tmp/dist [L] [plugs] [samples] [show] [seed]
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

static double ic_of(ttab & T, const unsigned char * S, int L)
{
  int n[asize];
  for (int y = 0; y < asize; y++)
    n[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * col = T.at(c, S[c]);
      for (int y = 0; y < asize; y++)
        n[y] += col[y];
    }
  long coin = 0;
  for (int y = 0; y < asize; y++)
    coin += static_cast<long>(n[y]) * (n[y] - 1);
  return (L > 1) ? static_cast<double>(coin)
                     / (static_cast<double>(L) * (L - 1)) : 0.0;
}

/* ------------------------------------------------------------- helpers ---- */

static void clear_board(unsigned char * S)
{
  for (int i = 0; i < asize; i++)
    S[i] = static_cast<unsigned char>(i);
}

/* A uniform random board of `plugs` disjoint pairs. */
static void random_board(unsigned char * S, int plugs, std::mt19937_64 & rng)
{
  int letters[asize];
  for (int i = 0; i < asize; i++)
    letters[i] = i;
  for (int i = 0; i < 2 * plugs; i++)
    {
      const int j = i + static_cast<int>(rng() % (asize - i));
      std::swap(letters[i], letters[j]);
    }
  clear_board(S);
  for (int i = 0; i < 2 * plugs; i += 2)
    {
      S[letters[i]] = static_cast<unsigned char>(letters[i + 1]);
      S[letters[i + 1]] = static_cast<unsigned char>(letters[i]);
    }
}

static std::string board_str(const unsigned char * S)
{
  std::string s;
  for (int i = 0; i < asize; i++)
    {
      if (S[i] > i)
        {
          if (! s.empty())
            s.push_back(' ');
          s.push_back(num2char(i));
          s.push_back(num2char(S[i]));
        }
    }
  return s.empty() ? std::string("-") : s;
}

/* Greedy beam of width W, one plug per level, ranked on IC. */
static void beam_ic(ttab & T, int L, int W, int plugs,
                    std::vector<std::pair<double, std::string> > & out)
{
  struct cand { unsigned char S[asize]; double sc; };
  std::vector<cand> level(1);
  clear_board(level[0].S);
  level[0].sc = ic_of(T, level[0].S, L);

  for (int step = 0; step < plugs; step++)
    {
      std::vector<cand> next;
      for (const cand & base : level)
        {
          for (int a = 0; a < asize; a++)
            {
              if (base.S[a] != a)
                continue;
              for (int b = a + 1; b < asize; b++)
                {
                  if (base.S[b] != b)
                    continue;
                  cand c;
                  memcpy(c.S, base.S, asize);
                  c.S[a] = static_cast<unsigned char>(b);
                  c.S[b] = static_cast<unsigned char>(a);
                  c.sc = ic_of(T, c.S, L);
                  next.push_back(c);
                }
            }
        }
      std::sort(next.begin(), next.end(),
                [](const cand & x, const cand & y) { return x.sc > y.sc; });
      /* drop exact duplicate boards, keep the best W */
      std::vector<cand> keep;
      for (const cand & c : next)
        {
          bool dup = false;
          for (const cand & k : keep)
            {
              if (memcmp(k.S, c.S, asize) == 0)
                { dup = true; break; }
            }
          if (! dup)
            keep.push_back(c);
          if (static_cast<int>(keep.size()) >= W)
            break;
        }
      level.swap(keep);
    }

  out.clear();
  for (const cand & c : level)
    out.push_back(std::make_pair(c.sc, board_str(c.S)));
}

int main(int argc, char * * argv)
{
  const int L       = (argc > 1) ? atoi(argv[1]) : 100;
  const int PLUGS   = (argc > 2) ? atoi(argv[2]) : 4;
  const int SAMPLES = (argc > 3) ? atoi(argv[3]) : 200000;
  const int SHOW    = (argc > 4) ? atoi(argv[4]) : 20;
  const uint64_t SEED = (argc > 5)
                          ? strtoull(argv[5], nullptr, 10) : 20260823ULL;
  /* KEYS > 1 switches to the SURVEY, which is what says whether the single
     key dumped below is representative.  It is not optional colour: the first
     key inspected here had its best true-plug quadruple at +2.5 sd while the
     beam reached +8.3 on boards holding no true plug at all, and three other
     keys had the truth AT the top.  One example cannot tell those apart. */
  const int KEYS = (argc > 6) ? atoi(argv[6]) : 1;

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

  /* survey accumulators, used when KEYS > 1 */
  double sum_true_z = 0, sum_beam_z = 0;
  int hit_top1 = 0, hit_top8 = 0, n_truth_at_top = 0;

  for (int keyi = 0; keyi < KEYS; keyi++)
    {
    /* ---- one trial: excerpt, rotor key, hidden 10-pair board ------------ */
    char truth[maxlen + 1];
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
    /* NO SEPARATORS: init_steckerbrett() walks the string two characters at a
       time, so a space would be read as a letter -- char2num(' ') is -33 and
       the write lands outside the board. */
    char board[64];
    int bn = 0;
    for (int i = 0; i < 20; i += 2)
      {
        board[bn++] = num2char(tl[i]);
        board[bn++] = num2char(tl[i + 1]);
      }
    board[bn] = 0;

    /* encrypt the excerpt under that key and board */
    init_walzen(m, 1, w[0], w[1], w[2]);
    set_effective_reflector(m);
    precompute(m);
    init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
    init_steckerbrett(m, board);
    textlength = L;
    for (int i = 0; i < L; i++)
      num_ciphertext[i] = char2num(truth[i]);
    setup_mapping(m, true);
    decode(m);                        /* symmetric: this is the encryption */
    for (int i = 0; i < L; i++)
      num_ciphertext[i] = char2num(m.plaintext[i]);

    /* the rotor key is GIVEN; only the board is unknown */
    init_ring_grund(m, r[0], r[1], r[2], g[0], g[1], g[2]);
    setup_mapping(m, true);
    ttab T;
    build_T(m, T);

    /* ---- the distribution ----------------------------------------------- */
    unsigned char S[asize];
    clear_board(S);
    const double ic_empty = ic_of(T, S, L);

    std::vector<double> pop;
    pop.reserve(static_cast<size_t>(SAMPLES));
    std::vector<std::pair<double, std::string> > shown;
    for (int i = 0; i < SAMPLES; i++)
      {
        random_board(S, PLUGS, rng);
        const double v = ic_of(T, S, L);
        pop.push_back(v);
        if (i < SHOW)
          shown.push_back(std::make_pair(v, board_str(S)));
      }
    std::vector<double> sorted(pop);
    std::sort(sorted.begin(), sorted.end());

    double mean = 0;
    for (double v : pop)
      mean += v;
    mean /= static_cast<double>(pop.size());
    double var = 0;
    for (double v : pop)
      var += (v - mean) * (v - mean);
    var /= static_cast<double>(pop.size() - 1);
    const double sd = sqrt(var);
    auto pct = [&](double p)
      {
        size_t i = static_cast<size_t>(p * (sorted.size() - 1));
        return sorted[i];
      };
    auto rank_of = [&](double v)
      {
        return 100.0 * static_cast<double>(
                 std::lower_bound(sorted.begin(), sorted.end(), v)
                 - sorted.begin()) / static_cast<double>(sorted.size());
      };

    /* the best PLUGS-subset of the TRUE board: the ceiling for a seed */
    double ic_true_best = -1, ic_true_worst = 2;
    std::string true_best_str;
    {
      int pl[13][2], np = 0;
      for (int i = 0; i < asize; i++)
        {
          if (m.steckerbrett[i] > i)
            { pl[np][0] = i; pl[np][1] = m.steckerbrett[i]; np++; }
        }
      /* every PLUGS-subset of the np true pairs */
      std::vector<int> idx(static_cast<size_t>(PLUGS));
      for (int i = 0; i < PLUGS; i++)
        idx[static_cast<size_t>(i)] = i;
      while (true)
        {
          clear_board(S);
          for (int i = 0; i < PLUGS; i++)
            {
              const int p = idx[static_cast<size_t>(i)];
              S[pl[p][0]] = static_cast<unsigned char>(pl[p][1]);
              S[pl[p][1]] = static_cast<unsigned char>(pl[p][0]);
            }
          const double v = ic_of(T, S, L);
          if (v > ic_true_best)
            { ic_true_best = v; true_best_str = board_str(S); }
          if (v < ic_true_worst)
            ic_true_worst = v;
          int i = PLUGS - 1;
          while ((i >= 0) && (idx[static_cast<size_t>(i)] == np - PLUGS + i))
            i--;
          if (i < 0)
            break;
          idx[static_cast<size_t>(i)]++;
          for (int j = i + 1; j < PLUGS; j++)
            idx[static_cast<size_t>(j)] = idx[static_cast<size_t>(j - 1)] + 1;
        }
    }

    std::vector<std::pair<double, std::string> > beam;
    beam_ic(T, L, 8, PLUGS, beam);

    /* ---- report (single-key dump only) ------------------------------- */
    if (KEYS == 1)
      {
    printf("IC of %d-plug boards on ONE key.  L = %d, telegraphic German,\n",
           PLUGS, L);
    printf("rotor key given (B %d%d%d), true board hidden: %s\n\n",
           w[0] + 1, w[1] + 1, w[2] + 1,
           board_str(m.steckerbrett).c_str());

    printf("%d random %d-plug boards, the null a seed search must beat:\n",
           SAMPLES, PLUGS);
    printf("  mean %.5f   sd %.5f\n", mean, sd);
    printf("  min %.5f  p1 %.5f  p50 %.5f  p99 %.5f  max %.5f\n\n",
           sorted.front(), pct(0.01), pct(0.50), pct(0.99), sorted.back());

    /* an ASCII histogram over +-4 sd */
    {
      const int NB = 41;
      std::vector<int> bin(static_cast<size_t>(NB), 0);
      const double lo = mean - 4 * sd, hi = mean + 4 * sd;
      for (double v : pop)
        {
          int b = static_cast<int>((v - lo) / (hi - lo) * NB);
          if (b < 0)
            b = 0;
          if (b >= NB)
            b = NB - 1;
          bin[static_cast<size_t>(b)]++;
        }
      int peak = 1;
      for (int b : bin)
        peak = (b > peak) ? b : peak;
      printf("  histogram, %d bins over mean +- 4 sd\n", NB);
      for (int b = 0; b < NB; b++)
        {
          const double c = lo + (b + 0.5) * (hi - lo) / NB;
          const int n = static_cast<int>(
            50.0 * bin[static_cast<size_t>(b)] / peak);
          printf("  %7.5f %+5.1f sd |%.*s\n", c, (c - mean) / sd, n,
                 "**************************************************");
        }
      printf("\n");
    }

    printf("the first %d of those random boards, in the order drawn:\n", SHOW);
    printf("  %-32s %9s %8s %8s\n", "board", "IC", "sd", "pctile");
    for (const std::pair<double, std::string> & p : shown)
      printf("  %-32s %9.5f %+8.2f %7.2f%%\n", p.second.c_str(), p.first,
             (p.first - mean) / sd, rank_of(p.first));
    printf("\n");

    printf("reference points:\n");
    printf("  %-32s %9.5f %+8.2f %7.2f%%\n", "empty board (0 plugs)", ic_empty,
           (ic_empty - mean) / sd, rank_of(ic_empty));
    printf("  %-32s %9.5f %+8.2f %7.2f%%\n", "worst true-plug subset",
           ic_true_worst, (ic_true_worst - mean) / sd, rank_of(ic_true_worst));
    printf("  %-32s %9.5f %+8.2f %7.2f%%\n", "BEST true-plug subset",
           ic_true_best, (ic_true_best - mean) / sd, rank_of(ic_true_best));
    printf("    = %s\n", true_best_str.c_str());
    printf("\n  the IC beam's top 8 (what an optimiser actually returns):\n");
    for (size_t i = 0; i < beam.size(); i++)
      printf("  %-32s %9.5f %+8.2f %7.2f%%\n", beam[i].second.c_str(),
             beam[i].first, (beam[i].first - mean) / sd,
             rank_of(beam[i].first));
      }

      /* ---- survey accumulation ------------------------------------- */
      sum_true_z += (ic_true_best - mean) / sd;
      sum_beam_z += (beam[0].first - mean) / sd;
      if (rank_of(ic_true_best) >= 99.99)
        n_truth_at_top++;
      {
        /* how many of the beam's plugs are TRUE plugs */
        std::string tb = board_str(m.steckerbrett);
        int in1 = 0;
        for (size_t q = 0; q + 1 < beam[0].second.size(); q += 3)
          {
            if (tb.find(beam[0].second.substr(q, 2)) != std::string::npos)
              in1++;
          }
        hit_top1 += in1;
        std::set<std::string> u;
        for (const std::pair<double, std::string> & bp : beam)
          {
            for (size_t q = 0; q + 1 < bp.second.size(); q += 3)
              {
                if (tb.find(bp.second.substr(q, 2)) != std::string::npos)
                  u.insert(bp.second.substr(q, 2));
              }
          }
        hit_top8 += static_cast<int>(u.size());
      }
    }

  if (KEYS > 1)
    {
      const double kd = KEYS;
      printf("\nSURVEY over %d keys, L = %d, %d plugs:\n", KEYS, L, PLUGS);
      printf("  best TRUE-plug subset      %+6.2f sd (mean)\n",
             sum_true_z / kd);
      printf("  IC beam top-1              %+6.2f sd (mean)\n",
             sum_beam_z / kd);
      printf("  truth at the 100th pctile  %d of %d keys\n",
             n_truth_at_top, KEYS);
      printf("  true plugs in beam top-1   %.2f of %d\n",
             hit_top1 / kd, PLUGS);
      printf("  true plugs in beam top-8   %.2f\n", hit_top8 / kd);
    }
  free(m.subst_array);
  delete mp;
  return 0;
}
