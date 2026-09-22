/* Microbenchmark: the climb's probe loop with and without the cached
   pre-exit decrypt (ENIGMA_QCACHE, src/scoring.cc qcache_probe).

   Four ways to score one plugboard probe at L = 167 under -f's loop shape
   (four private histograms, all8-sized gather), on random tables:

     decode        steck[rows[i][steck[ct[i]]]]  -- what score_iter does
     q+patch       steck'[q[i]], q patched for the ~4/26 of positions whose
                   ciphertext letter the toggle moves, then patched back --
                   what qcache_probe does
     q only        the q loop alone, no patching: the ceiling of the idea
     sorted+copy   q kept letter-sorted so a patch is a contiguous memcpy,
                   read through an index array -- the layout tried and
                   rejected

   Build and run:
     g++ -O3 -std=c++17 -o mb eval/proto_qcache_mb.cc && ./mb
     clang++ -O3 -std=c++17 -o mb eval/proto_qcache_mb.cc && ./mb

   Measured on an x86 Xeon @ 2.8 GHz, ns per probe, the best of three reps
   in each of two runs (the clang reps scatter by up to ~10%):

                 decode   q+patch   q only   sorted+copy
     g++        326-335   298-315  253-263       313-331
     clang      263-283   231-280  203-227       254-274

   The q loop is ~20% cheaper; the patch and restore give most of it back,
   leaving g++ ~-8% and clang anywhere from -12% to nothing. End to end in
   the real climb on the same box it reads within +-5% either way.
   The number this does not have is arm64, where the scorer loops have
   repeatedly behaved differently (CLAUDE.md, the 4x unroll). */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>

enum { A = 26, L = 167, N = 2000000 };

static uint8_t tab[A][A][A][A];
static unsigned char mapping[L][A];
static const unsigned char * rows[L];
static unsigned char ct[L];
static unsigned char q[L];
static uint16_t qpos[L];
static int qstart[A + 1];
static uint16_t inv[L];
static unsigned char qp[L];
static unsigned char qtab[A][A][L];

/* The -f loop body over a decode functor D, as ngram_ic_decode has it. */
template<typename F>
static inline long score(F D)
{
  int f0[A] = {}, f1[A] = {}, f2[A] = {}, f3[A] = {};
  int a = D(0), b = D(1), c = D(2);
  f0[a]++; f1[b]++; f2[c]++;
  long isum = 0;
  int i = 3;
  for (; i + 3 < L; i += 4)
    {
      const int d0 = D(i), d1 = D(i + 1), d2 = D(i + 2), d3 = D(i + 3);
      f0[d0]++; f1[d1]++; f2[d2]++; f3[d3]++;
      isum += tab[a][b][c][d0] + tab[b][c][d0][d1]
              + tab[c][d0][d1][d2] + tab[d0][d1][d2][d3];
      a = d1; b = d2; c = d3;
    }
  for (; i < L; i++)
    {
      const int d = D(i);
      f0[d]++;
      isum += tab[a][b][c][d];
      a = b; b = c; c = d;
    }
  int coin = 0;
  for (int j = 0; j < A; j++)
    {
      const int n = f0[j] + f1[j] + f2[j] + f3[j];
      coin += n * (n - 1);
    }
  return isum * 1000 + coin;
}

__attribute__((noinline)) static long ref(const unsigned char * st)
{
  return score([&](int i) { return st[rows[i][st[ct[i]]]]; });
}

__attribute__((noinline)) static long qs(const unsigned char * st)
{
  return score([&](int i) { return st[q[i]]; });
}

__attribute__((noinline)) static long qps(const unsigned char * st)
{
  return score([&](int i) { return st[qp[inv[i]]]; });
}

__attribute__((noinline)) static void patch(const unsigned char * st,
                                            const int * pos, int cnt)
{
  for (int k = 0; k < cnt; k++)
    {
      const int l = pos[k];
      const unsigned char * col = & mapping[0][0] + st[l];
      for (int j = qstart[l]; j < qstart[l + 1]; j++)
        {
          const int i = qpos[j];
          q[i] = col[i * A];
        }
    }
}

__attribute__((noinline)) static void patch_sorted(const unsigned char * st,
                                                   const int * pos, int cnt)
{
  for (int k = 0; k < cnt; k++)
    {
      const int l = pos[k];
      memcpy(qp + qstart[l], qtab[l][st[l]], qstart[l + 1] - qstart[l]);
    }
}

int main()
{
  std::mt19937 r(1);
  for (auto & x : tab)
    for (auto & y : x)
      for (auto & z : y)
        for (auto & w : z)
          w = r() & 255;
  for (int i = 0; i < L; i++)
    {
      for (int d = 0; d < A; d++)
        mapping[i][d] = r() % A;
      rows[i] = mapping[i];
      ct[i] = r() % A;
    }

  int cnt[A + 1] = {};
  for (int i = 0; i < L; i++)
    cnt[ct[i] + 1]++;
  for (int c = 0; c < A; c++)
    qstart[c + 1] = qstart[c] + cnt[c + 1];
  int fill[A];
  for (int c = 0; c < A; c++)
    fill[c] = qstart[c];
  for (int i = 0; i < L; i++)
    qpos[fill[ct[i]]++] = i;

  unsigned char st[A];   /* a ten-pair board */
  for (int i = 0; i < A; i++)
    st[i] = i;
  for (int i = 0; i < 20; i += 2)
    {
      st[i] = i + 1;
      st[i + 1] = i;
    }
  for (int i = 0; i < L; i++)
    q[i] = rows[i][st[ct[i]]];
  for (int j = 0; j < L; j++)
    inv[qpos[j]] = j;
  for (int l = 0; l < A; l++)
    for (int v = 0; v < A; v++)
      for (int j = qstart[l]; j < qstart[l + 1]; j++)
        qtab[l][v][j - qstart[l]] = mapping[qpos[j]][v];
  for (int l = 0; l < A; l++)
    memcpy(qp + qstart[l], qtab[l][st[l]], qstart[l + 1] - qstart[l]);
  for (int i = 0; i < L; i++)
    if (qp[inv[i]] != q[i])
      return 3;

  /* Each probe swaps two partners and names four letters as moved, the
     MERGE case and the most expensive patch. */
  auto probe = [&](int n, unsigned char * s2, int * pos)
  {
    memcpy(s2, st, A);
    const int a = n % 20, b = (n * 7 + 3) % A;
    std::swap(s2[a], s2[b]);
    pos[0] = a;
    pos[1] = b;
    pos[2] = (a + 1) % A;
    pos[3] = (b + 1) % A;
  };

  long sink = 0;
  for (int rep = 0; rep < 3; rep++)
    {
      unsigned char s2[A];
      int pos[4];
      auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < N; n++)
        {
          probe(n, s2, pos);
          sink += ref(s2);
        }
      auto t1 = std::chrono::steady_clock::now();
      for (int n = 0; n < N; n++)
        {
          probe(n, s2, pos);
          patch(s2, pos, 4);
          sink += qs(s2);
          patch(st, pos, 4);
        }
      auto t2 = std::chrono::steady_clock::now();
      for (int n = 0; n < N; n++)
        {
          probe(n, s2, pos);
          sink += qs(s2);
        }
      auto t3 = std::chrono::steady_clock::now();
      for (int n = 0; n < N; n++)
        {
          probe(n, s2, pos);
          patch_sorted(s2, pos, 4);
          sink += qps(s2);
          patch_sorted(st, pos, 4);
        }
      auto t4 = std::chrono::steady_clock::now();
      auto ns = [](auto x, auto y)
      {
        return std::chrono::duration<double, std::nano>(y - x).count() / N;
      };
      printf("decode %.1f   q+patch %.1f   q only %.1f   sorted+copy %.1f ns\n",
             ns(t0, t1), ns(t1, t2), ns(t2, t3), ns(t3, t4));
    }
  return sink == 42;
}
