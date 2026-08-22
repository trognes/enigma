#include "machine.h"

#include "common.h"
#include "options.h"
#include "text.h"
#include "wiring.h"

#include <string.h>

/* Read-only wiring tables, derived once by init() and shared by every search. */
static unsigned char rotor_fwd[rotor_count][asize];
static unsigned char rotor_rev[rotor_count][asize];
static unsigned char notch[rotor_count][asize];
/* notch_gap[w][g] = characters this wheel can advance from window position g
   before its notch fires, i.e. the smallest k >= 0 with notch[w][(g+k) % 26].
   Lets setup_mapping() skip straight to the next stepping event instead of
   testing the notch once per character. Built by init(). */
static unsigned char notch_gap[rotor_count][asize];
/* notch_halfperiod[w] = "this wheel's notch SET is invariant under a shift of
   13", which is true of exactly VI, VII and VIII (both notch at M(12) and
   Z(25), 13 apart). Two things reach the machine from a stepping wheel's (ring,
   start): the offset diff26(g, r), which a joint shift preserves, and the
   ABSOLUTE position, which is read only by the notch test -- and that test
   cannot tell g from g+13 when the notch set has period 13. So for such a
   wheel, shifting its ring and start together by 13 is a byte-identical decode,
   unconditionally and at every message length. Derived from the notch table
   rather than hard-coded, so a wheel added later is picked up automatically;
   the equivalence itself is checked empirically in tests/run_tests.sh rather
   than trusted from this comment. Exploited for the RIGHT wheel in
   search_worker() -- see g_r2_halve. The MIDDLE wheel needs nothing here:
   §7.12's collapse derives its classes by simulating the stepping, so it
   already picks this up (measured 13.0 classes against 26.0 for a single-notch
   middle wheel at L=700). */
unsigned char notch_halfperiod[rotor_count];

static unsigned char reflector[reflector_count][asize];

void init()
{
  for (int i=0; i < rotor_count; i++)
    for (int j=0; j < asize; j++)
      {
        rotor_fwd[i][j] = char2num(rotor_string[i][j]);
        rotor_rev[i][j] = strchr(rotor_string[i],num2char(j)) - rotor_string[i];
        notch[i][j] = strchr(notch_string[i], num2char(j)) != NULL;
      }

  for (int i=0; i < rotor_count; i++)
    for (int j=0; j < asize; j++)
      {
        int k = 0;
        while ((k < asize) && ! notch[i][(j + k) % asize])
          k++;
        /* 26 means the wheel has no notch at all */
        notch_gap[i][j] = static_cast<unsigned char>(k);
      }

  for (int i = 0; i < rotor_count; i++)
    {
      bool any = false, period13 = true;
      for (int j = 0; j < asize; j++)
        {
          any = any || notch[i][j];
          if (notch[i][j] != notch[i][(j + asize / 2) % asize])
            period13 = false;
        }
      /* "no notch at all" is trivially period-13 and must NOT count: a
         notchless wheel never steps anything, so its absolute position is
         unread and the equivalence is the whole ring, not a shift of 13 -- a
         different fact, and one nothing here relies on. */
      notch_halfperiod[i] = static_cast<unsigned char>(any && period13);
    }

  for (int i=0; i < reflector_count; i++)
    for (int j=0; j < asize; j++)
      reflector[i][j] = char2num(reflector_string[i][j]);
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

/* Apply one rotor forward / in reverse at its (start - ring) offset. The offset is
   normalised into [0, 25] once, which is what lets both wrap-arounds be a single
   conditional instead of the two magic-constant divisions `(x + 26 +- y) % 26` compiled
   to. These run 7 times per entry while precompute() fills the 457 KB subst_array, i.e.
   3.2M times per wheel order -- 12-26% of a plain scan's instructions. */
static int rotor_l(machine & m, int x, int rotor_no)
{
  const int y = diff26(m.grundstellung[rotor_no], m.ringstellung[rotor_no]);
  x = rotor_fwd[m.walzenlage[rotor_no]][add26(x, y)];
  return diff26(x, y);
}

static int rotor_r(machine & m, int x, int rotor_no)
{
  const int y = diff26(m.grundstellung[rotor_no], m.ringstellung[rotor_no]);
  x = rotor_rev[m.walzenlage[rotor_no]][add26(x, y)];
  return diff26(x, y);
}


/* First middle-notch firing index for (w1, w2, start1, start2), or -1 for "never
   within `limit` characters". Pure stepping: no ring setting, start0, reflector or
   plugboard enters a stepping decision, so those do not index this. */
int mid_first_fire(int w1, int w2, int s1, int s2, int limit)
{
  int g1 = s1;
  int g2 = s2;
  for (int i = 0; i < limit; i++)
    {
      if (notch[w1][g1])
        return i;                    /* the firing that steps the left wheel */
      if (notch[w2][g2])
        g1 = step26(g1);
      g2 = step26(g2);
    }
  return -1;
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

  /* greek_offset is (start - ring) mod 26 and so is always in [0, 25], but
     that is established at its assignment site in another translation unit --
     the analyser cannot see it here, and reads a possibly-negative `o` as an
     out-of-bounds index into rotor_fwd below.  Stating the invariant costs one
     branch PER TASK (this runs once per wheel_task, never per character) and
     turns a silent wrong answer into a loud one if it is ever violated. */
  if ((m.greek_offset < 0) || (m.greek_offset >= asize))
    fatal("Internal error: Greek wheel offset out of range");
  const int o = m.greek_offset;
  const unsigned char * thin = reflector[m.ukw];      /* ukw = thin index 4/5 */
  const unsigned char * gf = rotor_fwd[m.greek];
  const unsigned char * gr = rotor_rev[m.greek];
  for (int x = 0; x < asize; x++)
    {
      int a = diff26(gf[add26(x, o)], o);    /* Greek forward  (rotor_l style) */
      int b = thin[a];                       /* thin reflector                 */
      int c = diff26(gr[add26(b, o)], o);    /* Greek reverse  (rotor_r style) */
      m.reflector_eff[x] = static_cast<unsigned char>(c);
    }
}

void precompute(machine & m)
{
  /* The stack is
       R2(g3) o [ R1(g2) o R0(g1) o Refl o L0(g1) o L1(g2) ] o L2(g3)
     so the bracketed middle depends on (g1, g2) only and is built 676 times
     rather than 17576; and the right wheel's two permutations depend on g3
     only, so they are tabulated once. The innermost loop then costs three
     table lookups per letter instead of seven rotor applications, and none of
     the modular arithmetic that used to wrap each one.
       Worth 10.8x: 134.0M -> 12.4M instructions, which takes precompute from
     10-21% of a plain scan to 1.0%. Counting only the table lookups predicts
     2.2x; the rest was the arithmetic around them. */
  int r1 = 0;
  int r2 = 0;
  int r3 = 0;
  unsigned char l2[asize][asize], rr2[asize][asize];
  for (int g3 = 0; g3 < asize; g3++)
    {
      init_ring_grund(m, r1, r2, r3, 0, 0, g3);
      for (int x = 0; x < asize; x++)
        {
          l2[g3][x] = static_cast<unsigned char>(rotor_l(m, x, 2));
          rr2[g3][x] = static_cast<unsigned char>(rotor_r(m, x, 2));
        }
    }

  unsigned char mid[asize];
  for (int g1 = 0; g1 < asize; g1++)
    for (int g2 = 0; g2 < asize; g2++)
      {
        init_ring_grund(m, r1, r2, r3, g1, g2, 0);
        for (int y = 0; y < asize; y++)
          {
            int v = rotor_l(m, y, 1);
            v = rotor_l(m, v, 0);
            v = m.reflector_eff[v];
            v = rotor_r(m, v, 0);
            v = rotor_r(m, v, 1);
            mid[y] = static_cast<unsigned char>(v);
          }
        for (int g3 = 0; g3 < asize; g3++)
          {
            unsigned char * __restrict out = m.subst_array[g1][g2][g3];
            const unsigned char * __restrict fwd = l2[g3];
            const unsigned char * __restrict rev = rr2[g3];
            for (int x = 0; x < asize; x++)
              out[x] = rev[mid[fwd[x]]];
          }
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

  /* Only the RIGHT wheel moves on most characters, and while it moves alone the
     row address just walks 26 bytes at a time. So rather than test both notches
     once per character, jump to the next stepping event: notch_gap[w2][g2] says
     how many characters the right wheel has before its notch fires, and the
     middle wheel's own notch cannot fire mid-run because g1 does not change
     there. A run is then a branch-free pointer fill, and the branchy stepping
     logic runs once per event (about once per 26 characters, twice for the
     two-notch VI-VIII) instead of once per character. */
  int d0 = diff26(g0, r0);
  int d1 = diff26(g1, r1);
  int d2 = diff26(g2, r2);
  const unsigned char (* blk)[asize] = sa[d0][d1];
  const unsigned char * row = blk[d2];

  int i = 0;
  while (i < textlength)
    {
      g2 = add26(d2, r2);            /* invariant: d2 == diff26(g2, r2) */

      /* A stepping event: the middle wheel advances, carrying the left one when
         it sits on its own notch (the Enigma double step). One character. */
      if (notch[w1][g1] || notch[w2][g2])
        {
          if (notch[w1][g1])
            {
              g0 = step26(g0);  d0 = step26(d0);
            }
          g1 = step26(g1);  d1 = step26(d1);
          d2 = step26(d2);      /* g2 is re-derived at the loop top */
          blk = sa[d0][d1];
          row = blk[d2];
          if (copy_rows)
            {
              memcpy(m.mapping[i], row, asize);
              rows[i] = m.mapping[i];
            }
          else
            rows[i] = row;
          i++;
          continue;
        }

      /* A run: the right wheel steps alone for `n` characters. g2 is not
         tracked through it -- d2 == diff26(g2, r2) is an invariant, so g2 is
         recovered from d2 at the top of the loop, which also keeps `n` (up to
         26 for a notchless wheel) out of add26's [0, 25] domain. */
      int n = notch_gap[w2][g2];
      if (n > textlength - i)
        n = textlength - i;
      while (n > 0)
        {
          /* Split at the ring-offset wrap so the fill itself is branch-free. */
          int m2 = asize - 1 - d2;
          if (m2 > n)
            m2 = n;
          for (int k = 0; k < m2; k++)
            {
              row += asize;
              if (copy_rows)
                {
                  memcpy(m.mapping[i + k], row, asize);
                  rows[i + k] = m.mapping[i + k];
                }
              else
                rows[i + k] = row;
            }
          i += m2;
          d2 += m2;
          n -= m2;
          if (n > 0)
            {
              d2 = 0;
              row = blk[0];
              if (copy_rows)
                {
                  memcpy(m.mapping[i], row, asize);
                  rows[i] = m.mapping[i];
                }
              else
                rows[i] = row;
              i++;
              n--;
            }
        }
    }

  m.grundstellung[0] = static_cast<unsigned char>(g0);
  m.grundstellung[1] = static_cast<unsigned char>(g1);
  m.grundstellung[2] = static_cast<unsigned char>(add26(d2, r2));
}

/* Cumulative step counts of the middle and left wheels, per character position,
   for a key starting at (g1, g2). Mirrors setup_mapping()'s stepping exactly --
   including the double step, where the middle wheel sitting on its OWN notch
   advances both itself and the left wheel -- but records only the counts, since
   that is all the --ring-stride refinement's derivation needs.

   The refinement uses these to compute how far a candidate's schedule has
   drifted from the coarse winner's (archived/refinement.md §4). The substitution consumes
   a_i = o0 + left(i), b_i = o1 + mid(i) and c_i = o2 + i, so a candidate whose
   step counts differ from the winner's by a constant reproduces the winner's
   alignment exactly when its ring offsets absorb that constant. w1/w2 are
   TRANSLATED rotor indices (as held in machine::walzenlage), since notch[] is
   indexed that way. g1/g2 are start positions and so are in [0, 25], which is
   what lets the stepping use step26(). */
void step_counts(int w1, int w2, int g1, int g2,
                        unsigned short * mid, unsigned short * left)
{
  int nmid = 0, nleft = 0;
  for (int i = 0; i < textlength; i++)
    {
      if (notch[w1][g1])
        {
          nleft++;
          nmid++;
          g1 = step26(g1);
        }
      else if (notch[w2][g2])
        {
          nmid++;
          g1 = step26(g1);
        }
      g2 = step26(g2);
      mid[i] = static_cast<unsigned short>(nmid);
      left[i] = static_cast<unsigned short>(nleft);
    }
}

/* The distinct values of (a[i] - b[i]) over the message, ascending. At most
   `cap` are stored; the return value is how many. The refinement emits one
   candidate per distinct value rather than reducing them to a mode -- a mode is
   a guess that can be wrong on a short message where the two schedules alternate
   evenly, while enumerating a handful of values cannot be (archived/refinement.md §4). */
int step_deltas(const unsigned short * a, const unsigned short * b,
                       int * out, int cap)
{
  int n = 0;
  for (int i = 0; i < textlength; i++)
    {
      int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
      int j = 0;
      while ((j < n) && (out[j] != d))
        j++;
      if ((j == n) && (n < cap))
        out[n++] = d;
    }
  return n;
}

/* Add every value within `band` of one already in `out[0..n)`, deduplicated, and return
   the new count. The derivation is exact for the step-count term, but the coarse winner's
   own offset can be off for reasons no schedule explains (archived/refinement.md §7.2), and a small
   band around each derived value covers that at a few times the candidate count -- which
   is still two orders of magnitude below the enumeration it replaced. */
int widen_deltas(int * out, int n, int band, int cap)
{
  int base = n;
  for (int i = 0; i < base; i++)
    for (int d = -band; d <= band; d++)
      {
        if (d == 0)
          continue;
        int v = out[i] + d;
        int j = 0;
        while ((j < n) && (out[j] != v))
          j++;
        if ((j == n) && (n < cap))
          out[n++] = v;
      }
  return n;
}

