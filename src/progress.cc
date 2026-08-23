#include "progress.h"
#include "dedup.h"

#include "common.h"
#include "machine.h"
#include "options.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unistd.h>

best_result * g_progress = nullptr;

/* Progress-line columns (shared by the header and the lines): score right-aligned in 8
   (a 4-decimal score reaches 8 chars, e.g. "-12.7393", so 8 keeps the columns from
   shifting), reflector+wheels up to 5 chars (M4 "bB123"), ring and start 3 each --
   M4's 4-char ring/start simply print their extra character -- then the plugboard and
   the first preview_len characters of the decoded text.

   The plugboard column always reserves the full 13 pairs (38 chars), and every field is
   exactly as wide as its content, so each column is separated by a single space.

   The key is wider on the 4-wheel M4 (reflector+Greek+3 wheels = 5 chars, and a 4-char
   ring/start) than on a 3-wheel machine (4/3/3), so the two use their own format and
   their own preview length, each budgeted to land exactly on 80 columns:

     3-wheel: 8+1+4+1+3+1+3+1+38+1+19 = 80
     M4:      8+1+5+1+4+1+4+1+38+1+16 = 80

   The preview is what absorbs the difference -- it is truncated to whatever the key
   leaves, so a line never exceeds 80 columns for either machine. The header uses the
   same format, so its columns line up with the lines below it. */
static const int preview_len_3 = 19;   /* 3-wheel (standard / Norway) */
static const int preview_len_4 = 16;   /* 4-wheel M4: the wider key costs 3 characters */
static const int preview_max = 19;     /* buffer size: the larger of the two */
static const char progress_fmt_3[] = "%8s %-4s %-3s %-3s %-38s %s\n";
static const char progress_fmt_4[] = "%8s %-5s %-4s %-4s %-38s %s\n";
/* --crib adds an "A" column: which alignment the crib survived at. A crib run produces
   lines from many alignments and they are otherwise indistinguishable, so the line has to
   say which. The width comes out of the preview, the field the 80-column budget already
   designates as absorbing the difference between the two key layouts:
     3-wheel + crib: 8+1+4+1+3+1+3+1+38+1+3+1+15 = 80
     M4      + crib: 8+1+5+1+4+1+4+1+38+1+3+1+12 = 80 */
static const int preview_len_3c = 15;
static const int preview_len_4c = 12;
static const char progress_fmt_3c[] = "%8s %-4s %-3s %-3s %-38s %3s %s\n";
static const char progress_fmt_4c[] = "%8s %-5s %-4s %-4s %-38s %3s %s\n";

/* The alignment the current key's crib survived at, for the progress line. Display-only
   and per worker: set by search_worker once per key, read by showconfig from both the
   key-level merge and from inside a climb. thread_local rather than a `machine` member so
   struct machine's layout -- which the hot loops are documented to be sensitive to -- is
   left alone; nothing here is on a hot path. */
static thread_local int g_crib_stop_shown = -1;

/* Accessors rather than an extern thread_local: the value is written once per
   key and read once per printed line, so a call costs nothing, and it keeps
   the TLS access model a question this file answers alone. */
void set_crib_stop_shown(int alignment)
{
  g_crib_stop_shown = alignment;
}

int crib_stop_shown(void)
{
  return g_crib_stop_shown;
}

/* Longest doubling the scan looks for. W and V may not contain an X, so each is
   a single X-delimited token -- a WORD -- and the length distribution over the
   54 authentic decrypts is known: of the 25 carrying a doubling at all, the
   longest per message runs 6..13 and NOTHING reaches 14. The maximum is
   STUERZBAECHER at 13 (ENHANCEMENTS.md item 5, where the probes' own MAXLEN of
   16 was measured to saturate). So 30 is 2.3x anything observed.

   IT IS SET BY COST, NOT BY COVERAGE, and the cost difference between a
   generous cap and a tight one is below the noise floor. 20 was tried and
   reverted: it is 1.7x fewer passes on paper, but at the default gate only
   ~0.56% of keys reach the scan at all, and the wall-time difference did not
   resolve against a base-vs-base control. Given that, the wider cap is free
   insurance -- a doubling ABOVE the cap is MISSED rather than truncated (a long
   repeat does not decompose into a shorter matching one; sliding the window
   puts the copies out of alignment), and 54 messages is a thin basis for
   ruling out 14..30 entirely.

   What the cap must not be is absent: without one the scan runs every length
   from (n-1)/2 down to L, which is O(n^2) and grows with the message -- 193
   linear passes at 400 letters against 24 here, and a measured +7.6% of a run
   ungated at 200 letters against noise with the cap in place.

   A second effect argues mildly for a tighter cap and is recorded for
   completeness: the rule tolerates ONE mismatch across 2L letters, so a longer
   doubling is more likely to be disqualified by a second garble. Of the 25 real
   doublings 18 have no mismatch and 7 have exactly one, putting the effective
   per-letter rate near 2%; at that rate P(<=1 mismatch) falls from 90% at L=13
   to 81% at L=20 and 66% at L=30. It only bites at lengths that do not occur,
   so it does not decide the constant.

   --doubling-report is validated against this, so the two can never disagree and
   a too-large L cannot silently search nothing. */
extern const int doubling_maxlen;
const int doubling_maxlen = 30;

/* The column header is printed once per PROCESS, not once per best_result.
   --crib-list runs one sweep (and so one best_result) per crib, and the columns are
   identical between them, so a per-sweep flag would reprint the header for every
   crib. Only one best_result is live at a time -- the cribs run in sequence and the
   --ring-stride refinement runs after its search has joined -- so writing this under
   whichever mutex is current is safe. */
static bool g_header_shown = false;

/* Highest score echoed by ANY sweep so far, for the same reason: --crib-list runs one
   sweep per crib and the progress lines are one stream to the reader, so the mark that
   suppresses a repeat has to outlive a single best_result. Read and written between
   sweeps only (the cribs run in sequence), never on the hot path. */
double g_shown_high = score_min;

/* --- --confidence: the calibrated null, shared with the progress line
   ------------ Set once per process by calibrate_null() before the first sweep,
   read by progress_line() to turn a raw score into a MARGIN over what the whole
   search reaches by chance. Zero sd means "not calibrated"; every reader tests
   that.

   Why the margin and not a bare z: a progress line is a running MAXIMUM over
   the keys seen so far, so a bare z reads 3-5 sigma on the early lines -- which
   looks like p < 1e-5 and is exactly what chance delivers over a few thousand
   keys. The margin subtracts the chance best of the WHOLE key space, so zero is
   the meaningful line and a positive number means the board beats what the
   entire sweep would produce by luck.

   g_null_zk uses the TOTAL key count, never keys-so-far. That keeps the margin
   a constant offset from the score -- monotone, so the merge order and the
   display high-water mark are untouched -- and keeps the printed number
   independent of thread timing. A running count would make the number itself
   -T-dependent, which
   is a worse thing to print than the existing "which lines appear". */
double g_null_mu = 0.0;
double g_null_sd = 0.0;
double g_null_zk = 0.0;   /* sqrt(2 ln K), the chance best in sigmas */
size_t g_null_n = 0;      /* samples behind mu/sd, for the summary */
/* The K that g_null_zk was computed from. Kept so the summary reports the count
   its own bar used rather than being handed one: passing the key count in let
   the two drift, and under --ring-stride they did -- the caller passed the
   refinement's keys too, so the line read "chance best of 1528334 keys is
   5.3 sd" with 5.3 computed for 1527084. The error was 0.00015 sd (zk grows as
   sqrt(ln K), so it barely moves), but the fix removes the possibility rather
   than the instance. It cannot go the other way and fold the refinement into
   zk: the progress lines need this number BEFORE the sweep, so the margin stays
   a constant offset from the score -- and the refinement's keys are chosen
   conditional on the coarse winner, so they are not the independent draws
   sqrt(2 ln K) is about. */
size_t g_null_keys = 0;

static inline const char * progress_fmt(void)
{
  if (opt_crib_text)
    return opt_m4 ? progress_fmt_4c : progress_fmt_3c;
  return opt_m4 ? progress_fmt_4 : progress_fmt_3;
}

static inline int preview_len(void)
{
  if (opt_crib_text)
    return opt_m4 ? preview_len_4c : preview_len_3c;
  return opt_m4 ? preview_len_4 : preview_len_3;
}

/* Column header, printed once before the first progress line of a search. */
void showconfig_header(void)
{
  /* "Margin" when --confidence recalibrated the first column (showconfig). */
  const char * col = (g_null_sd > 0.0) ? "Margin" : "Score";
  if (opt_crib_text)
    fprintf(stderr, progress_fmt(), col, "W", "R", "G", "S", "A", "Text");
  else
    fprintf(stderr, progress_fmt(), col, "W", "R", "G", "S", "Text");
}

/* Format m's rotor key into w (reflector+wheels), r (ring), g (start) -- the columns
   showconfig prints, with the M4 Greek-offset and Norway-reflector handling. Shared with
   the --dump-all diagnostic so the two can never diverge. */
void format_key(machine & m, char (&w)[8], char (&r)[8], char (&g)[8])
{
  if (opt_m4)
    {
      /* M4: thin reflector (b/c) + static Greek wheel (B/G). Only the Greek
         (start - ring) offset is identifiable, so it is shown as start=offset,
         ring=A. The reflector/Greek/ring/start columns list the Greek first.
         Wheel numbers are single digits (1-8), printed as chars. */
      snprintf(w, sizeof(w), "%c%c%c%c%c",
               (m.ukw == m4_thin_base) ? 'b' : 'c',
               (m.greek == greek_base) ? 'B' : 'G',
               '1' + m.walzenlage[0],
               '1' + m.walzenlage[1],
               '1' + m.walzenlage[2]);
      snprintf(r, sizeof(r), "A%c%c%c",
               num2char(m.ringstellung[0]),
               num2char(m.ringstellung[1]),
               num2char(m.ringstellung[2]));
      snprintf(g, sizeof(g), "%c%c%c%c",
               num2char(m.greek_offset),
               num2char(m.grundstellung[0]),
               num2char(m.grundstellung[1]),
               num2char(m.grundstellung[2]));
    }
  else
    {
      /* display wheel numbers 1..N: standard rotors are index+1, Norway wheels
         are index - norway_rotor_base + 1; the reflector prints as its letter
         (N for Norway, else A/B/C). */
      int wheel_offset = opt_norenigma ? 1 - norway_rotor_base : 1;
      snprintf(w, sizeof(w), "%c%c%c%c",
               opt_norenigma ? 'N' : num2char(m.ukw),
               static_cast<char>('0' + m.walzenlage[0] + wheel_offset),
               static_cast<char>('0' + m.walzenlage[1] + wheel_offset),
               static_cast<char>('0' + m.walzenlage[2] + wheel_offset));
      snprintf(r, sizeof(r), "%c%c%c",
               num2char(m.ringstellung[0]),
               num2char(m.ringstellung[1]),
               num2char(m.ringstellung[2]));
      snprintf(g, sizeof(g), "%c%c%c",
               num2char(m.grundstellung[0]),
               num2char(m.grundstellung[1]),
               num2char(m.grundstellung[2]));
    }
}

/* --full-text: print the whole decrypted message below the progress line, wrapped and
   indented so it reads as a continuation of that line rather than as another one.
     Decoded on the fly from m's CURRENT board for the same reason the preview is:
   m.plaintext holds an earlier candidate while a climb is running. Called from
   showconfig(), so it inherits the best-result mutex and cannot interleave with another
   thread's output. */
static const int full_text_indent = 2;
/* Chosen so indent + width == the progress line's own width, which every
   variant of progress_fmt lands on exactly: 61 + 19 for 3 wheels, 64 + 16 for
   M4, 65 + 15 and 68 + 12 with the crib column. The full text then wraps
   against the same right margin as the preview it replaces, so the two read as
   one block. It was 76 (= 78 with the indent) from a time when the target was a
   79-column terminal; the progress lines were budgeted to 80 afterwards and the
   two were never reconciled, leaving the continuation 2 columns short. */
static const int full_text_width = 78;

static void show_full_text(machine & m)
{
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  char line[full_text_width + 1];
  for (int i = 0; i < textlength; i += full_text_width)
    {
      int n = textlength - i;
      if (n > full_text_width)
        n = full_text_width;
      for (int j = 0; j < n; j++)
        line[j] = num2char(decode_at(steck, rows, num_ciphertext, i + j));
      line[n] = 0;
      fprintf(stderr, "%*s%s\n", full_text_indent, "", line);
    }
}

/* Format m's plugboard into s: canonical (each pair low-high, pairs ordered by low
   letter), so a harness can dedupe boards by string equality. */
/* The buffer holds exactly 13 pairs -- "AB CD ... YZ" plus the NUL -- which is all a
   plugboard can have, since it is an INVOLUTION on 26 letters. The bound is therefore
   unreachable on any valid board, and is here because a display helper must not smash
   the stack when handed an invalid one: a non-involution can satisfy steckerbrett[j] > j
   for up to 25 letters. That was not hypothetical -- --crib with -s used to build such a
   board (see crib_try's known-plug seeding) and this function was where it crashed, with
   a "stack smashing detected" abort that named nothing useful. Truncating is the right
   failure here: the board is already wrong, and a diagnostic line is not the place to
   discover it. */
void format_plugboard(machine & m, char (&s)[3 * 13])
{
  char * p = s;
  const char * end = s + sizeof s;
  for (int j = 0; j < asize; j++)
    if (m.steckerbrett[j] > j)
      {
        if ((p + ((p > s) ? 3 : 2)) >= end)
          break;
        if (p > s)
          *p++ = ' ';
        *p++ = num2char(j);
        *p++ = num2char(m.steckerbrett[j]);
      }
  *p = 0;
}

/* The first column of a progress line, shared by showconfig and the
   --doubling-report report so the two can never disagree about what it means.

   Under --confidence it is the MARGIN over the whole search's chance best, not
   the raw score: a raw score cannot answer "is this good yet?", and a bare z
   would flatter the early lines (see the g_null_* comment). The header says
   "Margin" to match, so a saved log stays self-describing. --dump-all keeps raw
   scores as the machine-readable form.

   The column is 8 wide and the whole line is budgeted to land on 80, so a margin
   that does not fit must not be printed as-is -- it would shift every column
   after it. A real margin is well under 100 (the widest measured is +21), so the
   %e fallback only fires on a null too degenerate for calibrate_null's guard to
   have caught, and it keeps the field both readable and exactly 8. */
static void format_score(double score, char (&buf)[16])
{
  if (g_null_sd > 0.0)
    {
      snprintf(buf, sizeof(buf), "%+.2f",
               (score - g_null_mu) / g_null_sd - g_null_zk);
      if (strlen(buf) > 8)
        snprintf(buf, sizeof(buf), "%+.1e",
                 (score - g_null_mu) / g_null_sd - g_null_zk);
    }
  else
    {
      snprintf(buf, sizeof(buf), "%.4f", score);
      /* The same guard the margin gets, for the same reason. A raw per-symbol
         score is bounded below by the table's own minimum (ngram_bias), which
         is about -10 for a single order and about -14 measured for the
         weighted mixture -- exactly 8 characters, i.e. NO slack. The bundled
         tables never overflow, but ENIGMA_LOGLIN scales the quad table by
         arbitrary weights and did: x10 printed -142.3724 and shifted every
         column after it, while the margin branch held at 80 because it was
         guarded and this was not. An 8-wide field with two printers must have
         the same bound in both. */
      if (strlen(buf) > 8)
        snprintf(buf, sizeof(buf), "%.1e", score);
    }
}

void showconfig(machine & m, double score)
{
  char w[8], r[8], g[8], s[3 * 13], text[preview_max + 1];

  format_key(m, w, r, g);
  format_plugboard(m, s);

  /* Decode the text preview on the fly from the machine's CURRENT board --
     m.plaintext can be stale here (inside a running climb it still holds an
     earlier candidate). */
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  const int plen = preview_len();
  int n = (textlength < plen) ? textlength : plen;
  for (int i = 0; i < n; i++)
    text[i] = num2char(decode_at(steck, rows, num_ciphertext, i));
  text[n] = 0;

  char scorebuf[16];   /* margin under --confidence; see format_score */
  format_score(score, scorebuf);
  if (opt_crib_text)
    {
      char at[16];   /* wide enough for any int, so no truncation warning */
      /* 1-based, matching --crib-at: a displayed alignment must be a value the
         reader can type straight back in. */
      snprintf(at, sizeof(at), "%d", g_crib_stop_shown + 1);
      fprintf(stderr, progress_fmt(), scorebuf, w, r, g, s, at, text);
    }
  else
    fprintf(stderr, progress_fmt(), scorebuf, w, r, g, s, text);

  if (opt_full_text)
    show_full_text(m);
}

/* Serialises the --dump-all diagnostic lines; display-only, so results stay
   -T-deterministic. */
static std::mutex g_dump_mutex;

std::mutex & dump_mutex()
{
  return g_dump_mutex;
}

/* --dump-all: emit one converged (rotor key, restart) climb's FULL setting -- the rotor
   key (reflector+wheels / ring / start), the score, and the plugboard -- so a wildcarded
   search can be inspected key-by-key. Reuses showconfig's format_key/format_plugboard so
   the rotor key matches the progress line exactly (the climb path restored the true start
   positions, so the key is correct here). Under the shared mutex; display-only. */
void dump_all(machine & m, double score)
{
  char w[8], r[8], g[8], s[3 * 13];
  format_key(m, w, r, g);
  format_plugboard(m, s);
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  fprintf(stderr, "dumpall %s %s %s %.4f %s\n", w, r, g, score, s);
}

/* --- live sweep progress -----------------------------------------------------
   The score lines say how WELL the search is doing and nothing about how far it
   has come: on a big key space they thin out to nothing precisely when the run
   is longest, so there is no way to tell a slow sweep from a stuck one. This is
   the other half -- a single \r line carrying percentage, rate and ETA.

   TTY-only, like the -F tier-1 line, so redirected logs and the tests stay
   clean. Also suppressed under --dump-all, whose output is the machine-readable
   form the harnesses parse and must not have a \r line interleaved into it
   (--dump-all prints under its own mutex, so it could not be sequenced with
   this one anyway).

   g_sweep_total == 0 means "not armed" and is the only thing the hot path
   tests. bruteforce() arms it around the main sweep alone: the --ring-stride
   refinement reuses search_worker over its own small key space, and leaving the
   counter armed would let it run the percentage past 100. */
static std::atomic<size_t> g_sweep_done{0};
static size_t g_sweep_total = 0;        /* work items in the armed sweep */
static size_t g_sweep_restarts = 1;     /* items per key, so counts read as keys */
/* The sweep's start, held as the clock's RAW TICK COUNT rather than a
   time_point.  A static-duration object whose constructor is formally
   throwing cannot have that exception caught anywhere -- which is what
   clang-tidy's bugprone-throwing-static-initialization objects to, and
   steady_clock::time_point's default constructor is only constexpr, not
   noexcept.  Nothing here needs a time_point: the value is written once and
   only ever subtracted from a later reading, so the representation suffices
   and zero is a valid "not armed yet". */
static long long g_sweep_t0_ticks = 0;
static bool g_sweep_drawn = false;      /* a \r line is on screen; under best.mutex */
static int g_sweep_width = 0;           /* its length, so the erase matches exactly */
/* ms since g_sweep_t0 of the last redraw, so the line can be rate-limited by
   TIME rather than only by percentage of work done. Atomic because the check
   runs on every worker outside the mutex. */
static std::atomic<long long> g_sweep_last_ms{-1000000};

/* Counts span six orders of magnitude between a 676-key sweep and a full M4
   wildcard, and the line has to stay a fixed width, so they are abbreviated. */
static void fmt_count(char * buf, size_t n, double v)
{
  if (v < 10000.0)
    snprintf(buf, n, "%.0f", v);
  else if (v < 1e6)
    snprintf(buf, n, "%.1fk", v / 1e3);
  else if (v < 1e9)
    snprintf(buf, n, "%.2fM", v / 1e6);
  else
    snprintf(buf, n, "%.2fG", v / 1e9);
}

static void fmt_eta(char * buf, size_t n, double secs)
{
  if (!(secs >= 0.0) || (secs >= 86400.0 * 99))
    snprintf(buf, n, "?");
  else if (secs < 60.0)
    snprintf(buf, n, "%.0fs", secs);
  else if (secs < 3600.0)
    snprintf(buf, n, "%dm%02ds", static_cast<int>(secs) / 60,
             static_cast<int>(secs) % 60);
  else if (secs < 86400.0)
    snprintf(buf, n, "%dh%02dm", static_cast<int>(secs) / 3600,
             (static_cast<int>(secs) % 3600) / 60);
  else
    snprintf(buf, n, "%.0fd%02dh", floor(secs / 86400.0),
             (static_cast<int>(secs) % 86400) / 3600);
}

/* Erase the \r line so an ordinary stderr line can be printed over it. The
   caller holds best.mutex, which is what every score line is already printed
   under -- so this is the one place the two streams meet. */
void sweep_progress_clear(void)
{
  if (! g_sweep_drawn)
    return;
  /* Exactly as wide as what was drawn, not a blanket 79: on a terminal narrower
     than the erase the extra spaces wrap, and the \r then returns to the start
     of the SECOND line, leaving the first one dirty. */
  fprintf(stderr, "\r%*s\r", g_sweep_width, "");
  g_sweep_drawn = false;
}

/* Account for `n` finished work items and redraw. One relaxed atomic add per
   tick_block items.

   REDRAW ON A CLOCK, not on a 1% boundary. A percentage boundary is the wrong
   clock: 1% of the work takes longer the bigger the sweep, so the line updated
   most rarely on exactly the runs that need it most. Measured at the climb rate
   of ~1800 keys/s, that was one update every 5.8 s over 1.05M keys but every
   2.5 MINUTES over 27.4M and every 21 minutes over 230M -- long enough that the
   first line looks like a hang. A fixed interval gives one cadence whatever the
   keyspace, and the only draw exempt from it is the last, so the line always
   finishes at 100%. */
void sweep_progress_tick(size_t n, best_result & best)
{
  const size_t total = g_sweep_total;
  if (total == 0)
    return;
  const size_t after = g_sweep_done.fetch_add(n, std::memory_order_relaxed) + n;
  const bool final_draw = (after >= total);

  const double el = std::chrono::duration<double>
    (std::chrono::steady_clock::duration
       (std::chrono::steady_clock::now().time_since_epoch().count()
        - g_sweep_t0_ticks)).count();
  /* A sweep that finishes in well under a second should not flash a progress
     line up and wipe it again; and an ETA off the first few milliseconds is
     noise anyway. */
  if (el < 0.5)
    return;

  /* Checked before taking the mutex, so an ordinary tick costs one relaxed
     load. Five seconds is slow enough to read and fast enough to show a run is
     alive; a sub-second cadence just makes the terminal churn. */
  const long long now_ms = static_cast<long long>(el * 1000.0);
  const long long redraw_gap = 5000;
  if (! final_draw
      && (now_ms - g_sweep_last_ms.load(std::memory_order_relaxed)
          < redraw_gap))
    return;
  /* Claim the slot: whichever thread wins the exchange draws, the others
     return. Without this, every worker that passed the test above would queue
     on the mutex and redraw the same line in turn. */
  if (final_draw)
    {
      /* The last item always draws, so the line ends at 100% rather than
         wherever the clock happened to leave it. */
      g_sweep_last_ms.store(now_ms, std::memory_order_relaxed);
    }
  else
    {
      long long prev = g_sweep_last_ms.load(std::memory_order_relaxed);
      if (now_ms - prev < redraw_gap)
        return;
      if (! g_sweep_last_ms.compare_exchange_strong(prev, now_ms,
                                                    std::memory_order_relaxed))
        return;   /* another worker claimed this slot; only one draws */
    }

  /* Reading the shared counter rather than this thread's `after`: other
     workers have advanced it since, and the displayed percentage should be the
     sweep's, not this thread's view of it. */
  const size_t shown = g_sweep_done.load(std::memory_order_relaxed);
  const size_t did = (shown < total) ? shown : total;
  /* Restart is the OUTER dimension, so the counts have to be read per PASS.
     Dividing the item count by the restarts -- which is what this did while
     restarts were innermost, and is why g_sweep_restarts exists -- would report
     6% of keys covered at the point where every key has been visited once,
     i.e. exactly the fact the reader is watching for. Within-pass counts plus
     the pass number say it directly. The rate is key-VISITS per second (one
     item = one visit) and the ETA runs to the end of the whole sweep, both
     unchanged in meaning from the single-restart case. */
  const size_t per_pass = (g_sweep_restarts > 0) ? (total / g_sweep_restarts)
                                                 : total;
  const size_t pass = (per_pass > 0) ? (did / per_pass) : 0;
  const double done_keys = static_cast<double>((per_pass > 0)
                                               ? (did % per_pass) : did);
  const double all_keys = static_cast<double>(per_pass);
  const double rate = static_cast<double>(did) / el;
  char db[16], tb[16], rb[16], eb[16];
  fmt_count(db, sizeof(db), (did == total) ? all_keys : done_keys);
  fmt_count(tb, sizeof(tb), all_keys);
  fmt_count(rb, sizeof(rb), rate);
  fmt_eta(eb, sizeof(eb),
          (rate > 0.0) ? static_cast<double>(total - did) / rate : -1.0);

  /* Generous: the four fields are bounded by their own 16-byte buffers, but the
     compiler cannot see that and warns on the sum. */
  char line[192];
  /* Two size_t, "pass ", "/" and ", " -- 48 is generous, but the compiler
     cannot prove the pass number is small and warns on the sum otherwise. */
  char pb[64] = "";
  if (g_sweep_restarts > 1)
    snprintf(pb, sizeof(pb), "pass %zu/%zu, ",
             ((pass < g_sweep_restarts) ? pass : g_sweep_restarts - 1) + 1,
             g_sweep_restarts);
  /* --seed-dedup: the skip rate matters mid-run, not just in the final line,
     because the runs this is for last days -- and it is the one number that
     says whether the filter is earning its memory. Appended rather than
     inserted so the fields before it stay where a reader's eye expects them,
     and it is the first field to drop if a later one pushes the line past 80
     columns (the final report always carries it). */
  char sd[32] = "";
  if (seed_dedup_on())
    {
      const uint64_t sk = seed_dedup_skipped();
      const uint64_t se = seed_dedup_seeds();
      if (se > 0)
        snprintf(sd, sizeof(sd), ", %.0f%% dup",
                 100.0 * static_cast<double>(sk) / static_cast<double>(se));
    }
  snprintf(line, sizeof(line),
           "Progress:  %3zu%% (%s%s / %s keys) %s/s, %s left%s",
           (shown >= total) ? 100 : (shown * 100) / total, pb, db, tb, rb, eb,
           sd);

  std::lock_guard<std::mutex> lock(best.mutex);
  /* Padded to the widest line drawn so far, and erased at that width too. The
     line can SHRINK (999k/s -> 1.0M/s, 10m00s -> 9m59s), and a bare \r plus a
     shorter string would leave the tail of the previous one on screen. */
  const int w = static_cast<int>(strlen(line));
  if (w > g_sweep_width)
    g_sweep_width = w;
  fprintf(stderr, "\r%-*s", g_sweep_width, line);
  fflush(stderr);
  g_sweep_drawn = true;
}

/* Print one progress line under the best-result mutex, emitting the column
   header before the first line of the run. */
void progress_line(best_result & b, machine & m, double score)
{
  sweep_progress_clear();   /* the \r line must not be printed over */
  if (! b.header_shown && ! g_header_shown)
    {
      b.header_shown = true;
      g_header_shown = true;
      showconfig_header();
    }
  showconfig(m, score);
}

/* --doubling-report: longest W X V in `pt[0..n)` with |W| = |V| = len >= minlen, no X
   inside either half, and at most ONE mismatched letter between the halves.
   Returns len (0 if none) and sets *at to W's offset.

   Telegraphic German doubles an important word around the X separator --
   "ROMANOWO X ROMANOWO" -- as the operator's own error correction, so its
   presence in a decrypt is evidence the decrypt is real German rather than a
   plugboard climb's high-scoring noise.

   ONE mismatch is tolerated because of the CHANNEL, and it buys exactly the
   error the channel produces. Enigma has no diffusion, so one corrupted
   ciphertext letter damages exactly one plaintext letter -- in one copy of the
   doubling and not the other. A transmission garble is therefore a
   SUBSTITUTION, which is what this tolerates.
     It does NOT cover an operator dropping or adding a letter: an indel
   misaligns the two copies, so every position after it differs and |W| != |V|
   besides. Real traffic does contain those -- the Nr 173 form doubles a surname
   as SCUHNACHER (10) against SCHUHMACHER (11) -- and this rule misses them by
   design, since widening to indels would mean an edit distance and a null far
   thinner than the 16x-per-letter one the L threshold is priced against
   (ENHANCEMENTS.md item 5(d)).

   The LONGEST match is reported rather than a count: the len and len-1 windows
   inside one doubled word are the same fact, not independent evidence.

   Linear per X rather than quadratic: extend outward from the separator, one
   pair at a time, and stop at the second mismatch or the first X. The obvious
   "try every length at every X" form is O(n^3) and would cost more than the
   climb it is reporting on. */
static int find_doubling(const char * pt, int n, int minlen, int maxmm,
                         int * at)
{
  /* Longest first, so the first hit is the answer and the scan stops. Capped at
     doubling_maxlen: that is what keeps the cost O(maxlen * n) instead of
     O(n^2), and no real doubling comes close (see the constant). */
  int start = (n - 1) / 2;
  if (start > doubling_maxlen)
    start = doubling_maxlen;
  for (int len = start; len >= minlen; len--)
    {
      /* A doubling is a TRANSLATION by len+1, not a reflection: W[i] sits at
         pt[j-len+i] and V[i] at pt[j+1+i], so the pair is (y, y+len+1) for
         every y in W. Extending OUTWARD from the separator instead compares W
         reversed against V, which matches only palindromes -- the first version
         of this did exactly that and reported nothing on ENGELMANN X
         ENGELMANN. */
      const int d = len + 1;
      int nbad = 0, nmis = 0;   /* over the sliding window of len pairs */
      for (int y = 0; y + d < n; y++)
        {
          if ((pt[y] == 'X') || (pt[y + d] == 'X'))
            nbad++;
          if (pt[y] != pt[y + d])
            nmis++;
          if (y >= len)
            {
              const int z = y - len;
              if ((pt[z] == 'X') || (pt[z + d] == 'X'))
                nbad--;
              if (pt[z] != pt[z + d])
                nmis--;
            }
          /* The window now holds the len pairs ending at y, i.e. W = pt[y-len+1
             .. y] and V = pt[y+2 .. y+d], with the separator at j = y+1. */
          if (y >= len - 1)
            {
              const int j = y + 1;
              if ((pt[j] == 'X') && (nbad == 0) && (nmis <= maxmm))
                {
                  * at = j - len;
                  return len;
                }
            }
        }
    }
  return 0;
}

/* --doubling-report L: report a converged climb whose score clears the z gate AND
   whose decrypt carries a doubling of at least L letters.

   A CONFIRMATION SIGNAL, never a score term. It cannot promote a wrong key
   because it does not enter any ranking -- so unlike the score-bonus form
   (ENHANCEMENTS.md 5(e), swept and measured down) a false positive costs the
   reader a second look and nothing else, and a non-firing says nothing.

   THE Z GATE COMES FIRST, and that is what makes it free. Computing z is two
   flops on a value already in hand; only ~0.56% of keys clear z > 3, and only
   those pay the decode and the scan. Ordering it the other way would decode
   every converged climb.

   The line is the ordinary progress line with the text preview replaced by
   ">> <len> <WORD>", so the columns stay aligned with everything else in the
   log and the marker makes it greppable. It is NOT a new-best announcement:
   these fire on any key past the gate, whatever the search's high-water mark,
   which is the whole point -- the true key can be reported while some other
   board still leads on score.

   Display-only, under the same mutex as the progress lines, so which lines
   appear is thread-timing dependent exactly as the existing echo is, and which
   candidate WINS is untouched. */
void report_doubling(machine & m, double score)
{
  if (opt_doubling_report <= 0)
    return;
  /* A degenerate null (a one-key space) leaves g_null_sd at 0 and z undefined;
     calibrate_null already falls back to raw scores there, so there is nothing
     to gate on and the report stays silent rather than dividing by ~1e-15. */
  if (g_null_sd <= 0.0)
    return;
  if (((score - g_null_mu) / g_null_sd) < opt_doubling_z)
    return;

  /* Decode in full from the machine's CURRENT board: m.plaintext is stale here
     on every path but --tune-phase (the scorers fuse the decode and never write
     it back), the same reason showconfig decodes its preview on the fly. */
  char pt[maxlen + 1];
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  for (int i = 0; i < textlength; i++)
    pt[i] = num2char(decode_at(steck, rows, num_ciphertext, i));
  pt[textlength] = 0;

  int at = 0;
  const int len = find_doubling(pt, textlength, opt_doubling_report,
                                opt_doubling_mismatches, & at);
  if (len <= 0)
    return;

  char w[8], r[8], g[8], sb[3 * 13], scorebuf[16];
  format_key(m, w, r, g);
  format_plugboard(m, sb);
  format_score(score, scorebuf);
  /* Sized on doubling_maxlen, the real bound: find_doubling() clamps its scan
     to it, so len can never exceed 30 however long the message is. These used
     to be maxlen/2 (512 bytes) -- 17x anything reachable -- and gcc 14+ warned
     that the snprintf below "may be truncated", because it can only reason
     from the BUFFER size and 3 + 11 + 1 + 512 overflows 528. Sizing to the
     true bound is both honest and what silences it; padding the destination
     instead would have hidden the mismatch rather than fixed it. The +20
     covers ">> " plus the widest %d an int can print. */
  char word[doubling_maxlen + 1];
  memcpy(word, pt + at, static_cast<size_t>(len));
  word[len] = 0;
  char text[doubling_maxlen + 20];
  snprintf(text, sizeof(text), ">> %d %s", len, word);

  /* Collapse an identical repeat. One rotor key gets a call per converged
     restart plus one after --polish, and restarts that reach the same board
     produce the same line, so the true key would otherwise print -R+1 identical
     rows. Only an EXACT repeat of the PREVIOUS line is dropped -- a different
     key, board or word always prints -- and the suppression is best-effort: two
     threads interleaving can still let one through. Display state only, so it
     cannot affect which candidate wins. */
  static char last[256];

  char line[256];
  if (opt_crib_text)
    {
      char a[16];
      snprintf(a, sizeof(a), "%d", g_crib_stop_shown + 1);
      snprintf(line, sizeof(line), progress_fmt(), scorebuf, w, r, g, sb, a,
               text);
    }
  else
    snprintf(line, sizeof(line), progress_fmt(), scorebuf, w, r, g, sb, text);

  best_result * bp = g_progress;
  /* bp is null only outside a search; the --polish call site runs inside one
     (bruteforce sets g_progress before the workers and clears it at the end),
     so the lock is taken there too and cannot deadlock -- nothing holds the
     mutex across either call site. `last` is read and written under it. */
  std::unique_lock<std::mutex> lock;
  if (bp != nullptr)
    lock = std::unique_lock<std::mutex>(bp->mutex);
  if (strcmp(line, last) == 0)
    return;
  snprintf(last, sizeof(last), "%s", line);
  sweep_progress_clear();   /* the \r line must not be printed over */
  if (! g_header_shown)
    {
      g_header_shown = true;
      if (bp != nullptr)
        bp->header_shown = true;
      showconfig_header();
    }
  fputs(line, stderr);
  /* --full-text applies here for the same reason it applies to a progress line:
     the report says a doubling is present, and the whole decrypt is what lets
     the reader judge it. Without this the option silently skipped the one line
     most worth expanding. */
  if (opt_full_text)
    show_full_text(m);
}

/* Echo an intermediate plugboard improvement from inside a climb: the same
   progress line the key-level merge prints, but at the granularity the user
   actually watches -- every accepted climb move that beats everything echoed so
   far, not just every finished climb. Gated to the TARGET scoring model (a staged
   pre-pass and the -F tier-1 filter climb score in a different model, so their values
   are not comparable with the ranking scores) and to workers that opted in
   (m.report). Costs one relaxed atomic load per ACCEPTED move -- nothing on the
   325-move scoring scans -- so the hot path is untouched. The worker restored the
   machine's grundstellung right after setup_mapping (climb paths only), so
   showconfig() prints the true start positions here. */
void report_climb_progress(machine & m, double score)
{
  best_result * bp = g_progress;
  if ((bp == nullptr) || (! m.report) || (m.scoring != opt_scoring))
    return;
  if (score <= bp->shown.load(std::memory_order_relaxed))
    return;
  std::lock_guard<std::mutex> lock(bp->mutex);
  if (score <= bp->shown.load(std::memory_order_relaxed))
    return;   /* another thread echoed something at least as good meanwhile */
  bp->shown.store(score, std::memory_order_relaxed);
  progress_line(* bp, m, score);
}

/* Arming is bruteforce()'s job and covers the MAIN sweep only: the
   --ring-stride refinement reuses search_worker over its own key space and
   would otherwise push the percentage past 100, and --crib-list runs one sweep
   per crib, each of which gets its own 0-100%. */
void sweep_progress_arm(size_t total_items, size_t restarts)
{
  g_sweep_done.store(0, std::memory_order_relaxed);
  g_sweep_last_ms.store(-1000000, std::memory_order_relaxed);
  g_sweep_restarts = (restarts > 0) ? restarts : 1;
  g_sweep_t0_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  g_sweep_total = total_items;
}

bool sweep_progress_armed(void)
{
  return g_sweep_total != 0;
}

void sweep_progress_disarm(void)
{
  g_sweep_total = 0;
}
