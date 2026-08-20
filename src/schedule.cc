#include "schedule.h"

#include "common.h"
#include "crib.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "progress.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/resource.h>

/* Parse the --score/-S schedule string into opt_stages[]/opt_nstages, and set
   opt_scoring to the target (last model stage). Tokens are <letter><optional int>:
   model letters i/m/b/t/q (a climb stage; the number caps its plug pairs, omitted =
   uncapped). On a syntax error it calls fatal(). With no --score the schedule is the
   single -i/-m/.../-q target, uncapped. The per-restart random kick (--random) and
   the partial exhaustion (--exhaust) are separate options, not schedule tokens. */
void parse_schedule()
{
  opt_nstages = 0;

  if (! opt_staged)
    {
      opt_stages[0].model = opt_scoring;
      opt_stages[0].cap = pairs_uncapped;
      opt_nstages = 1;
      return;
    }

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

      if (strchr("imbtqaf", letter))
        {
          if (opt_nstages >= max_stages)
            fatal("Illegal --score schedule: too many stages (max 16)");
          int cap = (n < 0) ? pairs_uncapped : n;
          if ((cap < 1) || (cap > pairs_uncapped))
            fatal("Illegal --score stage cap (1 to 13 plug pairs; omit for no cap)");
          opt_stages[opt_nstages].model = model_of(letter);
          opt_stages[opt_nstages].cap = cap;
          opt_nstages++;
        }
      else
        fatal("Illegal --score schedule (tokens are i/m/b/t/q/a/f + optional cap, "
              "e.g. --score m4f10; use --random for the kick, --exhaust for forcing)");
    }

  if (opt_nstages < 1)
    fatal("Illegal --score schedule: needs at least one model stage (i/m/b/t/q/a)");

  /* the last model stage is the target/ranking model */
  opt_scoring = opt_stages[opt_nstages - 1].model;
}
/* Does the parsed --score schedule carry climb-only detail -- i.e. more than one
   stage, or any stage capped below the board maximum? Such detail is meaningful only
   during a plugboard climb (-c); a bare rotor scan just ranks by the target model.
   Used to warn when a climb schedule is given without -c. */
bool schedule_is_climb_only()
{
  if (opt_nstages > 1)
    return true;
  for (int i = 0; i < opt_nstages; i++)
    if (opt_stages[i].cap < pairs_uncapped)
      return true;
  return false;
}