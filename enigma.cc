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

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <queue>
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
static const char * opt_datadir;  /* directory holding the n-gram files (default ".") */
static int opt_norenigma; /* use the 5 Norenigma (Norway Enigma) wheels */
static int opt_m4;        /* use M4 (4-rotor naval) mode */
/* M4 mode: 4th "Greek" wheel (Beta/Gamma, rotor indices 13-14) is static (never
   steps) and folds into a thin reflector (UKW-b/c, reflector indices 4-5) to form
   an effective reflector -- so the machine stays a 3-stepping-rotor engine. The
   Greek wheel and ring/start are taken from the first character of -w/-r/-g. */
static const int m4_thin_base = 4;   /* reflector index of UKW-b; UKW-c is +1 */
static const int greek_base = 13;    /* rotor index of Beta; Gamma is +1 */
static char opt_greek_walzen = '.';      /* Greek wheel: B (Beta), G (Gamma) or . */
static char opt_greek_ringstellung = '.';   /* Greek ring letter or . */
static char opt_greek_grundstellung = '.';  /* Greek start letter or . */
static int opt_maxwheel;
static int opt_scoring;
static int opt_hillclimb;
static int opt_restarts;  /* plugboard hill-climb random restarts (1 = none) */
static const char * opt_staged;  /* raw -S schedule string (e.g. "r2i6q"), or 0;
                                    parse_schedule() expands it into opt_stages[] */
/* Upper bound on -R, purely a sanity guard against a typo (each restart just
   re-runs the hill-climb from a fresh perturbed board -- no extra memory -- so the
   only real limit is patience). One billion is effectively unlimited for any real
   run yet stays well within int; raise it if you ever need more. */
static const int max_restarts = 1000000000;
static const int pairs_uncapped = asize / 2;   /* 13: a board holds at most this */

/* A parsed -S schedule is an ordered list of climb stages -- each a scoring model
   and a cap on the plug pairs it may set -- plus the per-restart random
   perturbation strength. Tokens are <letter><optional number>: model letters
   i/m/b/t/q (a stage, number = its pair cap, omitted = uncapped) and r (the random
   perturbation, number = plug pairs injected per restart, omitted = default_perturb).
   The last model stage is the target/ranking model. With no r token (including no -S
   at all) the perturbation is a fixed default_perturb plug pairs -- the sweep's best
   kick, near the typical plug count (CODE_REVIEW §9). */
static const int max_stages = 16;
struct climbstage
{
  int model;   /* SCORE_* */
  int cap;     /* max plug pairs this stage may set (1..13; 13 = uncapped) */
};
static struct climbstage opt_stages[max_stages];
static int opt_nstages;    /* number of model stages in opt_stages[] */
static int opt_perturb;    /* random plug pairs injected per restart: 0..13 (an rN
                              token; default_perturb when no r token; r0 = no-op) */
static const int default_perturb = 8;   /* no-r-token kick: best in the §9 sweep */
static int opt_prefilter; /* key pre-filter: rank all keys by a cheap IC climb, then
                             run the full -c climb on only the top opt_prefilter keys
                             (0 = off; requires -c) */
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
  /* Per character position, the 26-byte rotor-stack substitution row. The
     scorers read rows[i][k]; rows[i] either points straight into the shared
     subst_array (the brute-force scan -- no copy) or into the contiguous
     mapping[] below (hill-climbing, which re-reads each row hundreds of times
     and benefits from the locality). */
  const unsigned char * rows[maxlen];
  unsigned char mapping[maxlen][asize]; /* hill-climb's contiguous row copies */
  char plaintext[maxlen+1];             /* candidate / result */

  int ukw;                              /* reflector index (thin UKW-b/c in M4) */
  int walzenlage[wheels];               /* wheel order (rotor indices) */
  unsigned char grundstellung[wheels];  /* start positions */
  unsigned char ringstellung[wheels];   /* ring positions */

  /* effective reflector actually applied by subst_rotors: a plain copy of
     reflector[ukw] normally, or greek-folded thin reflector in M4 mode (set once
     per task by set_effective_reflector, before precompute -- never in the hot
     loop, which reads the precomputed subst_array). */
  unsigned char reflector_eff[asize];
  int greek;          /* M4 Greek rotor index (Beta/Gamma), else -1 */
  int greek_offset;   /* M4 (Greek start - ring) mod 26, else 0 */
  int scoring;        /* active scoring model (= opt_scoring; varied per stage by
                         the staged plugboard climb, so it is per-machine not the
                         global -- a shared global would race across worker threads) */

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

  char filename[1024];
  int len = snprintf(filename, sizeof(filename), "%s/%s_%s.txt",
                     opt_datadir, opt_language, suffix);
  if ((len < 0) || (len >= static_cast<int>(sizeof(filename))))
    fatal("Data directory / language path too long");

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

  x = m.reflector_eff[x];

  for(int r = 0; r < wheels; r++)
    x = rotor_r(m, x, r);

  return x;
}

/* Resolve the effective reflector for this machine's reflector / Greek wheel.
   Called once per task (before precompute), never per character. Standard and
   Norway modes just copy the wired reflector. In M4 the static Greek wheel folds
   into the thin reflector: the signal passes through the Greek wheel (at its fixed
   offset, forward like rotor_l), the thin reflector, then back through the Greek
   wheel (reverse like rotor_r). greek o thin o greek^-1 is the conjugate of an
   involution, so it is still a valid (involutory) reflector. */
void set_effective_reflector(machine & m)
{
  if (! opt_m4)
    {
      memcpy(m.reflector_eff, reflector[m.ukw], asize);
      return;
    }

  const int o = m.greek_offset;
  const unsigned char * thin = reflector[m.ukw];      /* ukw = thin index 4/5 */
  const unsigned char * gf = rotor_fwd[m.greek];
  const unsigned char * gr = rotor_rev[m.greek];
  for (int x = 0; x < asize; x++)
    {
      int a = mod26(gf[mod26(x + o)] - o);   /* Greek forward  (rotor_l style) */
      int b = thin[a];                       /* thin reflector                 */
      int c = mod26(gr[mod26(b + o)] - o);   /* Greek reverse  (rotor_r style) */
      m.reflector_eff[x] = static_cast<unsigned char>(c);
    }
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

/* Step the rotors over the message and record, per character position, a pointer
   to its rotor-stack substitution row (which depends only on the start-minus-ring
   offsets, so it is the same for all 26 input letters).

   The scan (no plugboard hill-climb) reads each position's row at most a couple
   of times and only ever at the single index steckerbrett[ciphertext[i]], so it
   just points rows[i] straight into the shared subst_array -- no per-position
   copy. Hill-climbing re-reads each row hundreds of times at varying indices as
   it permutes the plugboard, so with `copy_rows` it copies the 26-byte row into
   the contiguous mapping[] (better locality across the climb) and points there.

   The stepping state is held in plain locals for the duration of the loop rather
   than in m.grundstellung: the previous per-character read/modify/write through
   the struct could not be proven not to alias the stores, which serialised the
   loop and cost ~10-14% on the search path (worst on ARM). Locals let the
   compiler keep the rotor positions in registers; the final positions are
   written back once at the end. */
void setup_mapping(machine & m, bool copy_rows)
{
  if (textlength > maxlen)
    fatal("Ciphertext too long");

  const unsigned char (* __restrict sa)[asize][asize][asize] = m.subst_array;
  const unsigned char ** __restrict rows = m.rows;
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
      if (copy_rows)
        {
          memcpy(m.mapping[i], row, asize);
          rows[i] = m.mapping[i];
        }
      else
        rows[i] = row;
    }

  m.grundstellung[0] = static_cast<unsigned char>(g0);
  m.grundstellung[1] = static_cast<unsigned char>(g1);
  m.grundstellung[2] = static_cast<unsigned char>(g2);
}

/* Decode one ciphertext position: plugboard -> per-position rotor-stack row ->
   plugboard. A tiny inline so decode() and the scorers share one copy of the
   formula. rows[i] is the position's substitution row (in subst_array for the
   scan, or the contiguous mapping[] for hill-climb). The scorers fuse this into
   their loops (see below) so the decoded text is never materialised in a scratch
   array. The base pointers are __restrict locals the callers have hoisted out of
   struct machine. */
inline int decode_at(const unsigned char * __restrict steck,
                     const unsigned char * const * __restrict rows,
                     const unsigned char * __restrict ct,
                     int i)
{
  return steck[rows[i][steck[ct[i]]]];
}

inline void decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  char * __restrict pt = m.plaintext;
  for (int i = 0; i < textlength; i++)
    pt[i] = num2char(decode_at(steck, rows, ct, i));
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
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 4)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  int c = decode_at(steck, rows, ct, 2);
  for (int i = 3; i < textlength; i++)
    {
      int d = decode_at(steck, rows, ct, i);
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
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 3)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  for (int i = 2; i < textlength; i++)
    {
      int c = decode_at(steck, rows, ct, i);
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
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 2)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  for (int i = 1; i < textlength; i++)
    {
      int b = decode_at(steck, rows, ct, i);
      score += bigrams[a][b];
      a = b;
    }
  return score;
}

double monogram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  for (int i = 0; i < textlength; i++)
    score += monograms[decode_at(steck, rows, ct, i)];
  return score;
}

double ic_score_decode(machine & m)
{
  int freq[asize];
  for(int j=0; j<asize; j++)
    freq[j] = 0;

  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  for (int i = 0; i < textlength; i++)
    freq[decode_at(steck, rows, ct, i)]++;

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

  if (opt_m4)
    {
      /* M4: thin reflector (b/c) + static Greek wheel (B/G). Only the Greek
         (start - ring) offset is identifiable, so it is shown as start=offset,
         ring=A. The reflector/Greek/ring/start fields list the Greek first. */
      fprintf(stderr,
              "W: %c%c%d%d%d R: A%c%c%c G: %c%c%c%c ",
              (m.ukw == m4_thin_base) ? 'b' : 'c',
              (m.greek == greek_base) ? 'B' : 'G',
              m.walzenlage[0] + 1,
              m.walzenlage[1] + 1,
              m.walzenlage[2] + 1,
              num2char(m.ringstellung[0]),
              num2char(m.ringstellung[1]),
              num2char(m.ringstellung[2]),
              num2char(m.greek_offset),
              num2char(m.grundstellung[0]),
              num2char(m.grundstellung[1]),
              num2char(m.grundstellung[2]));
      showsteckerbrett(m);
      fprintf(stderr, "\n");
      return;
    }

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

  switch(m.scoring)
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

/* Last-resort "re-pair" move: take two existing plugs {a-x},{b-y} to the OTHER
   pairing of their four letters ({a-b,x-y} or {a-y,x-b}), keeping the plug count. A
   single switch cannot reach these (it would first drop to one plug, often a worse
   intermediate the greedy climb never takes), so this crosses a barrier two single
   moves cannot. It is run only once the cheap swap/remove moves have converged -- a
   handful of times per climb, not every pass -- so its O(plugs^2) cost is small.
   Applies and returns true iff the single best re-pair strictly beats cur_score. */
static bool try_repair(machine & m, int iter, double cur_score)
{
  int plo[asize / 2];
  int phi[asize / 2];
  int np = 0;
  for (int a = 0; a < asize; a++)
    if (m.steckerbrett[a] > a)
      {
        plo[np] = a;
        phi[np] = m.steckerbrett[a];
        np++;
      }

  double best = cur_score;
  int rp_pos[4] = { 0, 0, 0, 0 };
  int rp_val[4] = { 0, 0, 0, 0 };
  bool found = false;

  for (int i = 0; i < np; i++)
    for (int j = i + 1; j < np; j++)
      {
        int a = plo[i], x = phi[i], b = plo[j], y = phi[j];

        /* M1: {a-b, x-y} */
        m.steckerbrett[a] = b; m.steckerbrett[b] = a;
        m.steckerbrett[x] = y; m.steckerbrett[y] = x;
        double s1 = score_iter(m, iter);
        if (s1 > best)
          {
            best = s1; found = true;
            rp_pos[0] = a; rp_val[0] = b; rp_pos[1] = b; rp_val[1] = a;
            rp_pos[2] = x; rp_val[2] = y; rp_pos[3] = y; rp_val[3] = x;
          }

        /* M2: {a-y, x-b} */
        m.steckerbrett[a] = y; m.steckerbrett[y] = a;
        m.steckerbrett[x] = b; m.steckerbrett[b] = x;
        double s2 = score_iter(m, iter);
        if (s2 > best)
          {
            best = s2; found = true;
            rp_pos[0] = a; rp_val[0] = y; rp_pos[1] = y; rp_val[1] = a;
            rp_pos[2] = x; rp_val[2] = b; rp_pos[3] = b; rp_val[3] = x;
          }

        /* restore {a-x, b-y} */
        m.steckerbrett[a] = x; m.steckerbrett[x] = a;
        m.steckerbrett[b] = y; m.steckerbrett[y] = b;
      }

  if (found)
    for (int k = 0; k < 4; k++)
      m.steckerbrett[rp_pos[k]] = static_cast<unsigned char>(rp_val[k]);
  return found;
}

/* Climb the steckerbrett for the current scoring model until no move improves it,
   but never letting the board exceed max_pairs plug pairs (the staged climb caps the
   low-order pre-pass to its first few plugs; pass pairs_uncapped for an unconstrained
   climb -- a board can hold at most 13 pairs anyway). The cheap "switch" and "remove"
   moves are run to convergence; then a single best "re-pair" is tried as a barrier
   cross, and if it improves the cheap climb resumes from the new board. */
double hillclimb(machine & m, int max_pairs)
{
  int iter = 1;

  bool progress;
  do
    {
      progress = false;

      double best_score;
      double last_best;

      /* Cheap moves to convergence: each pass takes the single best of all "switch
         a-b" moves (force a-b, ejecting conflicts -- adds / moves an endpoint /
         merges two plugs into one) and all "remove" moves (free an existing pair). */
      do
        {
          best_score = score_iter(m, iter);
          last_best = best_score;

          /* current plug-pair count: at the cap, moves that would add a brand-new
             pair (both endpoints currently unplugged) are skipped below */
          int pairs = 0;
          for (int j = 0; j < asize; j++)
            if (m.steckerbrett[j] > j)
              pairs++;

          double move_score = best_score;
          int move_kind = 0;        /* 0 = switch, 1 = remove */
          int move_a = 0;
          int move_b = 0;

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
                /* at the cap, do not add a brand-new pair (both ends unplugged) */
                if ((pairs >= max_pairs) &&
                    (m.steckerbrett[a] == a) && (m.steckerbrett[b] == b))
                  continue;

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

                if (score > move_score)
                  {
                    move_score = score;
                    move_kind = 0;
                    move_a = a;
                    move_b = b;
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

          /* Removal moves: drop an existing plug pair, freeing both ends. The switch
             moves can add, re-pair or merge plugs but never simply delete one, so a
             staged climb that moved to a sharper model cannot shed a plug the previous
             model set without this. At most 13 pairs are plugged, so it is cheap. */
          for(int a=0; a<asize; a++)
            if (m.steckerbrett[a] > a)
              {
                int b = m.steckerbrett[a];
                m.steckerbrett[a] = a;
                m.steckerbrett[b] = b;

                double score = score_iter(m, iter);

                if (score > move_score)
                  {
                    move_score = score;
                    move_kind = 1;
                    move_a = a;
                    move_b = b;
                  }

                m.steckerbrett[a] = b;
                m.steckerbrett[b] = a;
              }

          if (move_score - best_score > 0)
            {
              int a = move_a;
              int b = move_b;

              if (move_kind == 1)
                {
                  /* remove the a-b plug, freeing both ends */
                  m.steckerbrett[a] = a;
                  m.steckerbrett[b] = b;
                }
              else
                {
                  /* switch plugs */
                  int x = m.steckerbrett[a];
                  int y = m.steckerbrett[b];
                  m.steckerbrett[x] = x;
                  m.steckerbrett[y] = y;
                  m.steckerbrett[a] = b;
                  m.steckerbrett[b] = a;
                }

#ifdef SHOWHILLCLIMB
              fprintf(stderr,
                      "%2d %s Imp: %10.4f Score: %10.4f ",
                      iter,
                      move_kind == 1 ? "del" : "set",
                      move_score - best_score,
                      move_score);
              showsteckerbrett(m);
              fprintf(stderr, "\n");
#endif

              best_score = move_score;
            }

          iter++;
        }
      while (best_score > last_best);

      /* Cheap moves converged: one last-resort re-pair barrier cross. If it
         improves, loop back and let the cheap climb resume from the new board. */
      if (try_repair(m, iter, best_score))
        progress = true;
      iter++;
    }
  while (progress);

  decode(m);

#ifdef SHOWHILLCLIMB
  printf("Plaintext: %s\n", m.plaintext);
#endif
  return score_iter(m, 0);
}

/* splitmix64: a tiny, well-distributed deterministic PRNG. Seeded per key (not
   from the clock or thread id) so a random-restart search stays reproducible and
   independent of the thread count. */
static inline uint64_t splitmix64(uint64_t * s)
{
  uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

/* Inject exactly k random plug pairs into the current plugboard, drawing only from
   letters that are still unplugged (so any fixed -s pairs are preserved). This is
   the per-restart perturbation: a kick of k random plugs (default_perturb, or an rN
   token) into a new basin, near the typical plug count so the staged climb need not
   tear down a near-saturated board (CODE_REVIEW §9). With k=0 it is a no-op (so r0
   makes restarts identical -- a useful control). */
void perturb_steckerbrett(machine & m, uint64_t * rng, int k)
{
  unsigned char freelet[asize];
  int nfree = 0;
  for (int i = 0; i < asize; i++)
    if (m.steckerbrett[i] == i)
      freelet[nfree++] = static_cast<unsigned char>(i);

  int want = k * 2;                  /* free letters to pair up */
  if (want > nfree)
    want = nfree - (nfree & 1);      /* clamp to an even count of free letters */

  /* partial Fisher-Yates: shuffle the first `want` free letters, then pair them */
  for (int i = 0; i < want; i++)
    {
      int j = i + static_cast<int>(splitmix64(rng) %
                                   static_cast<uint64_t>(nfree - i));
      unsigned char t = freelet[i]; freelet[i] = freelet[j]; freelet[j] = t;
    }
  for (int i = 0; i + 1 < want; i += 2)
    {
      int a = freelet[i], b = freelet[i + 1];
      m.steckerbrett[a] = static_cast<unsigned char>(b);
      m.steckerbrett[b] = static_cast<unsigned char>(a);
    }
}

/* Load the n-gram table backing a scoring model (IC needs none). */
void load_table(int model)
{
  switch (model)
    {
    case SCORE_MONO: ngrams_read(1, monograms, "monograms"); break;
    case SCORE_BI:   ngrams_read(2, & bigrams[0][0], "bigrams"); break;
    case SCORE_TRI:  ngrams_read(3, & trigrams[0][0][0], "trigrams"); break;
    case SCORE_QUAD: ngrams_read(4, & quadgrams[0][0][0][0], "quadgrams"); break;
    default: break;   /* IC: no table */
    }
}

/* Map a scoring-model letter (i/m/b/t/q) to its SCORE_* value. */
int model_of(char c)
{
  switch (c)
    {
    case 'i': return SCORE_IC;
    case 'm': return SCORE_MONO;
    case 'b': return SCORE_BI;
    case 't': return SCORE_TRI;
    case 'q': return SCORE_QUAD;
    default:  return SCORE_IC;
    }
}

/* Parse the -S schedule string into opt_stages[]/opt_nstages/opt_perturb, and set
   opt_scoring to the target (last model stage). Tokens are <letter><optional int>:
   model letters i/m/b/t/q (a climb stage; the number caps its plug pairs, omitted =
   uncapped) and r (the random perturbation; the number is plug pairs injected per
   restart, omitted = the default_perturb kick). On a syntax error it calls fatal().
   With no -S the schedule is the single -i/-m/.../-q target, uncapped; with no r
   token the perturbation stays at the fixed default_perturb kick. */
void parse_schedule()
{
  opt_nstages = 0;
  opt_perturb = default_perturb;   /* fixed kick unless an r token overrides it */

  if (! opt_staged)
    {
      opt_stages[0].model = opt_scoring;
      opt_stages[0].cap = pairs_uncapped;
      opt_nstages = 1;
      return;
    }

  bool seen_r = false;
  for (const char * p = opt_staged; *p; )
    {
      char letter = *p++;
      int n = -1;                       /* -1 = no explicit number */
      if (isdigit(static_cast<unsigned char>(*p)))
        {
          n = 0;
          while (isdigit(static_cast<unsigned char>(*p)))
            {
              n = n * 10 + (*p++ - '0');
              if (n > pairs_uncapped)
                break;                  /* range-checked below; avoid overflow */
            }
        }

      if (letter == 'r')
        {
          if (seen_r)
            fatal("Illegal -S schedule: at most one r (random) token");
          seen_r = true;
          if (n > pairs_uncapped)
            fatal("Illegal -S r token (0 to 13 random plug pairs)");
          /* omitted number = the default kick, just as a bare model token is
             uncapped; rN sets a fixed N (r0 = a no-op control) */
          opt_perturb = (n < 0) ? default_perturb : n;
        }
      else if (strchr("imbtq", letter))
        {
          if (opt_nstages >= max_stages)
            fatal("Illegal -S schedule: too many stages (max 16)");
          int cap = (n < 0) ? pairs_uncapped : n;
          if ((cap < 1) || (cap > pairs_uncapped))
            fatal("Illegal -S stage cap (1 to 13 plug pairs; omit for no cap)");
          opt_stages[opt_nstages].model = model_of(letter);
          opt_stages[opt_nstages].cap = cap;
          opt_nstages++;
        }
      else
        fatal("Illegal -S schedule (tokens are r/i/m/b/t/q + optional number, "
              "e.g. -S r2i6q)");
    }

  if (opt_nstages < 1)
    fatal("Illegal -S schedule: needs at least one model stage (i/m/b/t/q)");

  /* the last model stage is the target/ranking model */
  opt_scoring = opt_stages[opt_nstages - 1].model;
}

/* Staged plugboard climb: run each schedule stage in order, capping the plug pairs
   it may set. A lower-order model has a far smoother surface when only a plug or two
   are set, so an early stage steers the first plugs into a good basin that a
   single-model climb navigates poorly -- staging reshapes the *search* landscape
   (complementary to random restarts). The returned score and m.plaintext are in the
   target (last) model, so cross-key comparison is unaffected. With no -S this is a
   single uncapped climb in the -i/-m/.../-q model. */
double hillclimb_schedule(machine & m)
{
  double s = 0.0;
  for (int i = 0; i < opt_nstages; i++)
    {
      m.scoring = opt_stages[i].model;
      s = hillclimb(m, opt_stages[i].cap);
    }
  return s;   /* opt_nstages >= 1, so s is the target-model score */
}

/* Hill-climb the plugboard with optional random restarts: restart 0 uses the
   configured seed (identity or -s pairs) -- exactly the opt_restarts==1 behaviour
   -- then opt_restarts-1 further climbs start from the seed plus opt_perturb random
   plugs (a moderate kick, near the typical plug count), keeping the best. The
   rotor-stack mapping[] depends only on the key (not the plugboard), so it is reused
   across restarts; only the steckerbrett is reset each time. The RNG is seeded from
   the flat key index, so the result is independent of -T. Each start runs the staged
   climb. */
double hillclimb_restarts(machine & m, uint64_t key_index)
{
  double best = hillclimb_schedule(m);
  if (opt_restarts <= 1)
    return best;

  char best_pt[maxlen + 1];
  memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);

  uint64_t rng = key_index + 0x0123456789abcdefULL;
  for (int r = 1; r < opt_restarts; r++)
    {
      init_steckerbrett(m, opt_steckerbrett);   /* reset to the fixed seed */
      perturb_steckerbrett(m, & rng, opt_perturb);
      double s = hillclimb_schedule(m);
      if (s > best)
        {
          best = s;
          memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
        }
    }
  memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
  return best;
}




/* The reflector x wheel-order combinations are the unit of parallelism: each is
   independent (its own precompute + ring/start sweep). The ring/start ranges are
   identical for every task. */
struct wheel_task
{
  int u;
  int w[wheels];
  int greek;        /* M4 Greek rotor index, else -1 */
  int greek_off;    /* M4 (Greek start - ring) mod 26, else 0 */
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
      m.greek = t.greek;
      m.greek_offset = t.greek_off;
      set_effective_reflector(m);   /* fold in the Greek wheel (M4) once per task */
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

  m.scoring = opt_scoring;   /* per-machine; the staged climb varies it transiently */

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
              m.greek = t.greek;            /* for showconfig of a new best (M4) */
              m.greek_offset = t.greek_off;
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
          /* hill-climb re-reads each row many times -> copy into contiguous
             mapping[]; the scan reads straight from the shared subst_array */
          setup_mapping(m, opt_hillclimb != 0);

          /* Score directly. The scan does not decode the plaintext per key
             (the fused scorer reads each row once, straight from subst_array);
             it is materialised only when a new best is recorded, below.
             hillclimb() leaves m.plaintext set to its best plugboard's decode. */
          double score = opt_hillclimb ? hillclimb_restarts(m, idx)
                                       : score_iter(m, 0);

          if (score > local_best)
            {
              local_best = score;
              std::lock_guard<std::mutex> lock(best.mutex);
              if (score > best.score)
                {
                  if (! opt_hillclimb)
                    decode(m);   /* fill m.plaintext for this winning key */
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

/* --- key pre-filter (-F) ---------------------------------------------------

   With -c, the full plugboard climb (-R restarts x -S stages) is paid on *every*
   key. The pre-filter instead ranks all keys by a single cheap index-of-coincidence
   climb -- which, unlike a plugboard-free IC scan, partially recovers the stecker
   and so discriminates the true rotor key even under a full 10-pair board -- and
   then runs the expensive climb only on the top -F keys. */

/* Decode a flat key index and configure the machine for it: switch to the wheel
   order's table only when it changes from cur_wo, set ring/start, reset the
   plugboard and build mapping[]. Fills rg6 = {r1,r2,r3,g1,g2,g3} for showconfig. */
static void key_to_machine(machine & m, size_t idx,
                           const std::vector<wheel_task> & tasks,
                           const search_range & range, const int * rc, const int * gc,
                           subst_table all, size_t rg, size_t gsize,
                           size_t rc12, size_t gc12, size_t & cur_wo, int rg6[6])
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
      m.greek = t.greek;
      m.greek_offset = t.greek_off;
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
  setup_mapping(m, true);   /* both the filter climb and the finish climb need it */
  rg6[0] = r1; rg6[1] = r2; rg6[2] = r3; rg6[3] = g1; rg6[4] = g2; rg6[5] = g3;
}

struct scored_key { double score; size_t idx; };

/* A min-heap that keeps the top-N keys: top() is the eviction candidate -- the
   lowest score, ties broken by the largest idx (so equal scores keep the lower idx,
   which makes the kept set deterministic and -T-independent). */
struct keep_worse
{
  bool operator()(const scored_key & a, const scored_key & b) const
  {
    if (a.score != b.score)
      return a.score > b.score;   /* top() = smallest score */
    return a.idx < b.idx;         /* tie: top() = largest idx */
  }
};

/* Tier 1: rank a slice of the flat key space by a cheap IC climb; keep the
   thread-local top-N, then merge into the shared candidate list. No printing. */
void filter_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range, const int * rc, const int * gc,
                   subst_table all, size_t rsize, size_t gsize,
                   std::atomic<size_t> & next_key, size_t chunk, size_t topn,
                   std::mutex & cand_mutex, std::vector<scored_key> & cand)
{
  const size_t rg = rsize * gsize;
  const size_t total = tasks.size() * rg;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];

  m.scoring = SCORE_IC;   /* the cheap, smooth-surface filter model */

  std::priority_queue<scored_key, std::vector<scored_key>, keep_worse> heap;
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      size_t end = start + chunk;
      if (end > total)
        end = total;

      for (size_t idx = start; idx < end; idx++)
        {
          key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                         rc12, gc12, cur_wo, rg6);
          double s = hillclimb(m, pairs_uncapped);   /* single IC climb */

          if (heap.size() < topn)
            heap.push(scored_key{s, idx});
          else
            {
              const scored_key & w = heap.top();
              if ((s > w.score) || ((s == w.score) && (idx < w.idx)))
                {
                  heap.pop();
                  heap.push(scored_key{s, idx});
                }
            }
        }
    }

  std::lock_guard<std::mutex> lock(cand_mutex);
  while (! heap.empty())
    {
      cand.push_back(heap.top());
      heap.pop();
    }
}

/* Tier 2: run the full -R/-S plugboard climb on the shortlisted keys only, merging
   the global best exactly like search_worker's hill-climb path. */
void finish_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range, const int * rc, const int * gc,
                   subst_table all, size_t rsize, size_t gsize,
                   const std::vector<size_t> & shortlist,
                   std::atomic<size_t> & next, best_result & best)
{
  const size_t rg = rsize * gsize;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];

  m.scoring = opt_scoring;

  double local_best = score_min;
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];

  size_t k;
  while ((k = next.fetch_add(1)) < shortlist.size())
    {
      size_t idx = shortlist[k];
      key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                     rc12, gc12, cur_wo, rg6);

      double score = hillclimb_restarts(m, idx);

      if (score > local_best)
        {
          local_best = score;
          std::lock_guard<std::mutex> lock(best.mutex);
          if (score > best.score)
            {
              best.score = score;
              best.found = true;
              memcpy(best.plaintext, m.plaintext, textlength + 1);
              init_ring_grund(m, rg6[0], rg6[1], rg6[2], rg6[3], rg6[4], rg6[5]);
              fprintf(stderr, "%10.4f ", score);
              showconfig(m);
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
  else if (opt_m4)
    {
      /* thin reflector index: B -> m4_thin_base (UKW-b), C -> +1 (UKW-c) */
      if (opt_ukw[0] == '.')
        {
          u_min = m4_thin_base;
          u_max = m4_thin_base + 1;
        }
      else
        u_min = u_max = m4_thin_base + char2num(opt_ukw[0]) - 1;
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

  /* M4 adds two outer dimensions: the Greek wheel (Beta/Gamma) and its fixed
     offset. Only the (start - ring) offset of the static Greek wheel is
     identifiable, so the pos/ring ranges collapse to the set of distinct offsets
     (<= 26, not 26x26). Non-M4 searches use the single sentinels {-1} / {0}. */
  std::vector<int> greek_list;
  std::vector<int> offset_list;
  if (opt_m4)
    {
      if (opt_greek_walzen == '.')
        {
          greek_list.push_back(greek_base);
          greek_list.push_back(greek_base + 1);
        }
      else
        greek_list.push_back(greek_base + (opt_greek_walzen == 'B' ? 0 : 1));

      int gp_min, gp_max, gr_min, gr_max;
      if (opt_greek_grundstellung == '.') { gp_min = 0; gp_max = 25; }
      else gp_min = gp_max = char2num(opt_greek_grundstellung);
      if (opt_greek_ringstellung == '.') { gr_min = 0; gr_max = 25; }
      else gr_min = gr_max = char2num(opt_greek_ringstellung);

      bool seen[asize];
      for (int i = 0; i < asize; i++)
        seen[i] = false;
      for (int gp = gp_min; gp <= gp_max; gp++)
        for (int gr = gr_min; gr <= gr_max; gr++)
          seen[mod26(gp - gr)] = true;
      for (int off = 0; off < asize; off++)   /* ascending: deterministic order */
        if (seen[off])
          offset_list.push_back(off);
    }
  else
    {
      greek_list.push_back(-1);
      offset_list.push_back(0);
    }

  std::vector<wheel_task> tasks;
  for (int u1 = u_min; u1 <= u_max; u1++)
    for (int gi : greek_list)
      for (int off : offset_list)
        for (int w1 = w_min[0]; w1 <= w_max[0]; w1++)
          for (int w2 = w_min[1]; w2 <= w_max[1]; w2++)
            for (int w3 = w_min[2]; w3 <= w_max[2]; w3++)
              if ((w1 != w2) && (w1 != w3) && (w2 != w3))
                tasks.push_back(wheel_task{u1, {w1, w2, w3}, gi, off});

  /* The option validation should make this unreachable, but never run an empty
     search and emit uninitialised output. */
  if (tasks.empty())
    fatal("No machine configuration was searched "
          "(check the -u / -w / -x settings)");

  size_t nwo = tasks.size();
  size_t total_keys = nwo * rsize * gsize;

  /* memory accounting: one [asize]^4 (457 KB) table per task (reflector x wheel
     order, plus the Greek wheel x offset in M4), all resident. The biggest
     possible search is a full M4 wildcard (2 thin x 2 Greek x 26 offsets x 336
     wheel orders = 34 944 tasks ~= 14.9 GiB); every other mode is far smaller. No
     fixed ceiling is enforced -- the allocation below just fails gracefully if the
     machine cannot provide the memory. */
  g_table_count = nwo;
  g_table_bytes = nwo * static_cast<size_t>(asize) * asize * asize * asize;

  /* never start more threads than there is work to hand out */
  int nthreads = opt_threads;
  if (total_keys < static_cast<size_t>(nthreads))
    nthreads = static_cast<int>(total_keys);
  if (nthreads < 1)
    nthreads = 1;

  /* the shared, read-only rotor-stack tables (one [asize]^4 block per wheel
     order) and the per-thread machines (small: mapping/plaintext/settings). A
     clean message beats a std::terminate if the allocator refuses the block
     (note: under Linux memory overcommit a too-large request may instead succeed
     here and be OOM-killed later while precompute touches the pages). */
  subst_table all;
  try
    {
      all = new unsigned char[nwo * asize][asize][asize][asize];
    }
  catch (const std::bad_alloc &)
    {
      char msg[160];
      snprintf(msg, sizeof msg,
               "Could not allocate %.1f GB for the rotor tables "
               "(narrow -u / -w / -x, or fix the M4 Greek wheel/position)",
               g_table_bytes / 1e9);
      fatal(msg);
    }

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
  size_t chunk = total_keys / (static_cast<size_t>(nthreads) * 16);
  if (chunk < 1)
    chunk = 1;

  if (opt_prefilter > 0)
    {
      /* Tier 1: rank every key by a cheap IC climb, keep the top -F. */
      size_t topn = static_cast<size_t>(opt_prefilter);
      if (topn > total_keys)
        topn = total_keys;

      std::vector<scored_key> cand;
      std::mutex cand_mutex;
      std::atomic<size_t> fnext{0};
      if (nthreads == 1)
        filter_worker(*machines[0], tasks, range, rc, gc, all, rsize, gsize,
                      fnext, chunk, topn, cand_mutex, cand);
      else
        {
          std::vector<std::thread> pool;
          pool.reserve(static_cast<size_t>(nthreads));
          for (int t = 0; t < nthreads; t++)
            pool.emplace_back(filter_worker, std::ref(*machines[t]),
                              std::cref(tasks), std::cref(range), rc, gc, all,
                              rsize, gsize, std::ref(fnext), chunk, topn,
                              std::ref(cand_mutex), std::ref(cand));
          for (std::thread & th : pool)
            th.join();
        }

      /* deterministic global top-N: highest score first, ties by lowest idx */
      std::sort(cand.begin(), cand.end(),
                [](const scored_key & a, const scored_key & b)
                {
                  if (a.score != b.score) return a.score > b.score;
                  return a.idx < b.idx;
                });
      if (cand.size() > topn)
        cand.resize(topn);
      std::vector<size_t> shortlist;
      shortlist.reserve(cand.size());
      for (const scored_key & sk : cand)
        shortlist.push_back(sk.idx);

      fprintf(stderr,
              "Pre-filter: ranked %zu keys by a cheap IC climb, "
              "running the full climb on the top %zu\n",
              total_keys, shortlist.size());

      /* Tier 2: full -R / -S climb on the shortlist only. */
      std::atomic<size_t> snext{0};
      if (nthreads == 1)
        finish_worker(*machines[0], tasks, range, rc, gc, all, rsize, gsize,
                      shortlist, snext, best);
      else
        {
          std::vector<std::thread> pool;
          pool.reserve(static_cast<size_t>(nthreads));
          for (int t = 0; t < nthreads; t++)
            pool.emplace_back(finish_worker, std::ref(*machines[t]),
                              std::cref(tasks), std::cref(range), rc, gc, all,
                              rsize, gsize, std::cref(shortlist),
                              std::ref(snext), std::ref(best));
          for (std::thread & th : pool)
            th.join();
        }
    }
  else
    {
      std::atomic<size_t> next_key{0};
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
  fprintf(out, "  -u X         Reflector (umkehrwalze) X (A-C, N, M4 b/c, or .) [.]\n");
  fprintf(out, "  -w XYZ       Wheels (walzen) XYZ (1-8 or .) [...]\n");
  fprintf(out, "  -x integer   Highest wheel number to use (3-8) [5]\n");
  fprintf(out, "  -n           Use the Norway Enigma reflector (N) and wheels (1-5)\n");
  fprintf(out, "  -4           M4 (4-rotor naval) mode: -u selects thin reflector b/c;\n");
  fprintf(out, "               -w/-r/-g take 4 chars, Greek wheel (B/G) / ring / start first\n");
  fprintf(out, "  -r XYZ       Ring positions (ringstellung) XYZ (A-Z or .) [AA.]\n");
  fprintf(out, "  -g XYZ       Start positions (grundstellung) XYZ (A-Z or .) [...]\n");
  fprintf(out, "  -s AB...     Plugboard (steckerbrett) letter pairs (A-Z pairs) [none]\n");
  fprintf(out, "  -c           Perform hill climbing to determine plugboard settings\n");
  fprintf(out, "  -R integer   Plugboard hill-climb random restarts (1 = none) [1]\n");
  fprintf(out, "  -S schedule  Staged plugboard climb: <letter><opt.number> tokens.\n");
  fprintf(out, "               Models i/m/b/t/q (number caps plug pairs; last = target),\n");
  fprintf(out, "               rN = per-restart random plugs (N pairs, default 8).\n");
  fprintf(out, "               E.g. -S r2i6q\n");
  fprintf(out, "  -l language  Scoring language (english, german, danish, french); required\n");
  fprintf(out, "               for -m/-b/-t/-q (no default), not used by -i\n");
  fprintf(out, "  -i           Use index of coincidence (IC) to determine plaintext score\n");
  fprintf(out, "  -m           Use monogram statistics to determine plaintext score\n");
  fprintf(out, "  -b           Use bigram statistics to determine plaintext score\n");
  fprintf(out, "  -t           Use trigram statistics to determine plaintext score\n");
  fprintf(out, "  -q           Use quadgram statistics to determine plaintext score [default]\n");
  fprintf(out, "  -p filename  Name of file containing plaintext to compare result with\n");
  fprintf(out, "  -F integer   Key pre-filter: rank keys by a cheap IC climb, then run\n");
  fprintf(out, "               the full -c climb on only the top N keys (needs -c) [off]\n");
  fprintf(out, "  -d directory Directory holding the n-gram files (or $ENIGMA_DATA) [.]\n");
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
    fprintf(stderr, " (language: %s; n-gram files in %s)",
            opt_language, opt_datadir);
  fprintf(stderr, "; plugboard hill-climb: %s", opt_hillclimb ? "yes" : "no");
  if (opt_hillclimb && (opt_restarts > 1))
    fprintf(stderr, " (%d restarts, %d-pair kick)", opt_restarts, opt_perturb);
  if (opt_hillclimb && opt_staged)
    fprintf(stderr, " (staged: %s)", opt_staged);
  if (opt_prefilter > 0)
    fprintf(stderr, "; pre-filter: top %d keys", opt_prefilter);
  fprintf(stderr, "; threads: %d\n", opt_threads);

  if (opt_m4)
    {
      /* opt_ukw is upper-cased (B/C); echo the thin reflector in lower case */
      fprintf(stderr,
              "Machine:    M4 Enigma; thin reflector %c, Greek wheel %c, wheels %s",
              (opt_ukw[0] == '.') ? '.' : (opt_ukw[0] == 'B' ? 'b' : 'c'),
              opt_greek_walzen, opt_walzen);
      if (strchr(opt_walzen, '.'))
        fprintf(stderr, " (max wheel %d)", opt_maxwheel);
      fprintf(stderr, ", Greek ring %c start %c, ring %s, start %s\n",
              opt_greek_ringstellung, opt_greek_grundstellung,
              opt_ringstellung, opt_grundstellung);
    }
  else
    {
      fprintf(stderr, "Machine:    %s Enigma; reflector %s, wheels %s",
              opt_norenigma ? "Norway" : "standard", opt_ukw, opt_walzen);
      if (strchr(opt_walzen, '.'))
        fprintf(stderr, " (max wheel %d)", opt_maxwheel);
      fprintf(stderr, ", ring %s, start %s\n",
              opt_ringstellung, opt_grundstellung);
    }

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

  /* set default arguments. The reflector/wheels/ring/start defaults depend on the
     mode (standard vs M4's extra Greek position), so they are left null here and
     resolved after parsing, once -4 is known. */
  opt_ukw = 0;
  opt_walzen = 0;
  opt_ringstellung = 0;
  opt_grundstellung = 0;
  opt_steckerbrett = "";
  opt_language = 0;   /* no default; required for n-gram scoring (-m/-b/-t/-q) */
  opt_datadir = 0;    /* resolved after parsing: -d > $ENIGMA_DATA > "." */
  opt_plaintext = 0;
  opt_maxwheel = 5;
  opt_hillclimb = 0;
  opt_restarts = 1;
  opt_staged = 0;   /* -S schedule string, or 0 for the single-model climb */
  opt_scoring = SCORE_QUAD;
  opt_norenigma = 0;
  opt_m4 = 0;
  opt_threads = 1;
  opt_prefilter = 0;

  /* get arguments */

  int c;
  while ((c = getopt(argc, argv, "u:w:r:g:s:p:l:x:T:R:S:F:d:imbtqcvhn4")) != -1)
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
        case 'S':
          opt_staged = optarg;
          break;
        case 'x':
          opt_maxwheel = atoi(optarg);
          break;
        case 'T':
          opt_threads = atoi(optarg);
          break;
        case 'R':
          opt_restarts = atoi(optarg);
          break;
        case 'F':
          opt_prefilter = atoi(optarg);
          break;
        case 'l':
          opt_language = optarg;
          break;
        case 'd':
          opt_datadir = optarg;
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
        case '4':
          opt_m4 = 1;
          break;
        default:
          fprintf(stderr, "\n");
          help(stderr);
          exit(1);
          break;
        }
    }

  if (opt_norenigma && opt_m4)
    fatal("-n (Norway) and -4 (M4) are mutually exclusive");

  /* resolve the mode-dependent reflector/wheels/ring/start defaults now that the
     mode flags are known. M4 takes a 4th (Greek) character first in -w/-r/-g; the
     Greek ring defaults to A and its position is wildcarded, so all 26 effective
     reflectors are tried. */
  if (opt_m4)
    {
      if (! opt_ukw)           opt_ukw = ".";
      if (! opt_walzen)        opt_walzen = "....";
      if (! opt_ringstellung)  opt_ringstellung = "AAA.";
      if (! opt_grundstellung) opt_grundstellung = "....";
    }
  else
    {
      if (! opt_ukw)           opt_ukw = ".";
      if (! opt_walzen)        opt_walzen = "...";
      if (! opt_ringstellung)  opt_ringstellung = "AA.";
      if (! opt_grundstellung) opt_grundstellung = "...";
    }

  /* resolve the n-gram data directory: -d wins, else $ENIGMA_DATA, else the
     current directory (the historical behaviour) */
  if (! opt_datadir)
    opt_datadir = getenv("ENIGMA_DATA");
  if ((! opt_datadir) || (! opt_datadir[0]))
    opt_datadir = ".";

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
  else if (opt_m4)
    {
      /* M4: thin reflector (b/c, case-insensitive -> B/C), and a 4-character
         -w/-r/-g whose first character is the static Greek wheel/ring/start. */
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "BC.") != 1))
        fatal("Illegal ukw string (M4: must be b, c or .)");

      if (strlen(opt_walzen) != wheels + 1)
        fatal("Illegal walzen string (M4: 4 chars: Greek (B/G/.) + 3 wheels "
              "(1-8/.))");
      if (! strchr("BGbg.", opt_walzen[0]))
        fatal("Illegal Greek wheel (M4: must be B (Beta), G (Gamma) or .)");
      if (strspn(opt_walzen + 1, "12345678.") != wheels)
        fatal("Illegal walzen string (M4: the 3 wheels must be digits (1-8) "
              "or .)");

      if (strlen(opt_ringstellung) != wheels + 1)
        fatal("Illegal ringstellung string (M4: 4 letters (A-Z) or .)");
      if (strlen(opt_grundstellung) != wheels + 1)
        fatal("Illegal grundstellung string (M4: 4 letters (A-Z) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 8))
        fatal("Illegal max wheel (must be 3-8)");

      /* split off the Greek (first) char of -w/-r/-g; the remaining 3-character
         tails then pass through the shared checks below exactly like a standard
         machine. -r/-g were already upper-cased; normalise the Greek wheel. */
      opt_greek_walzen = (opt_walzen[0] == 'b') ? 'B'
                       : (opt_walzen[0] == 'g') ? 'G'
                       : opt_walzen[0];
      opt_greek_ringstellung = opt_ringstellung[0];
      opt_greek_grundstellung = opt_grundstellung[0];
      opt_walzen += 1;
      opt_ringstellung += 1;
      opt_grundstellung += 1;
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

  if ((opt_restarts < 1) || (opt_restarts > max_restarts))
    fatal("Illegal restart count (must be 1 to 1000000000)");

  /* Expand the -S schedule into opt_stages[]/opt_perturb and set opt_scoring to the
     target (last) stage. Validates the schedule syntax; fatal() on error. With no
     -S this builds the single -i/-m/.../-q stage. */
  parse_schedule();

  if ((opt_threads < 1) || (opt_threads > max_threads))
    fatal("Illegal thread count (must be 1 to 256)");

  /* The key pre-filter ranks every key by a cheap plugboard climb and runs the full
     climb only on the top -F keys, so it is only meaningful with -c. */
  if (opt_prefilter < 0)
    fatal("Illegal pre-filter count (-F must be >= 1)");
  if ((opt_prefilter > 0) && (! opt_hillclimb))
    fatal("The key pre-filter (-F) needs the plugboard hill-climb (-c)");

  /* Scoring only happens when the run ranks candidates -- a '.' wildcard in the
     reflector/wheels/ring/start -- or hill-climbs the plugboard (-c). A fully
     specified machine with no -c just enciphers its input: there is a single
     candidate and its decode is the output, so no score, and hence no scoring
     language, is needed. In that case fall back to the index of coincidence (which
     needs neither a table nor -l) so plain encryption/decryption works with no
     scoring options at all. (Note the default ring is "AA.", so an explicit -r is
     needed to encrypt -- otherwise the wildcard makes it a search.) */
  bool has_wildcard =
      strchr(opt_ukw, '.') || strchr(opt_walzen, '.') ||
      strchr(opt_ringstellung, '.') || strchr(opt_grundstellung, '.') ||
      (opt_m4 && (opt_greek_walzen == '.' ||
                  opt_greek_ringstellung == '.' ||
                  opt_greek_grundstellung == '.'));
  bool needs_scoring = has_wildcard || opt_hillclimb;
  if (! needs_scoring)
    opt_scoring = SCORE_IC;

  /* The n-gram scoring models (mono/bi/tri/quad) need a language, with no default;
     the index of coincidence (-i) and the r token are language-independent. Every
     stage that reads an n-gram table -- pre-pass or target -- needs -l. Only enforce
     this when scoring actually runs. */
  if (needs_scoring && ! opt_language)
    for (int i = 0; i < opt_nstages; i++)
      if (opt_stages[i].model != SCORE_IC)
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


  /* Load the n-gram tables scoring will use (none for IC), target first, so a
     missing or mistyped -l fails immediately (with the offending filename) before
     we read and consume standard input. Skipped entirely when just enciphering. */
  if (needs_scoring)
    {
      bool table_loaded[5] = { false, false, false, false, false };
      load_table(opt_scoring);
      table_loaded[opt_scoring] = true;
      for (int i = 0; i < opt_nstages; i++)
        {
          int model = opt_stages[i].model;
          if (! table_loaded[model])
            {
              load_table(model);
              table_loaded[model] = true;
            }
        }
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
