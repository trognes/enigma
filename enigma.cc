#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>

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

static const int blocksize = 16;   /* letters decoded per block in decode_num */

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
  int ukw;                              /* reflector index */
  int walzenlage[wheels];               /* wheel order (rotor indices) */
  unsigned char grundstellung[wheels];  /* start positions */
  unsigned char ringstellung[wheels];   /* ring positions */
  unsigned char steckerbrett[asize];    /* plugboard permutation */

  /* working tables, rebuilt as the search advances */
  unsigned char subst_array[asize][asize][asize][asize]; /* rotor stack per g-r */
  unsigned char mapping[maxlen][asize];                  /* per-position subst. */
  unsigned char num_plaintext[maxlen];                   /* decode scratch */
  char plaintext[maxlen+1];                              /* candidate / result */
};

static double monograms[asize];
static double bigrams[asize][asize];
static double trigrams[asize][asize][asize];
static double quadgrams[asize][asize][asize][asize];

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
void ngrams_read(int n, double * table, const char * suffix)
{
  int size = 1;
  for (int i = 0; i < n; i++)
    size *= asize;

  for (int i = 0; i < size; i++)
    table[i] = 1.0;

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

      table[index] = count + 1;
    }

  fclose(f);

  for (int i = 0; i < size; i++)
    table[i] = log10(table[i]);
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

inline void step_rotors(machine & m)
{
  if (notch[m.walzenlage[wheels-2]][m.grundstellung[wheels-2]])
    {
      m.grundstellung[wheels-3] = mod26(1+m.grundstellung[wheels-3]);
      m.grundstellung[wheels-2] = mod26(1+m.grundstellung[wheels-2]);
    }
  else if (notch[m.walzenlage[wheels-1]][m.grundstellung[wheels-1]])
    {
      m.grundstellung[wheels-2] = mod26(1+m.grundstellung[wheels-2]);
    }

  m.grundstellung[wheels-1] = mod26(1+m.grundstellung[wheels-1]);
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

  /* set up mapping trough the rotors for each character in the ciphertext.
     The rotor-stack row depends only on the (start - ring) offsets, which are
     constant across the alphabet, so resolve it once and copy the 26 bytes
     (also keeps the store to mapping[] from appearing to alias subst_array[]). */
  for (int i=0; i<textlength; i++)
    {
      step_rotors(m);
      const unsigned char * row = m.subst_array
        [mod26(m.grundstellung[0]-m.ringstellung[0])]
        [mod26(m.grundstellung[1]-m.ringstellung[1])]
        [mod26(m.grundstellung[2]-m.ringstellung[2])];
      memcpy(m.mapping[i], row, asize);
    }
}

inline int step_mapped(machine & m, int i, int x)
{
  return m.steckerbrett[m.mapping[i][m.steckerbrett[x]]];
}

inline void decode(machine & m)
{
  for (int i = 0; i < textlength; i++)
    m.plaintext[i] = num2char(step_mapped(m, i, num_ciphertext[i]));
  m.plaintext[textlength] = 0;
}

inline void map16_step(unsigned char * source,
                       unsigned char * map,
                       unsigned char * dest)
{
  for (int i = 0; i < blocksize; i++)
    dest[i] = map[asize*i+source[i]];
}

inline void map16_direct(unsigned char * source,
                         unsigned char * map,
                         unsigned char * dest)
{
  for (int i = 0; i < blocksize; i++)
    dest[i] = map[source[i]];
}

void showit(const char * msg, unsigned char * p)
{
  (void) msg; (void) p;   /* used only when the debug block below is enabled */
#if 0
  fprintf(stderr, "%s:", msg);
  for(int i=0; i<16; i++)
    fprintf(stderr, " %2d", p[i]);
  fprintf(stderr, "\n");
#endif
}

inline void decode_num(machine & m)
{
#if 0
  for (int i = 0; i < textlength; i++)
    m.num_plaintext[i] = step_mapped(m, i, num_ciphertext[i]);
#else
#if 0
  showit("cipher", num_ciphertext);
  for (int i = 0; i < textlength; i++)
    m.num_plaintext[i] =
      m.steckerbrett[m.mapping[i][m.steckerbrett[num_ciphertext[i]]]];
  showit("plain ", m.num_plaintext);
  fprintf(stderr, "\n");
#else

  int blocks = (textlength / blocksize) * blocksize;

  for (int i = 0; i < blocks; i += blocksize)
    {
      unsigned char temp1[blocksize];
      unsigned char temp2[blocksize];
      showit("cipher", num_ciphertext+i);
      map16_direct(num_ciphertext+i, m.steckerbrett, temp1);
      showit("steck ", temp1);
      map16_step(temp1, ((unsigned char *)(&m.mapping[i])), temp2);
      showit("mapped", temp2);
      map16_direct(temp2, m.steckerbrett, m.num_plaintext+i);
      showit("plain ", m.num_plaintext+i);
      //      fprintf(stderr, "\n");
    }

  /* remainder, when textlength is not a multiple of 16 (avoids reading
     mapping[] / num_ciphertext[] past textlength) */
  for (int i = blocks; i < textlength; i++)
    m.num_plaintext[i] = step_mapped(m, i, num_ciphertext[i]);

#endif
#endif
}

double quadgram_score_decode(machine & m)
{
  /* This decode and scoring function uses 99% of the computation time
     when hill-climbing. */

  decode_num(m);

  /*

  load triplet scores
    load 16 bytes at adr
    load 16 bytes at adr+1
    load 16 bytes at adr+2
    unpack low and high of each of these
    shift 0, 5 or 10 bits left
    add/or together
    gather from triplet score table

  */

  double score = 0.0;
  for (int i=0; i<textlength-3; i++)
    score += quadgrams[m.num_plaintext[i]][m.num_plaintext[i+1]]
      [m.num_plaintext[i+2]][m.num_plaintext[i+3]];
  return score;
}

double trigram_score_decode(machine & m)
{
  decode_num(m);

  double score = 0.0;
  for (int i=0; i<textlength-2; i++)
    score += trigrams[m.num_plaintext[i]][m.num_plaintext[i+1]][m.num_plaintext[i+2]];
  return score;
}

double bigram_score_decode(machine & m)
{
  decode_num(m);

  double score = 0.0;
  for (int i=0; i<textlength-1; i++)
    score += bigrams[m.num_plaintext[i]][m.num_plaintext[i+1]];
  return score;
}

double monogram_score_decode(machine & m)
{
  decode_num(m);

  double score = 0.0;
  for (int i = 0; i < textlength; i++)
    score += monograms[m.num_plaintext[i]];
  return score;
}

double ic_score_decode(machine & m)
{
  int freq[asize];
  for(int j=0; j<asize; j++)
    freq[j] = 0;

  for (int i = 0; i < textlength; i++)
    freq[step_mapped(m, i, num_ciphertext[i])]++;

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




void bruteforce(machine & m)
{
  int u_min, u_max;
  int w_min[wheels], w_max[wheels];
  int r_min[wheels], r_max[wheels];
  int g_min[wheels], g_max[wheels];

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
          r_min[i] = 0;
          r_max[i] = 25;
        }
      else
        {
          r_min[i] = r_max[i] = char2num(opt_ringstellung[i]);
        }

      if (opt_grundstellung[i] == '.')
        {
          g_min[i] = 0;
          g_max[i] = 25;
        }
      else
        {
          g_min[i] = g_max[i] = char2num(opt_grundstellung[i]);
        }
    }

  double best_score = score_min;
  char best_plaintext[maxlen+1];

  for (int u1 = u_min; u1 <= u_max; u1++)
    for (int w1 = w_min[0]; w1 <= w_max[0]; w1++)
      for (int w2 = w_min[1]; w2 <= w_max[1]; w2++)
        for (int w3 = w_min[2]; w3 <= w_max[2]; w3++)
          if ((w1 != w2) && (w1 != w3) && (w2 != w3))
            {
              init_walzen(m, u1, w1, w2, w3);

              precompute(m);

              for (int r1 = r_min[0]; r1 <= r_max[0]; r1++)
                for (int r2 = r_min[1]; r2 <= r_max[1]; r2++)
                  for (int r3 = r_min[2]; r3 <= r_max[2]; r3++)
                    for (int g1 = g_min[0]; g1 <= g_max[0]; g1++)
                      for (int g2 = g_min[1]; g2 <= g_max[1]; g2++)
                        for (int g3 = g_min[2]; g3 <= g_max[2]; g3++)
                          {
                            init_ring_grund(m, r1, r2, r3, g1, g2, g3);

                            init_steckerbrett(m, opt_steckerbrett);

                            setup_mapping(m);

                            double score;
                            if (opt_hillclimb)
                              {
                                score = hillclimb(m);
                              }
                            else
                              {
                                decode(m);
                                score = score_iter(m, 0);
                              }

                            if (score > best_score)
                              {
                                best_score = score;
                                memcpy(best_plaintext, m.plaintext, textlength + 1);
#if 1
                                init_ring_grund(m, r1, r2, r3, g1, g2, g3);
                                fprintf(stderr, "%10.4f ", score);
                                showconfig(m);
#endif
                              }
                          }
            }
  memcpy(m.plaintext, best_plaintext, textlength + 1);
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

void version()
{
  printf("Enigma cipher tool version 1.1.0\n");
  printf("Copyright (C) 2017-2026 Torbjørn Rognes\n");
  printf("\n");
}

void help()
{
  version();
  printf("Usage: enigma [OPTIONS]\n");
  printf("  -h           Show help information\n");
  printf("  -v           Show version information\n");
  printf("  -u X         Reflector (umkehrwalze) X (A-C, N or .) [.]\n");
  printf("  -w XYZ       Wheels (walzen) XYZ (1-8 or .) [...]\n");
  printf("  -x integer   Highest wheel number to use (3-8) [5]\n");
  printf("  -n           Use the Norway Enigma reflector (N) and wheels (1-5)\n");
  printf("  -r XYZ       Ring positions (ringstellung) XYZ (A-Z or .) [AA.]\n");
  printf("  -g XYZ       Start positions (grundstellung) XYZ (A-Z or .) [...]\n");
  printf("  -s AB...     Plugboard (steckerbrett) letter pairs (A-Z pairs) [none]\n");
  printf("  -c           Perform hill climbing to determine plugboard settings\n");
  printf("  -l language  Scoring language (english, german, danish, french); required\n");
  printf("               for -m/-b/-t/-q (no default), not used by -i\n");
  printf("  -i           Use index of coincidence (IC) to determine plaintext score\n");
  printf("  -m           Use monogram statistics to determine plaintext score\n");
  printf("  -b           Use bigram statistics to determine plaintext score\n");
  printf("  -t           Use trigram statistics to determine plaintext score\n");
  printf("  -q           Use quadgram statistics to determine plaintext score [default]\n");
  printf("  -p filename  Name of file containing plaintext to compare result with\n");
  printf("\n");
  printf("Defaults are indicated in [square brackets].\n");
  printf("\n");
  printf("The ciphertext is read from standard input. The final plaintext is written\n");
  printf("to standard output.\n");
  printf("\n");
  printf("For the reflector, wheels, ring position and start position, a dot (.)\n");
  printf("works as a wild card, leaving it unspecified. When these settings are not\n");
  printf("specified, the program will try all combinations to find the settings\n");
  printf("resulting in the highest plaintext score. If asked for, a hill climbing\n");
  printf("algorithm will be used to try to determine the plugboard settings.\n");
  printf("\n");
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
  fprintf(stderr, "; plugboard hill-climb: %s\n", opt_hillclimb ? "yes" : "no");

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
  if (argc == 1)
    {
      help();
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

  /* get arguments */

  int c;
  while ((c = getopt(argc, argv, "u:w:r:g:s:p:l:x:imbtqcvhn")) != -1)
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
        case 'l':
          opt_language = optarg;
          break;
        case 'v':
          version();
          exit(0);
          break;
        case 'h':
          help();
          exit(0);
          break;
        case 'n':
          opt_norenigma = 1;
          break;
        default:
          fprintf(stderr, "\n");
          help();
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


  /* read ciphertext */

  readciphertext();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  /* init */

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

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  ciphertext_letterdist();

  init();

  /* the per-search machine state lives in a single heap object (one today, one
     per worker thread once the search is parallelised) */
  machine * m = new machine();
  init_steckerbrett(*m, "");

  /* try all combinations */

  bruteforce(*m);

  /* write plaintext */

  fprintf(stderr, "\n");
  printf("%s\n", m->plaintext);

  /* read plaintext to compare to, if given */

  if (opt_plaintext)
    readplaintext(opt_plaintext, m->plaintext);

  delete m;
}
