#include "ngrams.h"

#include "common.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <vector>

/* Fold one Unicode code point to an A-Z letter index (0..25), or -1 if it is not
   a foldable Latin letter. Plain A-Z/a-z map directly; accented Latin letters
   fold to their base (diacritics removed: e-acute -> E, u-umlaut -> U, o-slash
   -> O, sharp-s -> S, ae/oe ligatures -> A/O, thorn -> T [pairs with eth -> D as
   the voiceless/voiced dental-fricative counterpart, Icelandic]). This lets the
   26-letter machine use the accented n-grams in the non-English tables (and
   accented plaintext) instead of discarding them. Added for Swedish/Finnish
   (A-ring, A/O-diaeresis -- already Latin-1), Icelandic (thorn) and Polish (the
   Latin Extended-A ogonek/stroke/acute/dot-above letters below): dropping them
   loses up to ~20% of a table's mass (Polish quadgrams), not a rounding error. */
int fold_codepoint(unsigned cp)
{
  if ((cp >= 'a') && (cp <= 'z'))
    cp -= 32;
  if ((cp >= 'A') && (cp <= 'Z'))
    return static_cast<int>(cp - 'A');
  /* Latin-1 supplement letters U+00C0..U+00FF -> base letter (' ' = not a letter);
     lower half mirrors the upper except the final cell (sharp-s S vs y-diaeresis Y). */
  static const char lat1[] =
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTS"
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTY";
  if ((cp >= 0xC0) && (cp <= 0xFF))
    {
      char b = lat1[cp - 0xC0];
      return (b == ' ') ? -1 : (b - 'A');
    }
  switch (cp)
    {
    case 0x0152: case 0x0153: return 'O' - 'A';   /* OE ligature */
    case 0x0178: return 'Y' - 'A';                /* Y with diaeresis */
    /* Latin Extended-A: Polish diacritics, diacritic stripped to the base letter
       (Z-acute and Z-dot-above both fold to Z, same "closest base letter"
       convention as sharp-s -> S above). */
    case 0x0104: case 0x0105: return 'A' - 'A';   /* A-ogonek (Ą/ą) */
    case 0x0106: case 0x0107: return 'C' - 'A';   /* C-acute (Ć/ć) */
    case 0x0118: case 0x0119: return 'E' - 'A';   /* E-ogonek (Ę/ę) */
    case 0x0141: case 0x0142: return 'L' - 'A';   /* L-stroke (Ł/ł) */
    case 0x0143: case 0x0144: return 'N' - 'A';   /* N-acute (Ń/ń) */
    case 0x015A: case 0x015B: return 'S' - 'A';   /* S-acute (Ś/ś) */
    /* Z-acute and Z-dot-above both fold to Z, so they share one case list. */
    case 0x0179: case 0x017A: case 0x017B: case 0x017C: return 'Z' - 'A';
    default: return -1;
    }
}

/* Fold a UTF-8 n-gram token (from the statistics files) to its A-Z base index.
   Returns the number of letters produced, or -1 if the token holds a code point
   that is not a foldable Latin letter. */
static int fold_gram(const char * s, int * index_out)
{
  const unsigned char * p = reinterpret_cast<const unsigned char *>(s);
  int idx = 0;
  int letters = 0;
  while (*p)
    {
      unsigned cp;
      if (*p < 0x80)
        { cp = *p; p += 1; }
      else if (((*p & 0xE0) == 0xC0) && ((p[1] & 0xC0) == 0x80))
        { cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
      else if (((*p & 0xF0) == 0xE0) && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80))
        { cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
      else
        return -1;
      int b = fold_codepoint(cp);
      if (b < 0)
        return -1;
      idx = idx * asize + b;
      letters++;
    }
  * index_out = idx;
  return letters;
}

/* Read an n-gram statistics table from "<language>_<suffix>.txt" into 'itable', the
   flat backing store of the corresponding uint8 array (mono8 / bi8 / tri8 / quad8),
   contiguous and row-major so the n letters of a record map to the single index
   ((a*26 + b)*26 + ...) of size 26^n. Raw counts are accumulated in a transient uint32
   scratch buffer; each entry is then stored as the log10 probability log10(count /
   total) -- a per-gram log-likelihood -- quantised to int16 fixed-point (see the
   ngram_scale note), so the additive scorers sum a log-probability and the per-symbol
   average (score_iter) is a cross-entropy (dits/char). Unseen n-grams are floored at
   log10(1 / total) -- scored as a single occurrence -- so an unattested gram is
   penalised like the rarest attested one rather than ruled out. Parsing stops at end of
   file or the first malformed record. */
/* Read the raw n-gram counts for one order into `table` (pre-sized to asize^n by the
   caller); returns the total count. Extracted from ngrams_read so the file-read is a
   single-purpose helper, separate from the quantisation. */
static uint64_t load_counts(int n, std::vector<uint32_t> & table,
                            const char * datadir, const char * language,
                            const char * suffix)
{
  char filename[1024];
  int len = snprintf(filename, sizeof(filename), "%s/%s_%s.txt",
                     datadir, language, suffix);
  if ((len < 0) || (len >= static_cast<int>(sizeof(filename))))
    fatal("Data directory / language path too long");

  FILE * f = fopen(filename, "r");
  if (!f)
    {
      fprintf(stderr, "Fatal error: Unable to open the language statistics file %s\n",
              filename);
      exit(1);
    }

  uint64_t total = 0;   /* sum of all counts, in uint64 (can exceed uint32) */
  int nonmappable = 0;  /* records skipped because a gram char could not fold */
  int overflowed = 0;   /* records whose count exceeded UINT32_MAX and was clamped */
  char line[256];
  while (fgets(line, sizeof(line), f))
    {
      /* One record per line: "<GRAM> <count>". FOLD each gram to its A-Z base
         (fold_gram: accents removed, e.g. u-umlaut -> U) and ACCUMULATE counts,
         since several accented grams now collide onto one base gram. The tables
         are frequency sorted and the non-English languages interleave accented
         grams (German umlauts and eszett, Danish/French accents) from near the
         top; the original parser stopped at the first such record, truncating
         e.g. the german quadgram table to its first 29 of 366k entries (4.9% of
         the count) and crippling non-English scoring. Folding keeps the whole
         distribution the 26-letter machine can represent. A record whose gram
         does not fold to exactly n A-Z letters (e.g. a stray digit) is skipped.

         `count` is parsed as unsigned long long (not directly into the uint32_t
         `table` cell): a generated table can contain a count that overflows 32
         bits -- the wehrmacht_quadgrams.txt generator (eval/build_telegraphic_ngrams.py)
         once emitted values up to ~8.3e20 from an unclamped reweighting ratio, and
         parsing that straight into `unsigned` via "%u" is undefined behaviour on
         overflow (glibc happened to saturate to UINT32_MAX, silently tying 843
         distinct quadgrams at one value -- see archived/PERFORMANCE.md 6.17). The generator
         is now capped and should never produce this again, but the loader clamps
         explicitly and audibly rather than depending on that, or on unspecified
         sscanf behaviour, to stay correct for any future/external table. */
      /* Hand-rolled instead of sscanf("%15s %llu", ...). sscanf reinterprets the format
         string and runs a general integer conversion for every one of the ~457k lines in
         the english quadgram table; measured on that file it is 59 ms of the 71 ms this
         loop costs, against 11 ms for the parse below (110 ms vs 27 ms under ASan). Since
         every invocation of the tool pays this before doing any work, it was ~30% of a
         short run and a third of the sanitizer CI job.

         Equivalent to the sscanf form on the bundled tables (verified byte-identical
         table hashes across every language x model), and deliberately STRICTER on
         malformed input in three places, none of them reachable from a well-formed table:
           - a token longer than 15 bytes is skipped here. sscanf would truncate it to 15
             and then usually fail the number conversion; in the one case it does not
             (chars 16+ happen to be digits) the record is still dropped below, because a
             >15-byte token folds to at least 6 letters and every n is <= 4.
           - a NEGATIVE count is skipped rather than wrapped. "%llu" accepts a sign and
             wraps, which the UINT32_MAX clamp below would then silently turn into the
             largest possible count -- the worst way to mishandle it.
           - a count exceeding 64 bits saturates rather than being undefined behaviour,
             which is what "%llu" overflow formally is (see the clamp comment above). */
      char gram[16];
      unsigned long long count64 = 0;
      {
        const char * p = line;
        while ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\v')
               || (*p == '\f') || (*p == '\r'))
          p++;
        const char * gstart = p;
        while ((*p != '\0') && (*p != ' ') && (*p != '\t') && (*p != '\n')
               && (*p != '\v') && (*p != '\f') && (*p != '\r'))
          p++;
        size_t glen = static_cast<size_t>(p - gstart);
        if ((glen == 0) || (glen >= sizeof(gram)))
          continue;                       /* empty line, or an over-long token */
        memcpy(gram, gstart, glen);
        gram[glen] = '\0';
        while ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\v')
               || (*p == '\f') || (*p == '\r'))
          p++;
        if (*p == '+')
          p++;
        if ((*p < '0') || (*p > '9'))
          continue;                       /* no count (or a negative one) */
        while ((*p >= '0') && (*p <= '9'))
          {
            unsigned d = static_cast<unsigned>(*p - '0');
            if (count64 > (UINT64_MAX - d) / 10)
              { count64 = UINT64_MAX; break; }   /* saturate, never wrap */
            count64 = count64 * 10 + d;
            p++;
          }
      }
      if (count64 > UINT32_MAX)
        {
          count64 = UINT32_MAX;
          overflowed++;
        }
      unsigned count = static_cast<unsigned>(count64);
      int index;
      int r = fold_gram(gram, & index);
      if (r < 0)          /* a code point we cannot fold to A-Z: warn + skip */
        {
          nonmappable++;
          continue;
        }
      if (r != n)         /* folds to the wrong number of letters: skip quietly */
        continue;
      table[index] += count;
      total += count;
    }

  fclose(f);
  if (nonmappable > 0)
    fprintf(stderr, "Note: %s: skipped %d record(s) with non-mappable characters.\n",
            filename, nonmappable);
  if (overflowed > 0)
    fprintf(stderr, "Warning: %s: clamped %d record(s) with a count exceeding %u.\n",
            filename, overflowed, UINT32_MAX);
  return total;
}

void ngrams_read(int n, uint8_t * itable, double * bias_out, double * scale_out,
                 const char * datadir, const char * language,
                 const char * suffix,
                 const double * force_ll, bool force_sym)   /* defaults: ngrams.h */
{
  int size = 1;
  for (int i = 0; i < n; i++)
    size *= asize;

  /* Raw counts are accumulated here (transient -- only itable outlives this call).
     uint32 holds every count exactly (the largest in the data is ~5.3e8, well inside
     the range); float would lose precision above 2^24 ~ 16.7M. */
  std::vector<uint32_t> table(size, 0);   /* unseen: count 0 until floored below */
  uint64_t total = load_counts(n, table, datadir, language, suffix);

  /* Quantise log10(count / total) into itable as uint8 fixed-point. Unseen grams are
     floored at count = floor_count = 1 (a hapax): not truly impossible (corpora have
     gaps, texts have typos), a deep floor was measured not to help, and this bounds the
     range to log10(max_count) so 8 bits suffice. The per-table bias is the minimum
     stored value (log10(min_effective_count/total) -- the floor when any gram is unseen,
     else the rarest seen), so q = round((v - bias) * ngram_scale) spends all 256 levels
     on the actual range. table[] held only the raw counts (scratch). */
  const double floor_count = 1.0;   /* unseen gram == a single occurrence (a hapax) */
  if (total == 0)
    total = 1;                        /* empty/degenerate table: avoid div-by-zero */

  /* RAISED FLAT FLOOR (env ENIGMA_FLOOR = T, default 0 -> byte-identical): merge every
     low-count gram (raw count <= T) onto ONE flat floor at the count-T level, on the theory
     that counts of 1 or 2 are corpus noise, so distinguishing them from "unseen" only feeds
     the wrong boards a noisy bottom-end gradient. This REMOVES gradient (unlike the graded
     probes, which add it). T=2 collapses {0,1,2} to the count-2 value; T=0 keeps the old
     behaviour (only count-0 unseen grams floored to a hapax). */
  const char * fl = getenv("ENIGMA_FLOOR");
  const int floor_t = (fl != nullptr) ? atoi(fl) : 0;
  const double floor_val = (floor_t >= 1) ? static_cast<double>(floor_t) : floor_count;

  /* SMOOTHING PROBE (env ENIGMA_SMOOTHING): how an unseen gram is scored.
       (default) flat floor -- every unseen gram == count floor_count = 1 (byte-identical).
       laplace   -- add-one: every gram (seen too) gets +1, total -> N + V; un-merges
                    hapax (2) from unseen (1). The blank/uniform pseudocount.
       background-- grade an unseen gram by its letter-composition prior q = p(A)p(B)p(C)p(D)
                    (monogram table, which is complete -> gap-free), mapped into a bounded
                    band [BG_LO, BG_HI] BELOW a hapax so seen grams always outrank unseen.
                    The wide letter-product range is why the adaptive scale is needed. */
  const char * sm = getenv("ENIGMA_SMOOTHING");
  const bool laplace    = (sm != nullptr) && (strcmp(sm, "laplace") == 0);
  const bool background = (sm != nullptr) && (strcmp(sm, "background") == 0);
  const bool overlap    = (sm != nullptr) && (strcmp(sm, "overlap") == 0) && (n == 4);
  const double BG_HI = 0.5, BG_LO = 1e-4;   /* unseen graded within [1e-4, 0.5] < hapax */
  /* laplace add-delta (Lidstone): delta < 1 penalises unseen harder while keeping a FLAT
     floor (all unseen == delta), un-merged from a hapax (1 + delta). ENIGMA_DELTA, default 1. */
  const char * ed = getenv("ENIGMA_DELTA");
  const double delta = (ed != nullptr) ? atof(ed) : 1.0;

  double p_letter[asize]; double max_qbg = 1.0;
  if (background)
    {
      std::vector<uint32_t> mono(asize, 0);
      uint64_t mt = load_counts(1, mono, datadir, language, "monograms");
      if (mt == 0) mt = 1;
      double mx = 0.0;
      for (int a = 0; a < asize; a++)
        { p_letter[a] = static_cast<double>(mono[a]) / static_cast<double>(mt);
          if (p_letter[a] > mx) mx = p_letter[a]; }
      for (int j = 0; j < n; j++) max_qbg *= mx;   /* (max letter prob)^n */
      if (max_qbg <= 0.0) max_qbg = 1.0;
    }

  /* overlap-corrected back-off (quad only): estimate an unseen quad ABCD's joint prob as
     p(ABC)*p(BCD)/p(BC) -- the linear-chain Markov estimate through the shared trigram
     overlap. The estimate is rank-mapped into the SAME [BG_LO, BG_HI] band below a hapax
     as background, so seen grams always outrank unseen; only the gradient WITHIN the floor
     band differs (a well-shaped tri/bi estimate vs background's crude monogram product).
     A gram with no lower-order support (tri or bi == 0) gets q = 0 -> BG_LO (deepest). */
  std::vector<uint32_t> tri_t, bi_t;
  double tri_total = 1.0, bi_total = 1.0, max_qov = 1.0;
  auto overlap_q = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    uint32_t t_abc = tri_t[(a * asize + b) * asize + c3];
    uint32_t t_bcd = tri_t[(b * asize + c3) * asize + d];
    uint32_t b_bc  = bi_t[b * asize + c3];
    if (t_abc == 0 || t_bcd == 0 || b_bc == 0) return 0.0;
    double p_abc = static_cast<double>(t_abc) / tri_total;
    double p_bcd = static_cast<double>(t_bcd) / tri_total;
    double p_bc  = static_cast<double>(b_bc) / bi_total;
    return p_abc * p_bcd / p_bc;
  };
  if (overlap)
    {
      tri_t.assign(static_cast<size_t>(asize) * asize * asize, 0);
      bi_t.assign(static_cast<size_t>(asize) * asize, 0);
      uint64_t tt = load_counts(3, tri_t, datadir, language, "trigrams");
      uint64_t bt = load_counts(2, bi_t, datadir, language, "bigrams");
      tri_total = tt ? static_cast<double>(tt) : 1.0;
      bi_total  = bt ? static_cast<double>(bt) : 1.0;
      double mx = 0.0;
      for (int i = 0; i < size; i++)
        if (table[i] == 0)
          { double q = overlap_q(i); if (q > mx) mx = q; }
      max_qov = (mx > 0.0) ? mx : 1.0;
    }

  /* Jelinek-Mercer per-gram interpolation (quad only, env ENIGMA_INTERP="l4,l3,l2,l1"):
     store the interpolated CONDITIONAL probability log10 P(D|ABC) with
       P(D|ABC) = l4*c(ABCD)/c(ABC) + l3*c(BCD)/c(BC) + l2*c(CD)/c(C) + l1*c(D)/N
     (weights normalised to sum 1). The mono term (complete table) keeps P>0 for every
     quad, so no floor is needed -- unseen quads back off smoothly to the lower orders.
     Unlike the floor probes this reshapes the SEEN scores, not just the tail. Note the
     model becomes conditional (vs the default joint quad), so "off" != any l4. */
  const char * ip = getenv("ENIGMA_INTERP");
  const bool interp = (ip != nullptr) && (n == 4);

  /* Log-linear interpolation (quad only, env ENIGMA_LOGLIN="a,b,c,d"): store a WEIGHTED SUM
     of the independent joint log-scores of the four orders that a window ABCD contains,
       v(ABCD) = a*log p(ABCD) + b*log p(BCD) + c*log p(CD) + d*log p(D),
     each order's own MLE joint log-prob with the usual hapax floor. Summed over the
     message's windows this is a weighted sum of the overall quad/tri/bi/mono scores (up to a
     3-letter boundary term). Stays JOINT -- no conditional reframing -- and weights (1,0,0,0)
     are byte-identical to the default quad. A geometric (log-linear) mixture of the models. */
  /* force_ll (the SCORE_ALL "-a" table) forces the log-linear symmetric fold with baked
     weights, independent of the ENIGMA_LOGLIN env (which stays an experimental override on
     the plain quad table). */
  const char * lp = getenv("ENIGMA_LOGLIN");
  const bool loglin = ((lp != nullptr) || (force_ll != nullptr)) && (n == 4) && !interp;
  /* symmetric folding (ENIGMA_LOGLIN_SYM): fold EVERY sub-gram a window contains (2 tris,
     3 bis, 4 monos) divided by its window-multiplicity (2/3/4), instead of only the trailing
     BCD/CD/D. Interior grams net the same weight; the difference is that the leading grams at
     the text start are now included (edge grams naturally down-weighted). Same one-lookup cost. */
  const bool loglin_sym = loglin &&
    (force_ll ? force_sym : (getenv("ENIGMA_LOGLIN_SYM") != nullptr));
  double lam[4] = {1.0, 0.0, 0.0, 0.0};   /* interp: normalised lambdas; loglin: raw weights */
  std::vector<uint32_t> mono_t;
  double mono_total = 1.0;
  if (interp || loglin)
    {
      double w[4] = {0.0, 0.0, 0.0, 0.0};
      if (force_ll)
        { for (int j = 0; j < 4; j++) w[j] = force_ll[j]; }
      else
        sscanf(interp ? ip : lp, "%lf,%lf,%lf,%lf", &w[0], &w[1], &w[2], &w[3]);
      double s = w[0] + w[1] + w[2] + w[3];
      if (interp)                          /* JM linear: normalise to sum 1 */
        {
          if (s <= 0.0) { w[0] = 1.0; s = 1.0; }
          for (int j = 0; j < 4; j++) lam[j] = w[j] / s;
        }
      else                                 /* log-linear: raw weights (overall scale is free) */
        {
          if (s <= 0.0) w[0] = 1.0;
          for (int j = 0; j < 4; j++) lam[j] = w[j];
        }
      if (tri_t.empty()) tri_t.assign(static_cast<size_t>(asize) * asize * asize, 0);
      if (bi_t.empty())  bi_t.assign(static_cast<size_t>(asize) * asize, 0);
      mono_t.assign(asize, 0);
      uint64_t tt = load_counts(3, tri_t, datadir, language, "trigrams");
      uint64_t bt = load_counts(2, bi_t, datadir, language, "bigrams");
      uint64_t mt = load_counts(1, mono_t, datadir, language, "monograms");
      tri_total  = tt ? static_cast<double>(tt) : 1.0;
      bi_total   = bt ? static_cast<double>(bt) : 1.0;
      mono_total = mt ? static_cast<double>(mt) : 1.0;
    }
  /* Every log10() here is applied to an INTEGER count, and counts repeat
     heavily: the 389 373 rows of the english quadgram file hold only 38 529
     distinct counts, and 92% of them are below 16384. Memoising on the count
     therefore removes 90% of the calls, which were 9.2% of a short run.
       Keyed on the count and evaluating the SAME expression, so every stored
     byte is unchanged. Rewriting log10(c/tot) as log10(c) - log10(tot) would
     NOT be safe: the two differ in the last ulp and these values feed a
     rounding boundary -- the same reason the vals[] scratch below stores
     doubles, not floats.
       Inverting the quantiser instead (only 256 output levels, and monotone in
     the count, so 256 thresholds plus a binary search would need no logs at
     all) was measured worth a further ~1%, and only on the non-loglin path:
     once the memo is in place, only the DISTINCT counts pay a log. */
  const uint32_t logmemo_lim = 1u << 14;   /* ~92% of rows; larger blows L2 */
  enum { LM_QUAD = 0, LM_TRI = 1, LM_BI = 2, LM_MONO = 3, LM_EFF = 4,
         LM_N = 5 };
  std::vector<double> logmemo[LM_N];
  auto memo_slot = [&](int which, uint32_t c) -> double *
  {
    if (c >= logmemo_lim)
      return nullptr;
    std::vector<double> & mv = logmemo[which];
    if (mv.empty())
      mv.assign(logmemo_lim, std::numeric_limits<double>::quiet_NaN());
    return &mv[c];
  };

  /* joint log10(count/total) of one order with the hapax floor (unseen -> single occurrence) */
  auto jlog = [&](int which, uint32_t c, double tot) -> double
  {
    double * slot = memo_slot(which, c);
    if ((slot != nullptr) && ! std::isnan(*slot))
      return *slot;
    const double v = log10((c > 0 ? static_cast<double>(c) : 1.0) / tot);
    if (slot != nullptr)
      *slot = v;
    return v;
  };
  auto loglin_v = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize;   /* v depends only on B,C,D (+ full quad) */
    double vq = jlog(LM_QUAD, table[idx], static_cast<double>(total));
    double vt = jlog(LM_TRI, tri_t[(b * asize + c3) * asize + d], tri_total);
    double vb = jlog(LM_BI, bi_t[c3 * asize + d], bi_total);
    double vm = jlog(LM_MONO, mono_t[d], mono_total);
    return lam[0] * vq + lam[1] * vt + lam[2] * vb + lam[3] * vm;
  };
  /* symmetric folding: all sub-grams of ABCD, each order divided by its window-multiplicity
     (tri/2, bi/3, mono/4) so interior grams net weight (b,c,d) and edge grams scale down. */
  auto loglin_v_sym = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    double vq = jlog(LM_QUAD, table[idx], static_cast<double>(total));
    double vt = jlog(LM_TRI, tri_t[(a * asize + b) * asize + c3], tri_total)
              + jlog(LM_TRI, tri_t[(b * asize + c3) * asize + d], tri_total);
    double vb = jlog(LM_BI, bi_t[a * asize + b], bi_total)
              + jlog(LM_BI, bi_t[b * asize + c3], bi_total)
              + jlog(LM_BI, bi_t[c3 * asize + d], bi_total);
    double vm = jlog(LM_MONO, mono_t[a], mono_total)
              + jlog(LM_MONO, mono_t[b], mono_total)
              + jlog(LM_MONO, mono_t[c3], mono_total)
              + jlog(LM_MONO, mono_t[d], mono_total);
    return lam[0] * vq + lam[1] * vt / 2.0 + lam[2] * vb / 3.0 + lam[3] * vm / 4.0;
  };
  auto interp_P = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    uint32_t q_abcd = table[idx];
    uint32_t t_abc = tri_t[(a * asize + b) * asize + c3];
    uint32_t t_bcd = tri_t[(b * asize + c3) * asize + d];
    uint32_t b_bc  = bi_t[b * asize + c3];
    uint32_t b_cd  = bi_t[c3 * asize + d];
    double p4 = (t_abc > 0) ? static_cast<double>(q_abcd) / t_abc : 0.0;
    double p3 = (b_bc  > 0) ? static_cast<double>(t_bcd)  / b_bc  : 0.0;
    double p2 = (mono_t[c3] > 0) ? static_cast<double>(b_cd) / mono_t[c3] : 0.0;
    double p1 = static_cast<double>(mono_t[d]) / mono_total;
    double P = lam[0] * p4 + lam[1] * p3 + lam[2] * p2 + lam[3] * p1;
    return (P > 0.0) ? P : 1.0 / mono_total;   /* backstop (p1>0 whenever letter D is seen) */
  };

  /* effective count of gram idx (raw count c) under the selected smoothing */
  auto eff_count = [&](int idx, double c) -> double
  {
    if (laplace) return c + delta;
    if (background || overlap)
      {
        if (c > 0.0) return c;                       /* seen: keep MLE */
        double q, denom;
        if (overlap) { q = overlap_q(idx); denom = max_qov; }
        else                                         /* background: letter-prior product */
          {
            q = 1.0; int t = idx;
            for (int j = 0; j < n; j++) { q *= p_letter[t % asize]; t /= asize; }
            denom = max_qbg;
          }
        double bg = q * (BG_HI / denom);
        return (bg < BG_LO) ? BG_LO : (bg > BG_HI ? BG_HI : bg);
      }
    return (c > floor_t) ? c : floor_val;            /* default flat floor (raisable) */
  };

  const double eff_total = laplace ? static_cast<double>(total) + delta * size
                                   : static_cast<double>(total);
  const double log_total = log10(eff_total);

  /* per-gram stored log-value: interpolation stores log10 P(D|ABC) directly; every other
     mode stores log10(effective_count / total). Byte-identical to the old two-line form for
     the non-interp path (log is monotonic, so min/max of the count == min/max of the value). */
  auto logval = [&](int idx) -> double
  {
    if (interp) return log10(interp_P(idx));
    if (loglin) return loglin_sym ? loglin_v_sym(idx) : loglin_v(idx);
    /* Default flat floor: eff_count() is a function of the count alone, so it
       memoises like jlog above. laplace/background/overlap grade by idx and
       must not. */
    if (! (laplace || background || overlap))
      {
        double * slot = memo_slot(LM_EFF, table[idx]);
        if ((slot != nullptr) && ! std::isnan(*slot))
          return *slot - log_total;
        const double v = log10(eff_count(idx, table[idx]));
        if (slot != nullptr)
          *slot = v;
        return v - log_total;
      }
    return log10(eff_count(idx, table[idx])) - log_total;
  };

  /* Evaluate logval ONCE per entry into a scratch array, rather than in both the min/max
     and the quantise loop below. The two loops used to call it 2 x size times (914k for
     quad), and under -a/-f each call recomputes the whole four-order log-linear mixture:
     measured 9.5 + 10.5 ms for -q but 77 + 77 ms for -f on the same table, i.e. the two
     passes cost more than parsing the file. Storing doubles (not floats) keeps every
     quantised byte identical -- the values feed a rounding boundary, so narrowing here
     would be a silent table change. 3.7 MB transient for quad, freed on return. */
  std::vector<double> vals(size);
  for (int i = 0; i < size; i++)
    vals[i] = logval(i);

  double vmin = 1e300, vmax = -1e300;
  for (int i = 0; i < size; i++)
    {
      double v = vals[i];
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
  if (vmin > vmax) { vmin = 0.0; vmax = 0.0; }   /* degenerate guard (empty table) */
  double bias = vmin;
  *bias_out = bias;

  /* Adaptive per-table scale: map the whole [vmin, vmax] span onto the full 0..255 byte
     range. With graded smoothing / interpolation the span shifts, which is exactly why the
     scale must adapt. Guard span==0 with the old fixed 32. */
  double span = vmax - vmin;
  double scale = (span > 0.0) ? 255.0 / span : 32.0;
  *scale_out = scale;

  for (int i = 0; i < size; i++)
    {
      double q = (vals[i] - bias) * scale;
      if (q < 0.0)
        q = 0.0;
      else if (q > 255.0)
        q = 255.0;
      itable[i] = static_cast<uint8_t>(q < 0.0 ? q - 0.5 : q + 0.5);
    }
}
