#include "common.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

[[noreturn]] void fatal(const char * message)
{
  fprintf(stderr, "\nFatal error: %s\n", message);
  exit(1);
}

/* Shared by the three parsers below. The offending text is quoted back but
   TRUNCATED: an option argument is attacker-free here, yet it is still
   unbounded user input reaching a fixed buffer, and a message that says
   `"abc" is not a number` is no more useful at 400 characters than at 24. */
[[noreturn]] static void bad_number(const char * s, const char * what,
                                    const char * why)
{
  char msg[160];
  snprintf(msg, sizeof(msg), "Illegal value for %s: \"%.24s%s\" %s",
           what, s, (strlen(s) > 24) ? "..." : "", why);
  fatal(msg);
}

/* strtol reports three distinct failures and they need telling apart, because
   only one of them is "you typed a word": nothing consumed (end == s), text
   left over (*end), and out of range (ERANGE). errno must be cleared first --
   strtol only ever SETS it. */
static long parse_long(const char * s, const char * what)
{
  if ((s == nullptr) || (*s == 0))
    bad_number("", what, "is empty");

  errno = 0;
  char * end = nullptr;
  const long v = strtol(s, & end, 10);

  if (end == s)
    bad_number(s, what, "is not a number");
  if (*end != 0)
    bad_number(s, what, "has trailing characters");
  if (errno == ERANGE)
    bad_number(s, what, "is out of range");

  return v;
}

int parse_opt_int(const char * s, const char * what)
{
  const long v = parse_long(s, what);

  /* Narrowing is checked here rather than left to the caller's bounds test.
     Every caller does bound its value, but a silent wrap would arrive there as
     a plausible in-range number -- which is the same class of bug this whole
     helper exists to remove, one level down. */
  if ((v < INT_MIN) || (v > INT_MAX))
    bad_number(s, what, "is out of range");

  return static_cast<int>(v);
}

double parse_opt_double(const char * s, const char * what)
{
  if ((s == nullptr) || (*s == 0))
    bad_number("", what, "is empty");

  errno = 0;
  char * end = nullptr;
  const double v = strtod(s, & end);

  if (end == s)
    bad_number(s, what, "is not a number");
  if (*end != 0)
    bad_number(s, what, "has trailing characters");
  if (errno == ERANGE)
    bad_number(s, what, "is out of range");

  return v;
}

/* -e only. Separate from parse_opt_int because a seed is genuinely a 64-bit
   quantity: the RNG stream is indexed by opt_seed + key index, and the echo
   prints values well past INT_MAX for a random_device draw, so a run reported
   as `seed: 13127098431578446302` has to be reproducible by passing that back
   in. strtoull would accept a leading '-' and wrap it, which is not what
   anyone typing a seed means, so it is rejected before the conversion. */
uint64_t parse_opt_u64(const char * s, const char * what)
{
  if ((s == nullptr) || (*s == 0))
    bad_number("", what, "is empty");
  if (*s == '-')
    bad_number(s, what, "is negative");

  errno = 0;
  char * end = nullptr;
  const unsigned long long v = strtoull(s, & end, 10);

  if (end == s)
    bad_number(s, what, "is not a number");
  if (*end != 0)
    bad_number(s, what, "has trailing characters");
  if (errno == ERANGE)
    bad_number(s, what, "is out of range");

  return static_cast<uint64_t>(v);
}
