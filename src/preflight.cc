#include "preflight.h"

#include "common.h"
#include "options.h"
#include "text.h"

#include <stdio.h>
#include <math.h>
#include <cmath>

/* Thresholds are set from the MEASURED tail of genuine Enigma, not from a
   nominal p-value: over one sample of 18000 SIMULATED encryptions spanning
   n = 40..600 -- authentic 1941 German under random keys and boards, not real
   traffic -- the largest z(IC) seen was 5.89 and NEITHER test fired once
   (preflight_null.py 2-3, which share a sample set so the two figures
   describe one population). The
   four broken 1941 messages -- genuine Enigma, and the useful controls, since
   two of them reach z = +4.2 -- all pass. QTXMA fires on both, at z = +10.9 and
   P = 8.5e-08. A false positive here is expensive in trust, a false negative
   only costs what it costs today, hence the wide margin. */
static const double preflight_z_ic = 6.0;
static const double preflight_p_absent = 1e-4;

struct preflight_stats
{
  double ic;         /* index of coincidence                                  */
  double z_ic;       /* (ic - 1/26) / sd, sd analytic                         */
  int absent;        /* letters of A-Z that never occur                       */
  double p_absent;   /* P(absent >= this many), first Bonferroni term         */
  bool flag_ic;
  bool flag_absent;
};

/* P(X >= k) for the number of unseen letters, first Bonferroni term
   C(A,k)(1-k/A)^n -- an upper bound, and an excellent approximation once it is
   small, which is the only regime the threshold cares about. Clamped at 1
   because the bound exceeds it badly for short messages, where several unseen
   letters are the norm (E[absent] = 5.4 at n = 40). */
static double absent_tail(int k, int n)
{
  if (k <= 0)
    return 1.0;
  if (k > asize)
    return 0.0;
  double c = 1.0;
  for (int i = 0; i < k; i++)
    c = c * (asize - i) / (i + 1);
  double p = c * pow(1.0 - static_cast<double>(k) / asize, n);
  return (p > 1.0) ? 1.0 : p;
}

static preflight_stats compute_preflight()
{
  preflight_stats s = {0.0, 0.0, 0, 1.0, false, false};
  long counts[asize] = {0};
  for (int i = 0; i < textlength; i++)
    counts[char2num(ciphertext[i])]++;
  for (int i = 0; i < asize; i++)
    if (counts[i] == 0)
      s.absent++;
  if (textlength < 2)
    return s;                    /* IC is undefined on fewer than two letters */
  long same = 0;
  for (int i = 0; i < asize; i++)
    same += counts[i] * (counts[i] - 1);
  double n = textlength;
  s.ic = same / (n * (n - 1));
  const double q = 1.0 / asize;
  double sd = sqrt(q * (1.0 - q) / (n * (n - 1) / 2.0));
  s.z_ic = (s.ic - q) / sd;
  s.p_absent = absent_tail(s.absent, textlength);
  s.flag_ic = s.z_ic > preflight_z_ic;
  s.flag_absent = s.p_absent < preflight_p_absent;
  return s;
}

/* Reported only when the run is a SEARCH -- which is also the only run for
   which the question is meaningful, since it is the one looking for a key that
   may not exist. With a fully-specified key the tool is encrypting or
   decrypting: on encryption the input is PLAINTEXT, which is language-like by
   definition, so reporting there would print a scary-looking line on every
   encryption (including the hundreds the test suite performs) about a
   ciphertext that is not one. */
static bool key_is_wildcarded()
{
  const char * o[4] = { opt_ukw, opt_walzen,
                        opt_ringstellung, opt_grundstellung };
  for (int i = 0; i < 4; i++)
    for (const char * p = o[i]; (p != nullptr) && (*p != 0); p++)
      if (*p == '.')
        return true;
  return false;
}

void report_preflight()
{
  if (opt_no_preflight || (textlength < 2) || ! key_is_wildcarded())
    return;
  preflight_stats s = compute_preflight();
  bool flagged = s.flag_ic || s.flag_absent;
  /* No continuation line may begin with an optionally-signed number: that is
     the shape of a progress line, and the harness greps stderr for
     `^ *[+-][0-9]` to pull the last margin out of a --confidence run. A
     second line reading "  +10.95 sd; ..." was picked up as one. */
  /* Label field is 12 wide and continuations are indented 12, matching every
     other settings-echo line ("Confidence: ", "Threads:    ", ...). This block
     used to carry one space more in both places, so it sat a column right of
     the rest of the echo. */
  fprintf(stderr,
          "Pre-flight: index of coincidence %.4f against the %.4f Enigma "
          "gives\n"
          "            (%+.2f sd), and %d of %d letters unused (P = %.2g)\n",
          s.ic, 1.0 / asize, s.z_ic, s.absent, asize, s.p_absent);
  if (! flagged)
    {
      fprintf(stderr, "            consistent with Enigma output\n");
      return;
    }
  fprintf(stderr,
          "WARNING: this does not look like Enigma output, so searching for a\n"
          "         key may be searching for something that does not exist.\n"
          "         Enigma is a permutation cipher and its output is"
          " near-flat;\n"
          "         this ciphertext has %s.\n"
          "         The thresholds (%.1f sd, P < %.0e) were set so that"
          " not one\n"
          "         of 18000 simulated Enigma ciphertexts trips them -- see\n"
          "         MODERN_BREAKING_NOTES 5l. Proceeding anyway.\n",
          (s.flag_ic && s.flag_absent)
            ? "language-like structure, and\n         too many unused letters"
            : (s.flag_ic ? "language-like structure"
                         : "too many unused letters"),
          preflight_z_ic, preflight_p_absent);
}
