#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

/* uwwwrrrggg = 3*8*7*6*26*26*26*26*26*26 = 311 387 102 208 */

static const char * reflector_string[] =
  {
    "EJMZALYXVBWFCRQUONTSPIKHGD",    // A
    "YRUHQSLDPXNGOKMIEBFZCWVJAT",    // B
    "FVPJIAOYEDRZXWGCTKUQSBNMHL",    // C
    "MOWJYPUXNDSRAIBFVLKZGQCHET",    // Norway
    "ENKQAUYWJICOPBLMDXZVFTHRGS",    // UKW-b M4 thin
    "RDOBJNTKVEHMLFCWZAXGYIPSUQ"     // UKW-c M4 thin
  };

static const char * rotor_string[] =
  {
    "EKMFLGDQVZNTOWYHXUSPAIBRCJ",  // i
    "AJDKSIRUXBLHWTMCQGZNPYFVOE",  // ii
    "BDFHJLCPRTXVZNYEIWGAKMUSQO",  // iii
    "ESOVPZJAYQUIRHXLNFTGKDCMWB",  // iv
    "VZBRGITYUPSDNHLXAWMJQOFECK",  // v
    "JPGVOUMFYQBENHZRDKASXLICTW",  // vi
    "NZJHGRCXMYSWBOUFAIVLPEKQDT",  // vii
    "FKQHTLXOCBJSPDZRAMEWNIUYGV",  // viii
    "WTOKASUYVRBXJHQCPZEFMDINLG",  // Norway i
    "GJLPUBSWEMCTQVHXAOFZDRKYNI",  // Norway ii
    "JWFMHNBPUSDYTIXVZGRQLAOEKC",  // Norway iii
    "FGZJMVXEPBWSHQTLIUDYKCNRAO",  // Norway iv
    "HEJXQOTZBVFDASCILWPGYNMURK",  // Norway v
    "LEYJVCNIXWPBQMDRTAKZGFUHOS",  // Beta
    "FSOKANUERHMBTIYCWLQPZXVGJD"   // Gamma
  };

static const char * notch_string[] =
  {
    "Q",
    "E",
    "V",
    "J",
    "Z",
    "MZ",
    "MZ",
    "MZ",
    "Q",
    "E",
    "V",
    "J",
    "Z",
    "",
    ""
  };

static const int maxlen = 1024;   /* maximum ciphertext length (letters) */
static const int asize = 26;
static const int wheels = 3;
static const int reflector_count = sizeof(reflector_string) / sizeof(char *);
static const int rotor_count = sizeof(rotor_string) / sizeof(char *);

/* Layout of the reflector[] / rotor[] wiring tables for Norway Enigma mode:
   reflector index 3 is UKW-N, rotor indices 8-12 are Norway wheels 1-5. */
static const int norway_reflector_index = 3;
static const int norway_rotor_base = 8;

/* A score lower than any achievable plaintext score (all models score >= 0). */
static const double score_min = -1e30;

/* Plaintext scoring models; values match the scoring_name[] order and the
   *_score_decode dispatch in score_iter(). */
enum scoring { SCORE_IC, SCORE_MONO, SCORE_BI, SCORE_TRI, SCORE_QUAD };

static const char * opt_ukw;
static const char * opt_walzen;
static const char * opt_ringstellung;
static const char * opt_grundstellung;
static const char * opt_steckerbrett;
static char * opt_plaintext; /* plaintext to compare to */
static const char * opt_language; /* english, german, danish, french ...; no default */
static int opt_norenigma; /* use the 5 Norenigma (Norway Enigma) wheels */
static int opt_maxwheel;
static int opt_scoring;
static int opt_hillclimb;
static int opt_threads;   /* worker threads for the search (default 1) */
static const int max_threads = 256;

static char ciphertext[maxlen+1];
static char altplaintext[maxlen+1];
static int textlength;
static unsigned char num_ciphertext[maxlen];

/* Read-only wiring tables, derived once by init() and shared by every search. */
static unsigned char rotor_fwd[rotor_count][asize];
static unsigned char rotor_rev[rotor_count][asize];
static unsigned char notch[rotor_count][asize];
static unsigned char reflector[reflector_count][asize];

/* Per-search mutable machine state. Grouping it into one object (rather than
   file-scope globals) makes the search reentrant: a future worker thread can own
   its own machine while the read-only wiring tables, n-gram statistics and
   ciphertext stay shared. */
struct machine
{
  unsigned char steckerbrett[asize];    /* plugboard permutation */
  unsigned char mapping[maxlen][asize]; /* per-position substitution */
  char plaintext[maxlen+1];             /* candidate / result */

  int ukw;                              /* reflector index */
  int walzenlage[wheels];               /* wheel order (rotor indices) */
  unsigned char grundstellung[wheels];  /* start positions */
  unsigned char ringstellung[wheels];   /* ring positions */

  /* The 457 KB rotor-stack table (rebuilt once per wheel order by precompute,
     read per ring/start by setup_mapping) is heap-allocated separately and
     reached through its own base pointer. Keeping it out of the struct leaves
     every member above at a small offset -- so both the hot decode tables and
     subst_array itself get tight addressing on every compiler/target, rather
     than whichever one lands past the big array paying for large offsets (which
     ARM in particular handles poorly). */
  unsigned char (* subst_array)[asize][asize][asize];
};

/* n-gram log-scores stored as float (half the memory of double, so the 457 KB
   quadgram table is ~1.8 MB instead of 3.6 MB and stays warmer in cache); the
   scorers accumulate the looked-up values into a double, so precision of the sum
   is unaffected. */
static float monograms[asize];
static float bigrams[asize][asize];
static float trigrams[asize][asize][asize];
static float quadgrams[asize][asize][asize][asize];

void fatal(const char * message)
{
  fprintf(stderr, "\nFatal error: %s\n", message);
  exit(1);
}

inline int char2num(char x)
{
  return x - 'A';
}

inline char num2char(int x)
{
  return static_cast<char>('A' + x);
}

/* Read an n-gram statistics table from "<language>_<suffix>.txt".

   'table' is the flat backing store of the corresponding global array
   (monograms / bigrams / trigrams / quadgrams). Those arrays are contiguous and
   row-major, so the n letters of a record map to the single index
   ((a*26 + b)*26 + ...) into 'table' of size 26^n. Each entry is seeded with 1
   (Laplace smoothing, so unseen n-grams score log10(1) = 0) and finally stored
   as log10(count + 1) for additive scoring. Parsing stops at end of file or the
   first malformed record. */
void ngrams_read(int n, float * table, const char * suffix)
{
  int size = 1;
  for (int i = 0; i < n; i++)
    size *= asize;

  for (int i = 0; i < size; i++)
    table[i] = 1.0f;

  char filename[64];
  snprintf(filename, sizeof(filename), "%s_%s.txt", opt_language, suffix);

  FILE * f = fopen(filename, "r");
  if (!f)
    {
      fprintf(stderr, "Fatal error: Unable to open the language statistics file %s\n",
              filename);
      exit(1);
    }

  while (1)
    {
      int index = 0;
      int ok = 1;
      for (int k = 0; k < n; k++)
        {
          char a;
          if ((fscanf(f, " %c", & a) != 1) || (a < 'A') || (a > 'Z'))
            {
              ok = 0;
              break;
            }
          index = index * asize + char2num(a);
        }

      int count;
      if (! ok || (fscanf(f, " %d", & count) != 1))
        break;

      table[index] = static_cast<float>(count + 1);
    }

  fclose(f);

  /* compute the log in double and store as float (one rounding, ~7 sig. digits;
     the score sum is still accumulated in double by the scorers) */
  for (int i = 0; i < size; i++)
    table[i] = static_cast<float>(log10(table[i]));
}


void init()
{
  for (int i=0; i < rotor_count; i++)
    for (int j=0; j < asize; j++)
      {
        rotor_fwd[i][j] = char2num(rotor_string[i][j]);
        rotor_rev[i][j] = strchr(rotor_string[i],num2char(j)) - rotor_string[i];
        notch[i][j] = strchr(notch_string[i], num2char(j)) != NULL;
      }

  for (int i=0; i < reflector_count; i++)
    for (int j=0; j < asize; j++)
      reflector[i][j] = char2num(reflector_string[i][j]);
}

void init_steckerbrett(machine & m, const char * steckerbrett_string)
{
  for (int j=0; j < asize; j++)
    m.steckerbrett[j] = j;

  int plug_count = static_cast<int>(strlen(steckerbrett_string) / 2);

  for (int i=0; i < plug_count; i++)
    {
      int a = char2num(steckerbrett_string[2*i+0]);
      int b = char2num(steckerbrett_string[2*i+1]);
      m.steckerbrett[a] = b;
      m.steckerbrett[b] = a;
    }
}

void init_walzen(machine & m, int u, int a, int b, int c)
{
  if (opt_norenigma)
    {
      m.ukw = norway_reflector_index + u;
      m.walzenlage[0] = norway_rotor_base + a;
      m.walzenlage[1] = norway_rotor_base + b;
      m.walzenlage[2] = norway_rotor_base + c;
    }
  else
    {
      m.ukw = u;
      m.walzenlage[0] = a;
      m.walzenlage[1] = b;
      m.walzenlage[2] = c;
    }
}

void init_ring_grund(machine & m, int a, int b, int c, int x, int y, int z)
{
  m.ringstellung[0] = a;
  m.ringstellung[1] = b;
  m.ringstellung[2] = c;
  m.grundstellung[0] = x;
  m.grundstellung[1] = y;
  m.grundstellung[2] = z;
}

int rotor_l(machine & m, int x, int rotor_no)
{
  int y = m.grundstellung[rotor_no] - m.ringstellung[rotor_no];
  x = (x + asize + y) % asize;
  x = rotor_fwd[m.walzenlage[rotor_no]][x];
  x = (x + asize - y) % asize;
  return x;
}

int rotor_r(machine & m, int x, int rotor_no)
{
  int y = m.grundstellung[rotor_no] - m.ringstellung[rotor_no];
  x = (x + asize + y) % asize;
  x = rotor_rev[m.walzenlage[rotor_no]][x];
  x = (x + asize - y) % asize;
  return x;
}

inline int mod26(int x)
{
  return (x+asize)%asize;
}

inline int subst_rotors(machine & m, int x)
{
  for (int r = wheels - 1; r >= 0; r--)
    x = rotor_l(m, x, r);

  x = reflector[m.ukw][x];

  for(int r = 0; r < wheels; r++)
    x = rotor_r(m, x, r);

  return x;
}

void precompute(machine & m)
{
  int r1 = 0;
  int r2 = 0;
  int r3 = 0;
  for (int g1 = 0; g1 < asize; g1++)
    for (int g2 = 0; g2 < asize; g2++)
      for (int g3 = 0; g3 < asize; g3++)
        {
          init_ring_grund(m, r1, r2, r3, g1, g2, g3);
          for (int x = 0; x < asize; x++)
            m.subst_array[g1][g2][g3][x] = subst_rotors(m, x);
        }
}

void setup_mapping(machine & m)
{
  if (textlength > maxlen)
    fatal("Ciphertext too long");

  /* Step the rotors over the message and record, per character position, the
     rotor-stack substitution row (which depends only on the start-minus-ring
     offsets and so is the same for all 26 input letters -- resolve it once and
     copy the 26 bytes).

     The stepping state is held in plain locals for the duration of the loop
     rather than in m.grundstellung: the previous per-character read/modify/write
     through the struct could not be proven not to alias the m.mapping[] store,
     which serialised the loop and cost ~10-14% on the search path (worst on
     ARM). Locals let the compiler keep the rotor positions in registers; the
     final positions are written back once at the end. */
  const unsigned char (* __restrict sa)[asize][asize][asize] = m.subst_array;
  const int w1 = m.walzenlage[1];   /* middle rotor (notch checked for stepping) */
  const int w2 = m.walzenlage[2];   /* right rotor  */
  const int r0 = m.ringstellung[0];
  const int r1 = m.ringstellung[1];
  const int r2 = m.ringstellung[2];
  int g0 = m.grundstellung[0];
  int g1 = m.grundstellung[1];
  int g2 = m.grundstellung[2];

  for (int i = 0; i < textlength; i++)
    {
      /* stepping schedule including the Enigma double-stepping anomaly: the
         middle rotor advances (carrying the left one) when it sits on its own
         notch, as well as on the usual right-rotor carry */
      if (notch[w1][g1])
        {
          g0 = mod26(1 + g0);
          g1 = mod26(1 + g1);
        }
      else if (notch[w2][g2])
        {
          g1 = mod26(1 + g1);
        }
      g2 = mod26(1 + g2);

      const unsigned char * row =
        sa[mod26(g0 - r0)][mod26(g1 - r1)][mod26(g2 - r2)];
      memcpy(m.mapping[i], row, asize);
    }

  m.grundstellung[0] = static_cast<unsigned char>(g0);
  m.grundstellung[1] = static_cast<unsigned char>(g1);
  m.grundstellung[2] = static_cast<unsigned char>(g2);
}

/* Decode one ciphertext position: plugboard -> per-position rotor mapping ->
   plugboard. A tiny inline so decode() and the scorers share one copy of the
   formula. The scorers fuse it into their loops (see below) so the decoded text
   is never materialised in a scratch array. The base pointers are __restrict
   locals the callers have already hoisted out of struct machine. */
inline int decode_at(const unsigned char * __restrict steck,
                     const unsigned char * __restrict map,
                     const unsigned char * __restrict ct,
                     int i)
{
  return steck[map[asize * static_cast<size_t>(i) + steck[ct[i]]]];
}

inline void decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];
  char * __restrict pt = m.plaintext;
  for (int i = 0; i < textlength; i++)
    pt[i] = num2char(decode_at(steck, map, ct, i));
  pt[textlength] = 0;
}

/* The four n-gram scorers fuse decoding into the score loop: each character is
   decoded once, on the fly, into a small sliding window of the last n decoded
   letters that indexes the n-gram table -- so the decoded message is never
   written to and re-read from a scratch array. The quadgram scorer is ~99% of
   hill-climb runtime. The short-text guards (textlength < n) keep the n-1
   pre-roll decodes in bounds and reproduce the old `i < textlength-(n-1)` loops
   (which simply ran zero times for shorter input). */

double quadgram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];

  double score = 0.0;
  if (textlength < 4)
    return score;

  int a = decode_at(steck, map, ct, 0);
  int b = decode_at(steck, map, ct, 1);
  int c = decode_at(steck, map, ct, 2);
  for (int i = 3; i < textlength; i++)
    {
      int d = decode_at(steck, map, ct, i);
      score += quadgrams[a][b][c][d];
      a = b;
      b = c;
      c = d;
    }
  return score;
}

double trigram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];

  double score = 0.0;
  if (textlength < 3)
    return score;

  int a = decode_at(steck, map, ct, 0);
  int b = decode_at(steck, map, ct, 1);
  for (int i = 2; i < textlength; i++)
    {
      int c = decode_at(steck, map, ct, i);
      score += trigrams[a][b][c];
      a = b;
      b = c;
    }
  return score;
}

double bigram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];

  double score = 0.0;
  if (textlength < 2)
    return score;

  int a = decode_at(steck, map, ct, 0);
  for (int i = 1; i < textlength; i++)
    {
      int b = decode_at(steck, map, ct, i);
      score += bigrams[a][b];
      a = b;
    }
  return score;
}

double monogram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];

  double score = 0.0;
  for (int i = 0; i < textlength; i++)
    score += monograms[decode_at(steck, map, ct, i)];
  return score;
}

double ic_score_decode(machine & m)
{
  int freq[asize];
  for(int j=0; j<asize; j++)
    freq[j] = 0;

  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * __restrict map = & m.mapping[0][0];
  for (int i = 0; i < textlength; i++)
    freq[decode_at(steck, map, ct, i)]++;

  double score = 0.0;
  for(int j=0; j<asize; j++)
    score += (double) freq[j] * (freq[j] - 1);
  return (textlength > 1) ? score / ((double) textlength * (textlength - 1)) : 0.0;
}

void showsteckerbrett(machine & m)
{
#if 0
  for (int j=0; j<asize; j++)
    putchar(num2char(m.steckerbrett[j]));
#else
  fprintf(stderr, "S:");
  for (int j=0; j<asize; j++)
    if (m.steckerbrett[j] > j)
      fprintf(stderr, " %c%c", num2char(j), num2char(m.steckerbrett[j]));
#endif
}

void showconfig(machine & m)
{
  /* display wheel numbers 1..N: standard rotors are index+1, Norway wheels are
     index - norway_rotor_base + 1; the reflector prints as its letter (N for
     Norway, else A/B/C). */
  int wheel_offset = opt_norenigma ? 1 - norway_rotor_base : 1;
  fprintf(stderr,
          "W: %c%d%d%d R: %c%c%c G: %c%c%c ",
          opt_norenigma ? 'N' : num2char(m.ukw),
          m.walzenlage[0] + wheel_offset,
          m.walzenlage[1] + wheel_offset,
          m.walzenlage[2] + wheel_offset,
          num2char(m.ringstellung[0]),
          num2char(m.ringstellung[1]),
          num2char(m.ringstellung[2]),
          num2char(m.grundstellung[0]),
          num2char(m.grundstellung[1]),
          num2char(m.grundstellung[2]));
  showsteckerbrett(m);
  fprintf(stderr, "\n");
}

double score_iter(machine & m, int iter)
{
  (void) iter;   /* the iteration counter is only used by SHOWHILLCLIMB */
  double score = 0;

  switch(opt_scoring)
    {
    case SCORE_IC:
      score = ic_score_decode(m);
      break;

    case SCORE_MONO:
      score = monogram_score_decode(m);
      break;

    case SCORE_BI:
      score = bigram_score_decode(m);
      break;

    case SCORE_TRI:
      score = trigram_score_decode(m);
      break;

    case SCORE_QUAD:
      score = quadgram_score_decode(m);
      break;

    default:
      fatal("Illegal scoring type");
    }

  return score;
}

int count[asize];
int order[asize];

int compare(const void * x, const void * y)
{
  int a = count[*(const int*)(x)];
  int b = count[*(const int*)(y)];

  if (a<b)
    return +1;
  else if (a>b)
    return -1;
  else
    return 0;
}

void ciphertext_letterdist()
{
  for(int j=0; j<asize; j++)
    {
      count[j]=0;
      order[j] = j;
    }

  for (int i=0; i<textlength; i++)
    count[char2num(ciphertext[i])]++;

  qsort(order, asize, sizeof(int), compare);

#if 0
  fprintf(stderr, "Ciphertext letter order: ");
  for(int j=0; j<asize; j++)
    fprintf(stderr, "%c", num2char(order[j]));
  fprintf(stderr, "\n");
#endif
}

double hillclimb(machine & m)
{
  /* Try to find the optimal steckerbrett for the given other settings */

  double best_score;
  double last_best;

  int iter = 1;

  /* iterate until a full pass over all plug swaps yields no improvement */
  do
    {
      best_score = score_iter(m, iter);

      last_best = best_score;

      double switch_score = best_score;
      int switch_a = 0;
      int switch_b = 0;

      //#define SHOWHILLCLIMB

#ifdef SHOWHILLCLIMB
      fprintf(stderr, "  ");
      for(int b=1; b<asize; b++)
        fprintf(stderr, "   %c", num2char(b));
      fprintf(stderr, "\n");
#endif
      for(int a=0; a<asize; a++)
      {
#ifdef SHOWHILLCLIMB
        fprintf(stderr, "%c:", num2char(a));
        for(int b=1; b<a+1; b++)
          fprintf(stderr, "    ");
#endif
        for(int b=a+1; b<asize; b++)
          {
            /* switch plugs */
            int x = m.steckerbrett[a];
            int y = m.steckerbrett[b];
            int xx = m.steckerbrett[x];
            int yy = m.steckerbrett[y];
            m.steckerbrett[x] = x;
            m.steckerbrett[y] = y;
            m.steckerbrett[a] = b;
            m.steckerbrett[b] = a;

            double score = score_iter(m, iter);

#ifdef SHOWHILLCLIMB
            fprintf(stderr, "%4.0f", (score - best_score)/10.0);
#endif

            if (score > switch_score)
              {
                switch_score = score;
                switch_a = a;
                switch_b = b;
              }

            /* restore plugs */
            m.steckerbrett[a] = x;
            m.steckerbrett[b] = y;
            m.steckerbrett[x] = xx;
            m.steckerbrett[y] = yy;
          }
#ifdef SHOWHILLCLIMB
        printf("\n");
#endif
        }

      if (switch_score - best_score > 0)
        {

          /* good move */

          int a = switch_a;
          int b = switch_b;

          /* switch plugs */
          int x = m.steckerbrett[a];
          int y = m.steckerbrett[b];
          m.steckerbrett[x] = x;
          m.steckerbrett[y] = y;
          m.steckerbrett[a] = b;
          m.steckerbrett[b] = a;

#ifdef SHOWHILLCLIMB
          fprintf(stderr,
                  "%2d %c%c Imp: %10.4f Score: %10.4f ",
                  iter,
                  num2char(a), num2char(b),
                  switch_score - best_score,
                  switch_score);
          showsteckerbrett(m);
          fprintf(stderr, "\n");
#endif

          best_score = switch_score;
        }

      iter++;
    }
  while (best_score > last_best);

  decode(m);

#ifdef SHOWHILLCLIMB
  printf("Plaintext: %s\n", m.plaintext);
#endif
  return score_iter(m, 0);
}




/* The reflector x wheel-order combinations are the unit of parallelism: each is
   independent (its own precompute + ring/start sweep). The ring/start ranges are
   identical for every task. */
struct wheel_task
{
  int u;
  int w[wheels];
};

struct search_range
{
  int r_min[wheels], r_max[wheels];
  int g_min[wheels], g_max[wheels];
};

/* Best result so far, shared across worker threads. It is updated (and the live
   progress line printed) under the mutex only when a worker beats the current
   global best; improvements are rare, so contention is negligible. */
struct best_result
{
  std::mutex mutex;
  double score = score_min;
  bool found = false;
  char plaintext[maxlen+1];
};

/* --- parallel search -------------------------------------------------------

   The search runs in two parallel phases over a fixed pool of per-thread
   machines:

   1. Precompute the rotor-stack table for every (reflector x wheel-order) once,
      into one big shared read-only block. (A table depends only on the reflector
      and wheel order, and serves every ring/start of that wheel order via the
      start-minus-ring offset; brute force has no early exit, so every table is
      needed anyway.)
   2. Sweep the whole flat (wheel-order x ring x start) key space: an atomic
      counter hands out adaptive-sized chunks, each worker decodes and scores its
      keys against the shared tables using its own private mapping.

   Parallelising the flat key space (not just the wheel order) means a search
   with the wheels fixed but ring/start wildcarded uses every thread -- the old
   wheel-order-only scheme left exactly that case single-threaded. */

/* Memory accounting for the final diagnostic (set by bruteforce). */
static size_t g_table_count = 0;
static size_t g_table_bytes = 0;

/* base pointer into the rotor-stack table block: the same type as
   machine::subst_array, so 'all + i*asize' is task i's [asize]^4 table */
typedef unsigned char (* subst_table)[asize][asize][asize];

/* Phase 1: fill the table for each wheel-order task pulled off the counter.
   all + i*asize is task i's table (asize rows of [asize][asize][asize]). */
void precompute_worker(machine & m,
                       const std::vector<wheel_task> & tasks,
                       std::atomic<size_t> & next_task,
                       subst_table all)
{
  size_t i;
  while ((i = next_task.fetch_add(1)) < tasks.size())
    {
      const wheel_task & t = tasks[i];
      init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
      m.subst_array = all + i * asize;
      precompute(m);
    }
}

/* Phase 2: decode + score a slice of the flat key space. A flat index decodes to
   (wheel-order, ring combo, start combo) by mixed radix over the per-position
   ranges; the worker points its machine at the already-computed table for that
   wheel order (no recompute) and re-reads the wheel order's settings only when
   it changes from one key to the next. */
void search_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range,
                   const int * rc, const int * gc,
                   subst_table all,
                   size_t rsize, size_t gsize,
                   std::atomic<size_t> & next_key,
                   size_t chunk,
                   best_result & best)
{
  const size_t rg = rsize * gsize;
  const size_t total = tasks.size() * rg;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];

  double local_best = score_min;
  size_t cur_wo = static_cast<size_t>(-1);

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      size_t end = start + chunk;
      if (end > total)
        end = total;

      for (size_t idx = start; idx < end; idx++)
        {
          size_t wo = idx / rg;
          size_t rem = idx % rg;
          size_t rflat = rem / gsize;
          size_t gflat = rem % gsize;

          if (wo != cur_wo)
            {
              cur_wo = wo;
              const wheel_task & t = tasks[wo];
              m.subst_array = all + wo * asize;
              init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
            }

          int r1 = range.r_min[0] + static_cast<int>(rflat / rc12);
          int rr = static_cast<int>(rflat % rc12);
          int r2 = range.r_min[1] + rr / rc[2];
          int r3 = range.r_min[2] + rr % rc[2];
          int g1 = range.g_min[0] + static_cast<int>(gflat / gc12);
          int gg = static_cast<int>(gflat % gc12);
          int g2 = range.g_min[1] + gg / gc[2];
          int g3 = range.g_min[2] + gg % gc[2];

          init_ring_grund(m, r1, r2, r3, g1, g2, g3);
          init_steckerbrett(m, opt_steckerbrett);
          setup_mapping(m);

          double score;
          if (opt_hillclimb)
            score = hillclimb(m);
          else
            {
              decode(m);
              score = score_iter(m, 0);
            }

          if (score > local_best)
            {
              local_best = score;
              std::lock_guard<std::mutex> lock(best.mutex);
              if (score > best.score)
                {
                  best.score = score;
                  best.found = true;
                  memcpy(best.plaintext, m.plaintext, textlength + 1);
#if 1
                  /* setup_mapping stepped grundstellung; restore the start
                     positions before echoing the config */
                  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
                  fprintf(stderr, "%10.4f ", score);
                  showconfig(m);
#endif
                }
            }
        }
    }
}

/* Resolve the search ranges from the options, enumerate the reflector x
   wheel-order tasks, precompute their rotor tables in parallel, then sweep the
   flat (wheel-order x ring x start) key space in parallel. The best decryption
   is written to 'result'. */
void bruteforce(char * result)
{
  int u_min, u_max;
  int w_min[wheels], w_max[wheels];
  search_range range;

  if (opt_norenigma)
    {
      u_min = 0;
      u_max = 0;
    }
  else
    {
      if (opt_ukw[0] == '.')
        {
          u_min = 0;
          u_max = 2;
        }
      else
        u_min = u_max = char2num(opt_ukw[0]);
    }

  for(int i=0; i<wheels; i++)
    {
      if (opt_walzen[i] == '.')
        {
          w_min[i] = 0;
          w_max[i] = opt_maxwheel - 1;
        }
      else
        {
          w_min[i] = w_max[i] = opt_walzen[i] - '1';
        }

      if (opt_ringstellung[i] == '.')
        {
          range.r_min[i] = 0;
          range.r_max[i] = 25;
        }
      else
        {
          range.r_min[i] = range.r_max[i] = char2num(opt_ringstellung[i]);
        }

      if (opt_grundstellung[i] == '.')
        {
          range.g_min[i] = 0;
          range.g_max[i] = 25;
        }
      else
        {
          range.g_min[i] = range.g_max[i] = char2num(opt_grundstellung[i]);
        }
    }

  /* per-position ring/start counts and their products */
  int rc[wheels], gc[wheels];
  for (int i = 0; i < wheels; i++)
    {
      rc[i] = range.r_max[i] - range.r_min[i] + 1;
      gc[i] = range.g_max[i] - range.g_min[i] + 1;
    }
  size_t rsize = static_cast<size_t>(rc[0]) * rc[1] * rc[2];
  size_t gsize = static_cast<size_t>(gc[0]) * gc[1] * gc[2];

  std::vector<wheel_task> tasks;
  for (int u1 = u_min; u1 <= u_max; u1++)
    for (int w1 = w_min[0]; w1 <= w_max[0]; w1++)
      for (int w2 = w_min[1]; w2 <= w_max[1]; w2++)
        for (int w3 = w_min[2]; w3 <= w_max[2]; w3++)
          if ((w1 != w2) && (w1 != w3) && (w2 != w3))
            tasks.push_back(wheel_task{u1, {w1, w2, w3}});

  /* The option validation should make this unreachable, but never run an empty
     search and emit uninitialised output. */
  if (tasks.empty())
    fatal("No machine configuration was searched "
          "(check the -u / -w / -x settings)");

  size_t nwo = tasks.size();
  size_t total_keys = nwo * rsize * gsize;

  /* memory accounting: one [asize]^4 table per wheel order, all resident */
  g_table_count = nwo;
  g_table_bytes = nwo * static_cast<size_t>(asize) * asize * asize * asize;
  if (g_table_bytes > static_cast<size_t>(8) * 1024 * 1024 * 1024)
    fatal("Search space too large to precompute the rotor tables "
          "(narrow -u / -w / -x)");

  /* never start more threads than there is work to hand out */
  int nthreads = opt_threads;
  if (total_keys < static_cast<size_t>(nthreads))
    nthreads = static_cast<int>(total_keys);
  if (nthreads < 1)
    nthreads = 1;

  /* the shared, read-only rotor-stack tables (one [asize]^4 block per wheel
     order) and the per-thread machines (small: mapping/plaintext/settings) */
  subst_table all = new unsigned char[nwo * asize][asize][asize][asize];

  std::vector<machine *> machines(static_cast<size_t>(nthreads));
  for (int t = 0; t < nthreads; t++)
    machines[t] = new machine();   /* subst_array is pointed at 'all' per task */

  /* phase 1: precompute every wheel order's table in parallel */
  std::atomic<size_t> next_task{0};
  if (nthreads == 1)
    precompute_worker(*machines[0], tasks, next_task, all);
  else
    {
      std::vector<std::thread> pool;
      pool.reserve(static_cast<size_t>(nthreads));
      for (int t = 0; t < nthreads; t++)
        pool.emplace_back(precompute_worker, std::ref(*machines[t]),
                          std::cref(tasks), std::ref(next_task), all);
      for (std::thread & th : pool)
        th.join();
    }

  /* phase 2: sweep the flat key space in parallel, adaptive chunks (each thread
     gets ~16 chunks: enough to balance the tail, few enough to amortise the
     atomic) */
  best_result best;
  std::atomic<size_t> next_key{0};
  size_t chunk = total_keys / (static_cast<size_t>(nthreads) * 16);
  if (chunk < 1)
    chunk = 1;

  if (nthreads == 1)
    search_worker(*machines[0], tasks, range, rc, gc, all,
                  rsize, gsize, next_key, chunk, best);
  else
    {
      std::vector<std::thread> pool;
      pool.reserve(static_cast<size_t>(nthreads));
      for (int t = 0; t < nthreads; t++)
        pool.emplace_back(search_worker, std::ref(*machines[t]),
                          std::cref(tasks), std::cref(range), rc, gc, all,
                          rsize, gsize, std::ref(next_key), chunk,
                          std::ref(best));
      for (std::thread & th : pool)
        th.join();
    }

  for (int t = 0; t < nthreads; t++)
    delete machines[t];
  delete[] all;

  if (! best.found)
    fatal("No machine configuration produced a score");

  memcpy(result, best.plaintext, textlength + 1);
}

void readciphertext()
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0)
    for (ssize_t i = 0; i < len; i++)
      {
        char c = toupper(buffer[i]);
        if ((c >= 'A') && (c <= 'Z'))
          {
            if (j >= maxlen)
              {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "Ciphertext too long (maximum is %d letters)", maxlen);
                fatal(msg);
              }
            ciphertext[j++] = c;
          }
      }

  if (len < 0)
    fatal("Error reading ciphertext from standard input");

  ciphertext[j] = 0;
  textlength = j;
}

void readplaintext(char * filename, const char * result)
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;

  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    fatal("Unable to open plaintext file");

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(fd, buffer, sizeof(buffer))) > 0)
    for (ssize_t i = 0; i < len; i++)
      {
        char c = toupper(buffer[i]);
        if ((c >= 'A') && (c <= 'Z'))
          {
            if (j >= maxlen)
              {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "Plaintext file too long (maximum is %d letters)", maxlen);
                fatal(msg);
              }
            altplaintext[j++] = c;
          }
      }

  int read_error = (len < 0);
  close(fd);
  if (read_error)
    fatal("Error reading plaintext file");

  altplaintext[j] = 0;

  if (textlength != j)
    fatal("Plaintext not same length as ciphertext");

  int identical = 0;
  for (int i=0; i<textlength; i++)
    if (result[i] == altplaintext[i])
      identical++;

  fprintf(stderr,
          "%d of %d letters (%.1f%%) identical to given plaintext\n",
          identical,
          textlength,
          100.0 * identical / textlength);
}

void alltoupper(char * text)
{
  int len = strlen(text);
  for(int i=0; i<len; i++)
    text[i] = toupper(text[i]);
}

/* version()/help() take the output stream: explicit -h/-v write to stdout and
   exit 0, while usage errors (no arguments, bad option) write to stderr and
   exit 1. */
void version(FILE * out)
{
  fprintf(out, "Enigma cipher tool version 1.1.0\n");
  fprintf(out, "Copyright (C) 2017-2026 Torbjørn Rognes\n");
  fprintf(out, "\n");
}

void help(FILE * out)
{
  version(out);
  fprintf(out, "Usage: enigma [OPTIONS]\n");
  fprintf(out, "  -h           Show help information\n");
  fprintf(out, "  -v           Show version information\n");
  fprintf(out, "  -u X         Reflector (umkehrwalze) X (A-C, N or .) [.]\n");
  fprintf(out, "  -w XYZ       Wheels (walzen) XYZ (1-8 or .) [...]\n");
  fprintf(out, "  -x integer   Highest wheel number to use (3-8) [5]\n");
  fprintf(out, "  -n           Use the Norway Enigma reflector (N) and wheels (1-5)\n");
  fprintf(out, "  -r XYZ       Ring positions (ringstellung) XYZ (A-Z or .) [AA.]\n");
  fprintf(out, "  -g XYZ       Start positions (grundstellung) XYZ (A-Z or .) [...]\n");
  fprintf(out, "  -s AB...     Plugboard (steckerbrett) letter pairs (A-Z pairs) [none]\n");
  fprintf(out, "  -c           Perform hill climbing to determine plugboard settings\n");
  fprintf(out, "  -l language  Scoring language (english, german, danish, french); required\n");
  fprintf(out, "               for -m/-b/-t/-q (no default), not used by -i\n");
  fprintf(out, "  -i           Use index of coincidence (IC) to determine plaintext score\n");
  fprintf(out, "  -m           Use monogram statistics to determine plaintext score\n");
  fprintf(out, "  -b           Use bigram statistics to determine plaintext score\n");
  fprintf(out, "  -t           Use trigram statistics to determine plaintext score\n");
  fprintf(out, "  -q           Use quadgram statistics to determine plaintext score [default]\n");
  fprintf(out, "  -p filename  Name of file containing plaintext to compare result with\n");
  fprintf(out, "  -T integer   Number of worker threads for the search (1-256) [1]\n");
  fprintf(out, "\n");
  fprintf(out, "Defaults are indicated in [square brackets].\n");
  fprintf(out, "\n");
  fprintf(out, "The ciphertext is read from standard input. The final plaintext is written\n");
  fprintf(out, "to standard output.\n");
  fprintf(out, "\n");
  fprintf(out, "For the reflector, wheels, ring position and start position, a dot (.)\n");
  fprintf(out, "works as a wild card, leaving it unspecified. When these settings are not\n");
  fprintf(out, "specified, the program will try all combinations to find the settings\n");
  fprintf(out, "resulting in the highest plaintext score. If asked for, a hill climbing\n");
  fprintf(out, "algorithm will be used to try to determine the plugboard settings.\n");
  fprintf(out, "\n");
}

void removespaces(char * p)
{
  char * q = p;
  while(char c = *p++)
    if (c != ' ')
      *q++ = c;
  *q=0;
}

/* Echo the resolved run configuration to stderr so it is never a mystery what
   scoring model / language / settings a run is actually using. A dot (.) in the
   reflector/wheels/ring/start fields means that position is being searched. */
void show_settings()
{
  static const char * const scoring_name[] =
    { "index of coincidence", "monograms", "bigrams", "trigrams", "quadgrams" };

  fprintf(stderr, "Ciphertext: %d letters\n", textlength);

  fprintf(stderr, "Scoring:    %s", scoring_name[opt_scoring]);
  if (opt_scoring == 0)
    fprintf(stderr, " (language-independent)");
  else
    fprintf(stderr, " (language: %s)", opt_language);
  fprintf(stderr, "; plugboard hill-climb: %s", opt_hillclimb ? "yes" : "no");
  fprintf(stderr, "; threads: %d\n", opt_threads);

  fprintf(stderr, "Machine:    %s Enigma; reflector %s, wheels %s",
          opt_norenigma ? "Norway" : "standard", opt_ukw, opt_walzen);
  if (strchr(opt_walzen, '.'))
    fprintf(stderr, " (max wheel %d)", opt_maxwheel);
  fprintf(stderr, ", ring %s, start %s\n", opt_ringstellung, opt_grundstellung);

  fprintf(stderr, "Plugboard:  ");
  if (opt_steckerbrett[0])
    {
      /* print the de-spaced pairs with a space between each pair (AB CD ...) */
      for (int i = 0; opt_steckerbrett[i]; i++)
        {
          if ((i > 0) && (i % 2 == 0))
            fputc(' ', stderr);
          fputc(opt_steckerbrett[i], stderr);
        }
      fputc('\n', stderr);
    }
  else
    fprintf(stderr, "(none)\n");
}

int main(int argc, char * * argv)
{
  auto t_start = std::chrono::steady_clock::now();

  if (argc == 1)
    {
      help(stderr);
      exit(1);
    }

  /* set default arguments */
  opt_ukw = ".";
  opt_walzen = "...";
  opt_ringstellung = "AA.";
  opt_grundstellung = "...";
  opt_steckerbrett = "";
  opt_language = 0;   /* no default; required for n-gram scoring (-m/-b/-t/-q) */
  opt_plaintext = 0;
  opt_maxwheel = 5;
  opt_hillclimb = 0;
  opt_scoring = SCORE_QUAD;
  opt_norenigma = 0;
  opt_threads = 1;

  /* get arguments */

  int c;
  while ((c = getopt(argc, argv, "u:w:r:g:s:p:l:x:T:imbtqcvhn")) != -1)
    {
      switch (c)
        {
        case 'u':
          alltoupper(optarg);
          opt_ukw = optarg;
          break;
        case 'w':
          opt_walzen = optarg;
          break;
        case 'r':
          alltoupper(optarg);
          opt_ringstellung = optarg;
          break;
        case 'g':
          alltoupper(optarg);
          opt_grundstellung = optarg;
          break;
        case 's':
          alltoupper(optarg);
          removespaces(optarg);
          opt_steckerbrett = optarg;
          break;
        case 'p':
          opt_plaintext = optarg;
          break;
        case 'i':
          opt_scoring = SCORE_IC;
          break;
        case 'm':
          opt_scoring = SCORE_MONO;
          break;
        case 'b':
          opt_scoring = SCORE_BI;
          break;
        case 't':
          opt_scoring = SCORE_TRI;
          break;
        case 'q':
          opt_scoring = SCORE_QUAD;
          break;
        case 'c':
          opt_hillclimb = 1;
          break;
        case 'x':
          opt_maxwheel = atoi(optarg);
          break;
        case 'T':
          opt_threads = atoi(optarg);
          break;
        case 'l':
          opt_language = optarg;
          break;
        case 'v':
          version(stdout);
          exit(0);
          break;
        case 'h':
          help(stdout);
          exit(0);
          break;
        case 'n':
          opt_norenigma = 1;
          break;
        default:
          fprintf(stderr, "\n");
          help(stderr);
          exit(1);
          break;
        }
    }

  /* validate arguments */

  if (opt_norenigma)
    {
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "N.") != 1))
        fatal("Illegal ukw string (must be N or .)");

      if ((strlen(opt_walzen) != wheels) ||
          (strspn(opt_walzen, "12345.") != wheels))
        fatal("Illegal walzen string (must be 3 digits (1-5) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 5))
        fatal("Illegal max wheel (must be 3 to 5)");
    }
  else
    {
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "ABC.") != 1))
        fatal("Illegal ukw string (must be A, B, C or .)");

      if ((strlen(opt_walzen) != wheels) ||
          (strspn(opt_walzen, "12345678.") != wheels))
        fatal("Illegal walzen string (must be 3 digits (1-8) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 8))
        fatal("Illegal max wheel (must be 3-8)");
    }

  /* A wheel cannot occupy two positions at once. Reject any explicitly named
     (non-'.') wheel repeated across positions: otherwise the permutation guard
     in bruteforce() skips every combination and the search silently finds
     nothing. */
  for (int i = 0; i < wheels; i++)
    for (int j = i + 1; j < wheels; j++)
      if ((opt_walzen[i] != '.') && (opt_walzen[i] == opt_walzen[j]))
        fatal("Illegal walzen string (a wheel cannot be used in two positions)");

  if ((strlen(opt_ringstellung) != wheels) ||
      (strspn(opt_ringstellung, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.") != wheels))
    fatal("Illegal ringstellung string (must be 3 letters (A-Z) or .)");

  if ((strlen(opt_grundstellung) != wheels) ||
      (strspn(opt_grundstellung, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.") != wheels))
    fatal("Illegal grundstellung string (must be 3 letters (A-Z) or .)");

  if ((strlen(opt_steckerbrett) > asize) ||
      (strspn(opt_steckerbrett, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") <
       strlen(opt_steckerbrett)))
    fatal("Illegal steckerbrett string (must be up to 13 letter pairs)");

  if ((opt_threads < 1) || (opt_threads > max_threads))
    fatal("Illegal thread count (must be 1 to 256)");

  /* The n-gram scoring models (mono/bi/tri/quad) need a language, with no
     default; the index of coincidence (-i) is language-independent. */
  if ((opt_scoring != SCORE_IC) && (! opt_language))
    fatal("A scoring language is required: add -l <language> "
          "(e.g. -l english), or use -i for the language-independent "
          "index of coincidence");

  if (opt_language &&
      ((strlen(opt_language) < 1) ||
       (strlen(opt_language) > 32) ||
       (strspn(opt_language,
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") <
        strlen(opt_language))))
    fatal("Illegal language name (must be 1-32 letters, e.g. english)");


  /* Load the n-gram table for the chosen scoring model first, so a missing or
     mistyped -l fails immediately (with the offending filename) before we read
     and consume standard input. */

  switch (opt_scoring)
    {
    case SCORE_IC:
      break;
    case SCORE_MONO:
      ngrams_read(1, monograms, "monograms");
      break;
    case SCORE_BI:
      ngrams_read(2, & bigrams[0][0], "bigrams");
      break;
    case SCORE_TRI:
      ngrams_read(3, & trigrams[0][0][0], "trigrams");
      break;
    case SCORE_QUAD:
      ngrams_read(4, & quadgrams[0][0][0][0], "quadgrams");
      break;
    default:
      break;
    }

  /* read ciphertext */

  readciphertext();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  ciphertext_letterdist();

  init();

  /* try all combinations (bruteforce allocates one machine per worker thread) */

  char result[maxlen+1];
  bruteforce(result);

  /* write plaintext */

  fprintf(stderr, "\n");
  printf("%s\n", result);

  /* read plaintext to compare to, if given */

  if (opt_plaintext)
    readplaintext(opt_plaintext, result);

  /* final diagnostic: wall-clock time and memory use */
  double secs = std::chrono::duration<double>
    (std::chrono::steady_clock::now() - t_start).count();
  struct rusage ru;
  double peak_mb = 0.0;
  if (getrusage(RUSAGE_SELF, & ru) == 0)
    {
      /* ru_maxrss is kilobytes on Linux but bytes on macOS/BSD */
#ifdef __APPLE__
      peak_mb = ru.ru_maxrss / (1024.0 * 1024.0);
#else
      peak_mb = ru.ru_maxrss / 1024.0;
#endif
    }
  fprintf(stderr,
          "Finished in %.2f s using %d thread%s; "
          "precomputed %zu rotor table%s (%.1f MB); peak memory %.0f MB\n",
          secs, opt_threads, (opt_threads == 1) ? "" : "s",
          g_table_count, (g_table_count == 1) ? "" : "s",
          g_table_bytes / (1024.0 * 1024.0), peak_mb);
}
