#include "crib.h"

#include "common.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "progress.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <stdint.h>

extern const int selfcrib_maxlen;
const int selfcrib_maxlen = 13;         /* longest -- nothing in the corpus reaches 14 */
/* Terminal mode needs `tails x lengths` entries; sweeping needs ~3 per (alignment,
   length) pair, which is message-dependent and runs to thousands -- so both live in a
   vector, read-only once init_self_crib() returns, like crib_order. */
/* One hypothesis: the doubled word runs [at, at+len) and [at+len+1, at+2len+1), with an X
   separator between and an X flank on the left (and on the right when the message ends
   with one). `anchor_pos` holds the positions asserted to be plaintext X. */
struct selfcrib_hyp
{
  int at, len, nanchor;
  /* Letters between the two copies: 1 for the separated W X W the default
     hypothesises, 0 for the TANDEM repeat --self-crib-tandem adds. It is a
     field rather than a constant because it is the only thing that differs --
     the equality edges, the closure and the ranking are identical. */
  int gap;
  int anchor_pos[3];
};
static std::vector<selfcrib_hyp> g_selfcrib_hyps;
int g_selfcrib_nhyps = 0;
/* The menu, built once by init_crib(). A menu is a property of the crib AND the place it
   sits, so everything except the crib letters themselves is per ALIGNMENT: the ciphertext
   letters it pairs with, and therefore the anchor letter the 26 hypotheses are about.
     DELIBERATELY DECLARED HERE, beside the cold option globals, and not next to the
   deduction code further down -- that would put several KB of arrays a few dozen lines
   from plug_fixed, the hot climb-loop global whose placement this file is already
   documented as sensitive to (see the struct-layout note in CLAUDE.md). Keeping cold data
   away from it costs nothing and removes a whole class of accidental regression. */
static unsigned char crib_p[maxlen];    /* the crib's letters, one per menu edge */
static int crib_edges = 0;              /* = the crib length */
static int crib_align[maxlen];          /* viable alignments, in order */
static unsigned char crib_anchor_at[maxlen];   /* anchor letter of each one's menu */
static int crib_aligns = 0;
/* Menu edge order per alignment, BFS outward from that alignment's anchor -- see
   init_crib(). crib_aligns x crib_edges, indexed [a * crib_edges + step]. A vector
   because both bounds are message-dependent and a maxlen x maxlen array would be a
   megabyte of mostly-unused cold storage. Read-only once init_crib() returns. */
static std::vector<unsigned short> crib_order;
std::atomic<size_t> g_crib_rejected{0};   /* keys the crib proved impossible */
std::vector<std::string> g_crib_list;    /* read-only after load_crib_list() */
extern const size_t crib_sample_keys;
const size_t crib_sample_keys = 256;
/* Keys on which both sides of the choice are actually RUN, to measure the expected
   gain (the hypothesis count above needs a much larger sample, but it is far cheaper
   per key -- a climb costs thousands of board scores where a deduction costs tens of
   propagations). Eight keys, and a crib that has fallen this far behind on them is
   reported as a bound rather than measured further. */
extern const size_t crib_gain_keys;
const size_t crib_gain_keys = 8;
extern const uint64_t crib_gain_budget;
const uint64_t crib_gain_budget = 64;
static std::vector<std::pair<std::string, double>> g_cribs;   /* read-only after load */
/* --- crib deduction (--crib): the menu and its closure ---------------------------
   Decryption of one character is  p = steck[core_i[steck[c]]], and the rotor core is its
   own inverse, so the same line rearranges to

       steck[p] = core_i[steck[c]]

   -- if you know what the ciphertext letter is plugged to, the core tells you what the
   plaintext letter is plugged to. That is the whole deduction step: one table lookup, on
   the rows[] table setup_mapping() already builds.

   The crib gives one such equation per position. Guess a single plug, chain the rule
   along every equation, and add RECIPROCITY -- steck[x] = y implies steck[y] = x, and no
   two letters may share a partner. That second part is Welchman's diagonal board, free
   here because the plugboard is stored as an involution, and measured to supply almost
   all of the rejecting power: a loop-free 12-letter menu still rejects 88% of rotor
   settings, against 0% without it (archived/cribs.md 4.1).

   A contradiction kills the guess. Kill all 26 and the rotor setting cannot have produced
   the crib, so the search skips it without scoring anything.

   The menu is a property of the crib and the ciphertext, not of the key, so its edges and
   its anchor letter are built once at startup by init_crib(). */
/* Build the alignment list once, and each alignment's anchor.

   An alignment is VIABLE only if the crib disagrees with the ciphertext at every position:
   an Enigma never encrypts a letter to itself, so a match there proves the crib cannot sit
   at that offset. That test costs nothing and removes about half the alignments outright
   (archived/cribs.md 6.6) -- it is pure arithmetic on the ciphertext, done here rather than per key.

   The anchor is the highest-degree letter of the menu's largest connected component, so
   one guess reaches as far as it can before a second would be needed. It depends on which
   ciphertext letters the crib lands on, hence one per alignment. */
void init_crib()
{
  crib_edges = static_cast<int>(strlen(opt_crib_text));
  for (int j = 0; j < crib_edges; j++)
    crib_p[j] = static_cast<unsigned char>(char2num(opt_crib_text[j]));

  int first = (opt_crib_at >= 0) ? opt_crib_at : 0;
  int last = (opt_crib_at >= 0) ? opt_crib_at : textlength - crib_edges;
  crib_aligns = 0;
  crib_order.clear();
  for (int at = first; at <= last; at++)
    {
      bool viable = true;
      for (int j = 0; j < crib_edges; j++)
        if (crib_p[j] == num_ciphertext[at + j])
          {
            viable = false;
            break;
          }
      if (! viable)
        continue;

      /* components by union-find over this alignment's edges, then the largest
         component's busiest letter */
      int parent[asize], deg[asize];
      for (int i = 0; i < asize; i++)
        {
          parent[i] = i;
          deg[i] = 0;
        }
      for (int j = 0; j < crib_edges; j++)
        {
          int a = crib_p[j], b = num_ciphertext[at + j];
          deg[a]++;
          deg[b]++;
          while (parent[a] != a) a = parent[a];
          while (parent[b] != b) b = parent[b];
          if (a != b)
            parent[a] = b;
        }
      int size[asize], root[asize];
      for (int i = 0; i < asize; i++)
        size[i] = 0;
      for (int i = 0; i < asize; i++)
        {
          int r = i;
          while (parent[r] != r) r = parent[r];
          root[i] = r;
          if (deg[i])
            size[r]++;
        }
      int big = -1;
      for (int i = 0; i < asize; i++)
        if (deg[i] && ((big < 0) || (size[root[i]] > size[root[big]])))
          big = i;
      int anchor = -1;
      for (int i = 0; i < asize; i++)
        if (deg[i] && (root[i] == root[big])
            && ((anchor < 0) || (deg[i] > deg[anchor])))
          anchor = i;
      if (anchor < 0)
        continue;

      /* Edge order: BREADTH-FIRST OUTWARD FROM THE ANCHOR, not crib order.
         An edge can only deduce anything once one of its endpoints is known, and the
         only letter known at the start is the anchor. In crib order the loop therefore
         visits edges whose endpoints are both still unknown, does nothing, and relies
         on the enclosing `while (changed)` to come back for them -- so a long menu is
         re-scanned repeatedly. Visiting edges as the frontier reaches them makes the
         work track the COMPONENT rather than the edge count.
           Measured on wrong keys (the case a sweep spends its time on), total edge
         steps for the 26 hypotheses: 226 -> 97 at a 16-letter crib and 253 -> 93 at
         50 letters, i.e. 2.3x rising to 2.7x, because the BFS cost stays flat (~95)
         while crib order grows with length. Longest cribs gain most.
           This is a pure REORDERING: the closure is order-independent, so exactly the
         same keys are rejected and exactly the same plugs deduced. It changes when a
         contradiction is found, never whether. */
      {
        size_t base = static_cast<size_t>(crib_aligns) * crib_edges;
        crib_order.resize(base + crib_edges);
        bool used[maxlen] = { false };
        bool seen[asize] = { false };
        int queue[asize], qh = 0, qt = 0, n = 0;
        seen[anchor] = true;
        queue[qt++] = anchor;
        while (qh < qt)
          {
            int x = queue[qh++];
            for (int j = 0; j < crib_edges; j++)
              {
                if (used[j])
                  continue;
                int a2 = crib_p[j], b2 = num_ciphertext[at + j];
                if ((a2 != x) && (b2 != x))
                  continue;
                used[j] = true;
                crib_order[base + n++] = static_cast<unsigned short>(j);
                int other = (a2 == x) ? b2 : a2;
                if (! seen[other])
                  {
                    seen[other] = true;
                    queue[qt++] = other;
                  }
              }
          }
        /* Edges in other components can never fire from this anchor, but they still
           have to be present: crib_try walks all crib_edges slots. */
        for (int j = 0; j < crib_edges; j++)
          if (! used[j])
            crib_order[base + n++] = static_cast<unsigned short>(j);
      }

      crib_align[crib_aligns] = at;
      crib_anchor_at[crib_aligns] = static_cast<unsigned char>(anchor);
      crib_aligns++;
    }
}
/* Assign steck[x] = y and its reciprocal, returning false on any disagreement.
   The two guards ARE Welchman's diagonal board: a letter already plugged to
   something else contradicts, and so does a partner already claimed by a third
   letter. Nearly all of the crib's rejecting power comes from these two tests,
   not from the menu itself. Callers must propagate the false -- there is no
   partial write, so the board is unchanged when it fails. */
static inline bool crib_set(int * board, int x, int y)
{
  if ((board[x] >= 0) && (board[x] != y))
    return false;
  if ((board[y] >= 0) && (board[y] != x))
    return false;
  board[x] = y;
  board[y] = x;
  return true;
}
/* Build the terminal-signature hypothesis list. Depends only on the CIPHERTEXT (its
   length, and which flank positions could carry a plaintext X), never on the key, so it
   is built once at startup exactly as init_crib() builds its alignments.

   `tail` distinguishes a message ending `... X NAME X NAME` from one ending
   `... X NAME X NAME X`; both occur in the corpus, so both are hypothesised. A hypothesis
   whose flank position holds a ciphertext X is impossible -- an Enigma never encrypts a
   letter to itself -- and is dropped here rather than rediscovered per key. */
/* Add one hypothesis if the ciphertext permits it. `flanks` selects which anchors are
   asserted to be plaintext X: 1 = separator only, 2 = + left flank, 3 = + right flank.
   An Enigma never encrypts a letter to itself, so a ciphertext X at an anchor position
   makes the hypothesis impossible -- dropped here rather than rediscovered per key. */
/* `flanks` selects which positions the hypothesis asserts are plaintext X.
   At gap 1 the separator comes free with the doubling and the flanks are extra:
   1 = separator, 2 = + left, 3 = + left and right.
     At gap 0 there IS no separator, so the left flank is asserted instead and
   `flanks` only chooses whether the right one joins it. THE HYPOTHESIS MUST
   CARRY AT LEAST ONE ANCHOR: the 26 guesses are on steck[X], and the equality
   edges cannot start anything until an anchor propagates that guess into the
   message -- with none, every board entry would stay unset and the hypothesis
   would deduce nothing at all. A tandem repeat can afford this because it
   nearly always HAS a left flank: 4 of 4 in the corpus, matching the 96%
   left-flank rate measured for the separated case, and asserting it recovers
   most of the sharpness the missing separator costs (top-5 168 -> 182 of 200,
   ENHANCEMENTS.md item 5). */
static void selfcrib_add(int at, int len, int flanks, int gap)
{
  const int xl = char2num('X');
  selfcrib_hyp h;
  h.at = at;
  h.len = len;
  h.gap = gap;
  h.nanchor = 0;
  if (gap >= 1)
    {
      h.anchor_pos[h.nanchor++] = at + len;              /* separator */
      if (flanks >= 2)
        h.anchor_pos[h.nanchor++] = at - 1;              /* left flank */
      if (flanks >= 3)
        h.anchor_pos[h.nanchor++] = at + 2*len + 1;      /* right flank */
    }
  else
    {
      h.anchor_pos[h.nanchor++] = at - 1;                /* left flank */
      if (flanks >= 3)
        h.anchor_pos[h.nanchor++] = at + 2*len;          /* right flank */
    }
  for (int k = 0; k < h.nanchor; k++)
    {
      const int pos = h.anchor_pos[k];
      if ((pos < 0) || (pos >= textlength) || (num_ciphertext[pos] == xl))
        return;
    }
  g_selfcrib_hyps.push_back(h);
}
void init_self_crib()
{
  g_selfcrib_hyps.clear();
  if (opt_self_crib_signature)
    {
      /* The word closes the message, so only its length is unknown and the left flank is
         always present. `tail` is the trailing X, present or not. */
      for (int tail = 0; tail <= 1; tail++)
        for (int len = opt_self_crib_length; len <= selfcrib_maxlen; len++)
          {
            const int at = textlength - tail - 2*len - 1;
            if (at < 1)
              continue;                    /* no room for the left flank */
            selfcrib_add(at, len, tail ? 3 : 2, 1);
          }
    }
  else
    {
      /* Every alignment, three flank variants each: the flanks are a GUESS (96% left,
         71% both in the corpus), so this must try asserting neither, the left, or both --
         asserting one the message does not have rejects the true key. */
      for (int len = opt_self_crib_length; len <= selfcrib_maxlen; len++)
        for (int at = 1; at + 2*len + 1 <= textlength; at++)
          for (int flanks = 1; flanks <= 3; flanks++)
            selfcrib_add(at, len, flanks, 1);
    }
  /* --self-crib-tandem: the same enumeration at gap 0, appended rather than
     replacing, since a message can hold either kind and the ranking sorts them
     out. Two flank variants, not three: at gap 0 the left flank is the only
     anchor available and is always asserted, so the choice is just whether the
     right one joins it. Roughly DOUBLES the hypothesis count (+101% over the
     corpus), which is why it is opt-in -- see the option comment. */
  if (opt_self_crib_tandem)
    for (int len = opt_self_crib_length; len <= selfcrib_maxlen; len++)
      for (int at = 1; at + 2*len <= textlength; at++)
        for (int flanks = 2; flanks <= 3; flanks++)
          selfcrib_add(at, len, flanks, 0);
  g_selfcrib_nhyps = static_cast<int>(g_selfcrib_hyps.size());
}
/* The self-crib closure for one hypothesis under one guess for steck[X].

   Two edge kinds, and the second is the whole difference from crib_try():

     anchor    plaintext X at a known position -- steck[c] = core[steck[X]], the classic
               rule, since the rotor core is an involution;
     equality  positions i and j carry the same UNKNOWN letter --
               steck[c_j] = core_j[core_i[steck[c_i]]], and the mirror image.

   crib_set() supplies reciprocity and Welchman's diagonal board unchanged: a plugboard is
   an involution, so setting steck[x] = y sets steck[y] = x, and a clash kills the guess.
   Returns false when the guess is contradictory; `board` holds -1 where undeduced. */
static bool self_crib_try(const machine & m, const selfcrib_hyp & h, int guess, int * board)
{
  const int xl = char2num('X');
  for (int i = 0; i < asize; i++)
    board[i] = -1;
  if (! crib_set(board, xl, guess))
    return false;

  bool changed = true;
  while (changed)
    {
      changed = false;
      for (int k = 0; k < h.nanchor; k++)
        {
          const unsigned char * __restrict core = m.rows[h.anchor_pos[k]];
          const int c = num_ciphertext[h.anchor_pos[k]];
          if ((board[c] >= 0) && (board[xl] < 0))
            {
              if (! crib_set(board, xl, static_cast<int>(core[board[c]])))
                return false;
              changed = true;
            }
          else if ((board[xl] >= 0) && (board[c] < 0))
            {
              if (! crib_set(board, c, static_cast<int>(core[board[xl]])))
                return false;
              changed = true;
            }
          else if ((board[xl] >= 0) && (board[c] >= 0))
            {
              if (static_cast<int>(core[board[c]]) != board[xl])
                return false;
            }
        }
      for (int t = 0; t < h.len; t++)
        {
          const int pi = h.at + t, pj = h.at + h.len + h.gap + t;
          const unsigned char * __restrict ci = m.rows[pi];
          const unsigned char * __restrict cj = m.rows[pj];
          const int a = num_ciphertext[pi], b = num_ciphertext[pj];
          if ((board[a] >= 0) && (board[b] < 0))
            {
              if (! crib_set(board, b, static_cast<int>(cj[ci[board[a]]])))
                return false;
              changed = true;
            }
          else if ((board[b] >= 0) && (board[a] < 0))
            {
              if (! crib_set(board, a, static_cast<int>(ci[cj[board[b]]])))
                return false;
              changed = true;
            }
          else if ((board[a] >= 0) && (board[b] >= 0))
            {
              if (ci[board[a]] != cj[board[b]])
                return false;
            }
        }
    }
  return true;
}
/* One menu edge with everything the deduction needs in one place, laid out in
   BFS order so the closure walks it sequentially. p and c are fixed for the
   whole run once the alignment is chosen; only `core` changes, and only per
   key. All three used to be re-gathered from three separate arrays --
   crib_p[order[jj]], num_ciphertext[at + j] and rows[at + j] -- once per edge
   per HYPOTHESIS, i.e. 26 times over for values that do not vary across the
   26. They profiled at 17.1% of a crib sweep between them (5.93 + 5.22 + 4.45
   + 1.48).
     Worth -13.1% of crib_try's instructions, but read the wall-clock caveat
   before quoting a speed: the loads this removes are all L1-resident, so a
   healthy out-of-order core hides them behind the dependent board[] chain and
   the branch mispredicts. Measured -0.3..-1.9% on the CI runners (six cells,
   all negative, so real but small) against -11.5% on one virtualised
   container where they evidently sit on the critical path. The instruction
   count is the portable number; the "-12%" in the commit message that
   introduced this struct is not -- that figure came from the container,
   and CI corrected it afterwards. */
struct crib_edge
{
  const unsigned char * core;
  unsigned char p, c;
};
/* Fill `ed` for alignment index `a` from this machine's rows[]. Called ONCE per
   key per alignment; the 26 hypotheses then read it and nothing else. */
static void crib_edges_for(const machine & m, int a, crib_edge * ed)
{
  const int at = crib_align[a];
  const unsigned short * const order =
    &crib_order[static_cast<size_t>(a) * crib_edges];
  for (int jj = 0; jj < crib_edges; jj++)
    {
      const int j = order[jj];
      ed[jj].core = m.rows[at + j];
      ed[jj].p = crib_p[j];
      ed[jj].c = num_ciphertext[at + j];
    }
}
/* One hypothesis at one alignment: "the anchor letter is plugged to hyp". Propagates to a
   fixed point and
   returns false on contradiction. `board` is left holding the partial plugboard it
   deduced (-1 = still unknown), which is what the hybrid will seed a climb from.
     `board` is int, not signed char: it holds -1 alongside letter values coming out of
   the UNSIGNED char rows[] table, and mixing those two signednesses is the bug class
   clang-tidy's bugprone-signed-char-misuse exists to catch. 26 ints, read once per key,
   cost nothing. */
static bool crib_try(int anchor, int hyp, int * board,
                     const crib_edge * __restrict ed)
{
  for (int i = 0; i < asize; i++)
    board[i] = -1;

  /* Start from what is already KNOWN, not from nothing. -s says those letters are
     plugged so, and --no-plug says its letters are plugged to nothing; a hypothesis
     that contradicts either is impossible and crib_set rejects it here rather than
     letting it through to be silently overwritten at the seeding site. That overwrite
     was a real bug: it left a board that was not an involution (A plugged to D by the
     deduction while B still pointed at A from -s), which then crashed
     format_plugboard. Cheap -- it runs once per hypothesis, over 26 letters, and the
     usual case of no -s and no --no-plug sets nothing at all. */
  if (have_known_plugs())
    {
      const bool * mark = known_plug_mark();
      const unsigned char * partner = known_plug_partner();
      for (int i = 0; i < asize; i++)
        if (mark[i] && ! crib_set(board, i, partner[i]))
          return false;
    }

  if (! crib_set(board, anchor, hyp))
    return false;
  bool changed = true;
  while (changed)
    {
      changed = false;
      for (int jj = 0; jj < crib_edges; jj++)
        {
          const unsigned char * core = ed[jj].core;
          int p = ed[jj].p, c = ed[jj].c;
          if ((board[c] >= 0) && (board[p] < 0))
            {
              if (! crib_set(board, p, static_cast<int>(core[board[c]])))
                return false;
              changed = true;
            }
          else if ((board[p] >= 0) && (board[c] < 0))
            {
              if (! crib_set(board, c, static_cast<int>(core[board[p]])))
                return false;
              changed = true;
            }
          else if ((board[p] >= 0) && (board[c] >= 0))
            {
              if (static_cast<int>(core[board[c]]) != board[p])
                return false;
            }
        }
    }
  return true;
}
/* --crib-dump: print every surviving hypothesis, the alignment it survived at, and the
   plugs it deduces, so a harness can check them against a known board (archived/cribs.md 10.1).
   Display-only and under the same mutex as the progress lines, so it cannot affect which
   candidate wins. Very verbose; off by default.

   THE LINE FORMAT HAS TWO CONSUMERS that both parse it positionally --
   eval/crib_vectors_check.py and the --crib checks in tests/run_tests.sh -- so adding a
   field here silently breaks them until they are updated. Adding the alignment field did
   exactly that. Change both when changing this. */
void crib_dump(machine & m, int r1, int r2, int r3, int g1, int g2, int g3);
/* The first alignment at which some hypothesis survives, or -1 when every hypothesis at
   every viable alignment contradicts -- in which case this rotor setting cannot have
   produced the crib ANYWHERE in the message, and the caller skips it without scoring.

   Returning the alignment rather than a bool is what step 5 will seed a climb from, and
   what the progress line reports now. The early exit matters and is asymmetric: a key that
   survives usually does so at one of the first alignments tried, while a REJECTED key pays
   the whole sweep -- and rejected keys are meant to be the common case.

   Pure function of the key: no shared state, so it is thread-safe and -T-deterministic. */
int crib_first_stop(const machine & m)
{
  int board[asize];
  crib_edge ed[maxlen];
  for (int a = 0; a < crib_aligns; a++)
    {
      crib_edges_for(m, a, ed);
      for (int h = 0; h < asize; h++)
        if (crib_try(crib_anchor_at[a], h, board, ed))
          return crib_align[a];
    }
  return -1;
}
/* Known-word ("crib") bonus for a converged board: sum the weights of every occurrence of
   a known word as a SUBSTRING of the decrypt. Substring (not token) matching is deliberate
   -- telegraphic traffic concatenates words within a clause (ROEMEINSBERTA, not
   ROEM.X.EINS.X.BERTA; X separates clauses, not words), so token matching would miss them.
   Read from m.plaintext, which the climb leaves holding the converged board's decrypt, so
   no extra decode. Deterministic (a pure function of the board and the fixed word list),
   hence -T-invariant. Only called when opt_crib is set, so the default path is untouched.
   Longer words carry more weight (see the crib file), which keeps rare short-word
   coincidences on garbage from swamping the genuine multi-word signal on the true board. */
double crib_score(const machine & m)
{
  const char * __restrict pt = m.plaintext;
  double s = 0.0;
  for (const std::pair<std::string, double> & cw : g_cribs)
    {
      const int k = static_cast<int>(cw.first.size());
      const char * __restrict w = cw.first.data();
      for (int i = 0; i + k <= textlength; i++)
        if (memcmp(pt + i, w, static_cast<size_t>(k)) == 0)
          {
            s += cw.second;   /* presence: each distinct word counts once, so a garbage
                                 board cannot win by repeating one coincidental short match */
            break;
          }
    }
  return s;
}
/* Load the known-word list for --crib-rerank: one word per line, an optional weight after
   it (default 1.0); '#' starts a comment. Words are folded to A-Z uppercase (matching the
   ciphertext/plaintext readers). Populates g_cribs and sets opt_crib. */
void load_cribs(const char * fname)
{
  FILE * f = fopen(fname, "r");
  if (f == nullptr)
    fatal("Cannot open crib file");
  char line[256];
  while (fgets(line, sizeof line, f) != nullptr)
    {
      char * p = line;
      while ((*p == ' ') || (*p == '\t'))
        p++;
      if ((*p == '#') || (*p == '\n') || (*p == '\0'))
        continue;
      std::string word;
      while ((*p != '\0') && (*p != ' ') && (*p != '\t') && (*p != '\n'))
        {
          int u = toupper(static_cast<unsigned char>(*p));
          if ((u >= 'A') && (u <= 'Z'))
            word += static_cast<char>(u);
          p++;
        }
      double w = 1.0;
      while ((*p == ' ') || (*p == '\t'))
        p++;
      if ((*p != '\0') && (*p != '\n') && (*p != '#'))
        {
          char * end = nullptr;
          double v = strtod(p, &end);
          if (end != p)
            w = v;
        }
      if (! word.empty())
        g_cribs.emplace_back(word, w);
    }
  fclose(f);
  opt_crib = g_cribs.empty() ? 0 : 1;
}
/* Load the crib LIBRARY for --crib-list: one crib per line, '#' starts a comment, and
   anything after the crib on a line is informational (build_cribs.py writes the tier,
   spare letters and provenance there). Letters are folded to A-Z uppercase like every
   other text this tool reads.
     File ORDER is significant and preserved: the generator emits its cribs most-likely-
   to-match first, and a run stops at the first crib that wins, so re-sorting the list
   would throw away the early exit that makes crib-outer worth doing (archived/cribs.md 5 step 5
   measured median time-to-first-hit at 10 h in this order against 82 h ordered by
   tier). Duplicates are dropped -- a repeated crib costs a full rotor sweep to learn
   nothing new -- but the FIRST occurrence keeps its position. */
void load_crib_list(const char * fname)
{
  FILE * f = fopen(fname, "r");
  if (f == nullptr)
    fatal("Cannot open --crib-list file");
  char line[1024];
  std::set<std::string> seen;
  while (fgets(line, sizeof line, f) != nullptr)
    {
      char * p = line;
      while ((*p == ' ') || (*p == '\t'))
        p++;
      if ((*p == '#') || (*p == '\n') || (*p == '\r') || (*p == '\0'))
        continue;
      std::string crib;
      while ((*p != '\0') && (*p != ' ') && (*p != '\t')
             && (*p != '\n') && (*p != '\r'))
        {
          int u = toupper(static_cast<unsigned char>(*p));
          if ((u >= 'A') && (u <= 'Z'))
            crib += static_cast<char>(u);
          p++;
        }
      /* A 1-letter crib has no menu edge to chain along, matching --crib's own limit. */
      if ((crib.size() >= 2) && seen.insert(crib).second)
        g_crib_list.push_back(crib);
    }
  fclose(f);
  if (g_crib_list.empty())
    fatal("--crib-list file holds no usable cribs (need at least 2 letters A-Z each)");
}
/* Pin one hypothesis's deduced plugs and run the staged climb from there. Shared by
   both crib_unit() paths so the seeded and unseeded runs cannot drift apart. */
static double crib_climb_one(machine & m, const int * board,
                             size_t key_index, int restart)
{
  init_steckerbrett(m, opt_steckerbrett);      /* board = identity + -s */
  plug_fixed_ex_reset(m);    /* pins = -s / --no-plug ... */
  for (int x = 0; x < asize; x++)
    if (board[x] >= 0)
      {
        m.steckerbrett[x] = static_cast<unsigned char>(board[x]);
        plug_fixed_ex_pin(m, x);               /* ... plus this deduction */
      }
  /* The kick is off by default here and should stay off: it can only scatter the
     letters the deduction did NOT settle, and a seeded climb starts near the answer
     (archived/cribs.md 7b). -R N still asks for N kicked passes if that is wanted. */
  if (opt_restarts >= 1)
    {
      uint64_t rng = restart_seed(key_index, restart);
      perturb_steckerbrett(m, & rng, opt_perturb);
    }
  const double sc = run_stages<true>(m);
  if (opt_dump_all)
    dump_all(m, sc);
  return sc;
}
/* --- the hybrid: deduce, then climb (archived/cribs.md 7, 12 step 5) --------------------------

   One work item at a key the crib did not reject: climb once from EVERY surviving
   hypothesis, seeded with the plugs that hypothesis deduces, and keep the best.

   The deduced plugs are HELD FIXED for the climb, in PLUG_FIXED_EX -- the same per-worker
   pin set --exhaust uses, because plug_fixed is a read-only global that no worker may
   touch. They stay fixed through --polish too: a deduced plug comes from arithmetic on the
   machine equation, while the finisher's cascade is score-driven local repair, so
   releasing them would let weaker evidence overwrite stronger (archived/cribs.md 7b). A WRONG
   hypothesis needs no such rescue -- it loses on score to the other 25.

   Letters the deduction settles as carrying NO cable are pinned as well: board[x] == x is
   a real finding, not an absence of one, and marking it stops the climb wasting moves on a
   letter that cannot be plugged. That is the value archived/cribs.md 7 wanted --no-plug for, had
   here for free.

   Cost is one climb per surviving hypothesis. With a long crib that is usually one; with a
   short one it is the several that archived/cribs.md 7a's seed mode expects and prices. */
double crib_unit(machine & m, size_t key_index, int restart)
{
  double best = 0.0;
  bool have = false;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  int best_at = -1;
  int board[asize];

  crib_edge ed[maxlen];

  if (opt_crib_seeds <= 0)
    {
      /* Historical path: climb EVERY survivor, in discovery order. Kept exactly as it
         was, so a run without --crib-seeds is byte-identical -- including the count of
         plugboards scored, which deduplication would change. */
      for (int a = 0; a < crib_aligns; a++)
        {
          crib_edges_for(m, a, ed);
          for (int h = 0; h < asize; h++)
            {
              if (! crib_try(crib_anchor_at[a], h, board, ed))
                continue;
              const double sc = crib_climb_one(m, board, key_index, restart);
              if (! have || (sc > best))
                {
                  best = sc;
                  have = true;
                  best_at = crib_align[a];
                  memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
                  memcpy(best_steck, m.steckerbrett, asize);
                }
            }
        }
    }
  else
    {
      /* --crib-seeds K: collect the distinct survivors, rank by the IC of their decrypt,
         climb the best K. Per-thread scratch rather than stack arrays, since a swept
         short crib yields hundreds of survivors -- far past what a per-call array should
         hold -- and reusing the buffers keeps allocation out of the per-key path.
         Cleared at entry, so a key's result depends on nothing but that key. */
      static thread_local std::vector<unsigned char> cs_steck, cs_fixed;
      static thread_local std::vector<double> cs_ic;
      static thread_local std::vector<int> cs_at, cs_order;
      cs_steck.clear(); cs_fixed.clear(); cs_ic.clear();
      cs_at.clear(); cs_order.clear();
      int nseed = 0;

      for (int a = 0; a < crib_aligns; a++)
        {
          crib_edges_for(m, a, ed);
          for (int h = 0; h < asize; h++)
            {
              if (! crib_try(crib_anchor_at[a], h, board, ed))
                continue;
              unsigned char st[asize], fx[asize];
              for (int i = 0; i < asize; i++)
                {
                  st[i] = static_cast<unsigned char>((board[i] >= 0) ? board[i] : i);
                  fx[i] = (board[i] >= 0) ? 1 : 0;
                }
              /* The dedupe key is the (board, pinned-letter-set) PAIR, as in
                 self_crib_unit(): two hypotheses can agree on every cable while one
                 additionally proves a letter carries none, and that is a different
                 seed -- it hands the climb one less letter to search. */
              bool dup = false;
              for (int k = 0; (k < nseed) && ! dup; k++)
                dup = (memcmp(& cs_steck[static_cast<size_t>(k) * asize],
                              st, asize) == 0) &&
                      (memcmp(& cs_fixed[static_cast<size_t>(k) * asize],
                              fx, asize) == 0);
              if (dup)
                continue;
              cs_steck.insert(cs_steck.end(), st, st + asize);
              cs_fixed.insert(cs_fixed.end(), fx, fx + asize);
              cs_at.push_back(crib_align[a]);
              memcpy(m.steckerbrett, st, asize);
              cs_ic.push_back(ic_score_decode(m));  /* the ranking, one decode each */
              nseed++;
            }
        }

      if (nseed == 0)
        return unit_no_score;    /* nothing survived: never wins the merge */

      /* Top K by IC, ties broken by the deterministic discovery order. Partial
         selection sort: K is small, so this beats sorting the whole list. */
      cs_order.resize(static_cast<size_t>(nseed));
      for (int i = 0; i < nseed; i++)
        cs_order[static_cast<size_t>(i)] = i;
      const int want = (opt_crib_seeds < nseed) ? opt_crib_seeds : nseed;
      for (int i = 0; i < want; i++)
        {
          size_t best_j = static_cast<size_t>(i);
          for (size_t j = static_cast<size_t>(i) + 1;
               j < static_cast<size_t>(nseed); j++)
            if (cs_ic[static_cast<size_t>(cs_order[j])] >
                cs_ic[static_cast<size_t>(cs_order[best_j])])
              best_j = j;
          const size_t ii = static_cast<size_t>(i);
          const int t = cs_order[ii]; cs_order[ii] = cs_order[best_j];
          cs_order[best_j] = t;
        }

      for (int i = 0; i < want; i++)
        {
          const int si = cs_order[static_cast<size_t>(i)];
          for (int x = 0; x < asize; x++)
            board[x] = cs_fixed[static_cast<size_t>(si) * asize
                                + static_cast<size_t>(x)]
                       ? static_cast<int>(cs_steck[static_cast<size_t>(si) * asize
                                                   + static_cast<size_t>(x)])
                       : -1;
          const double sc = crib_climb_one(m, board, key_index, restart);
          if (! have || (sc > best))
            {
              best = sc;
              have = true;
              best_at = cs_at[static_cast<size_t>(si)];
              memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
              memcpy(best_steck, m.steckerbrett, asize);
            }
        }
    }

  if (! have)
    return unit_no_score;      /* no hypothesis survived: never wins the merge */
  memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
  memcpy(m.steckerbrett, best_steck, asize);
  set_crib_stop_shown(best_at);   /* the progress line reports the winning alignment */
  return best;
}
/* --self-crib-seeds K: one rotor key's worth of terminal-signature seeding.

   Deduce under every (hypothesis, guess) pair, keep the DISTINCT surviving boards, rank
   them by the index of coincidence of their decrypt, and climb the top K with the deduced
   plugs pinned in PLUG_FIXED_EX -- the same per-worker pin set --exhaust and --crib use,
   since plug_fixed is a read-only global no worker may touch.

   Letters the deduction settles as carrying NO cable are pinned too: board[x] == x is a
   finding, not an absence of one, and marking it stops the climb spending moves on a
   letter that cannot be plugged.

   WHY RANK BEFORE CLIMBING, rather than climbing everything as --crib does. A long crib
   usually leaves one surviving hypothesis; this leaves ~28, and climbing all of them costs
   28x. Measured, the correct seed is ranked first by IC in 150 of 200 trials and the
   recovery curve against K is steeply diminishing -- on a real sweep K=1 already beats a
   cost-matched -R baseline 13/20 against 4/20, while K=5 buys three more recoveries for
   5x the compute. Hence a knob with a low default rather than an exhaustive pass.

   Cost is one decode per candidate (the ranking) plus K climbs. The decodes are ~1% of a
   climb, so K is the cost. Deterministic and -T-independent: the candidate order is
   hypothesis-major then guess, the dedupe keeps the first occurrence, and the sort is
   stable on (IC, index). */
double self_crib_unit(machine & m, size_t key_index, int restart)
{
  /* Per-thread scratch rather than stack arrays: sweeping yields thousands of seeds on a
     long message, which is far past what a per-call array can hold, and reusing the
     buffers keeps the allocation out of the per-key path. Cleared at entry, so each key's
     result depends on nothing but that key -- the -T-independence is unaffected. */
  static thread_local std::vector<unsigned char> sc_steck, sc_fixed;
  static thread_local std::vector<double> sc_ic;
  static thread_local std::vector<int> order;
  sc_steck.clear(); sc_fixed.clear(); sc_ic.clear(); order.clear();
  int nseed = 0;
  int board[asize];

  for (int hi = 0; hi < g_selfcrib_nhyps; hi++)
    for (int g = 0; g < asize; g++)
      {
        if (! self_crib_try(m, g_selfcrib_hyps[hi], g, board))
          continue;
        unsigned char st[asize], fx[asize];
        for (int i = 0; i < asize; i++)
          {
            st[i] = static_cast<unsigned char>((board[i] >= 0) ? board[i] : i);
            fx[i] = (board[i] >= 0) ? 1 : 0;
          }
        bool dup = false;
        for (int k = 0; (k < nseed) && ! dup; k++)
          dup = (memcmp(& sc_steck[static_cast<size_t>(k) * asize], st, asize) == 0) &&
                (memcmp(& sc_fixed[static_cast<size_t>(k) * asize], fx, asize) == 0);
        if (dup)
          continue;
        sc_steck.insert(sc_steck.end(), st, st + asize);
        sc_fixed.insert(sc_fixed.end(), fx, fx + asize);
        memcpy(m.steckerbrett, st, asize);
        sc_ic.push_back(ic_score_decode(m));  /* the ranking, one decode each */
        nseed++;
      }

  if (nseed == 0)
    return unit_no_score;      /* nothing survived: never wins the merge */

  /* Top K by IC, ties broken by the deterministic candidate order. Selection sort: K is
     small and nseed is ~28, so this is cheaper than sorting the whole list. */
  order.resize(static_cast<size_t>(nseed));
  for (int i = 0; i < nseed; i++)
    order[static_cast<size_t>(i)] = i;
  const int want = (opt_self_crib_seeds < nseed) ? opt_self_crib_seeds : nseed;
  for (int i = 0; i < want; i++)
    {
      size_t best_j = static_cast<size_t>(i);
      for (size_t j = static_cast<size_t>(i) + 1;
           j < static_cast<size_t>(nseed); j++)
        if (sc_ic[static_cast<size_t>(order[j])] >
            sc_ic[static_cast<size_t>(order[best_j])])
          best_j = j;
      const size_t ii = static_cast<size_t>(i);
      const int t = order[ii]; order[ii] = order[best_j]; order[best_j] = t;
    }

  double best = 0.0;
  bool have = false;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  for (int i = 0; i < want; i++)
    {
      const int si = order[static_cast<size_t>(i)];
      init_steckerbrett(m, opt_steckerbrett);      /* board = identity + -s */
      plug_fixed_ex_reset(m);    /* pins = -s / --no-plug ... */
      for (int x = 0; x < asize; x++)
        if (sc_fixed[static_cast<size_t>(si) * asize + static_cast<size_t>(x)])
          {
            m.steckerbrett[x] =
              sc_steck[static_cast<size_t>(si) * asize + static_cast<size_t>(x)];
            plug_fixed_ex_pin(m, x);               /* ... plus this deduction */
          }
      /* The kick is off by default and should stay off: a seeded climb starts near the
         answer, and -R 0 measured 201 of 204 exact recoveries at half the compute of
         -R 8. -R N still asks for N kicked passes if that is wanted. */
      if (opt_restarts >= 1)
        {
          uint64_t rng = restart_seed(key_index, restart);
          perturb_steckerbrett(m, & rng, opt_perturb);
        }
      const double sc = run_stages<true>(m);
      if (opt_dump_all)
        dump_all(m, sc);
      if (! have || (sc > best))
        {
          best = sc;
          have = true;
          memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(best_steck, m.steckerbrett, asize);
        }
    }
  if (! have)
    return unit_no_score;
  memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
  memcpy(m.steckerbrett, best_steck, asize);
  return best;
}
/* --crib-dump: one line per surviving hypothesis at this key -- "cribstop <key> <anchor>
   <letter> <plugs>" -- so a harness can check the deduced plugs against a known board
   (archived/cribs.md 10.1) and count stops (10.3). The rotor key is rebuilt from the caller's
   ring/start rather than read from the machine, because on the scan path setup_mapping
   has already stepped grundstellung (the documented lazy restore). Under the same mutex
   as --dump-all; display-only, so results stay -T-deterministic. */
void crib_dump(machine & m, int r1, int r2, int r3, int g1, int g2, int g3)
{
  char w[8], r[8], g[8];
  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  format_key(m, w, r, g);
  int board[asize];
  std::lock_guard<std::mutex> lock(dump_mutex());
  crib_edge ed[maxlen];
  for (int a = 0; a < crib_aligns; a++)
    {
      crib_edges_for(m, a, ed);
      for (int h = 0; h < asize; h++)
      {
        int anchor = crib_anchor_at[a];
        if (! crib_try(anchor, h, board, ed))
          continue;
        fprintf(stderr, "cribstop %s %s %s %d %c %c", w, r, g, crib_align[a] + 1,
                num2char(anchor), num2char(h));
        for (int x = 0; x < asize; x++)
          if (board[x] >= 0)
            fprintf(stderr, " %c%c", num2char(x), num2char(board[x]));
        fputc('\n', stderr);
      }
    }
}
/* How many alignments the crib can sit at. A crib run reports this in the
   settings echo and the cost table; nothing outside needs the list itself. */
int crib_alignment_count()
{
  return crib_aligns;
}

/* Run the deduction over every alignment and hypothesis for this key, counting
   what survives and how many letters each survivor pins. Factored out of the
   crib-cost estimator so the menu tables, crib_edge and crib_try stay private
   to this file -- the estimator lives with the search, which is where the
   sampling loop it sits in belongs. */
void crib_count_hypotheses(const machine & m, size_t & hyps, size_t & pins)
{
  std::vector<crib_edge> edbuf(static_cast<size_t>(crib_edges));
  crib_edge * ed = edbuf.data();
  int board[asize];
  for (int a = 0; a < crib_aligns; a++)
    {
      crib_edges_for(m, a, ed);
      for (int h = 0; h < asize; h++)
        if (crib_try(crib_anchor_at[a], h, board, ed))
          {
            hyps++;
            for (int i = 0; i < asize; i++)
              if (board[i] >= 0)
                pins++;
          }
    }
}

/* --crib-rerank's known-word list. Read-only once loaded; the finisher that
   reads it lives with the search. */
const std::vector<std::pair<std::string, double>> & crib_words()
{
  return g_cribs;
}

/* Drop the loaded known-word list. Used by the option-reset path, which
   restores defaults so a second parse in the same process starts clean. */
void crib_words_clear()
{
  g_cribs.clear();
}
