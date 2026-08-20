/* The --score/-S staged climb schedule: a string of model tokens, each with an
   optional cap on the plug pairs that stage may set, parsed once into
   opt_stages[].

   The staging is the point, not the parsing. A lower-order model has a far
   smoother surface while few plugs are set, so an early stage steers the first
   plugs into a basin a single-model climb navigates poorly -- staging reshapes
   the landscape, where random restarts resample it, and the two are
   complementary. The LAST model token is the target: it sets the ranking model,
   so the target lives in the string rather than beside it. */

#ifndef ENIGMA_SCHEDULE_H
#define ENIGMA_SCHEDULE_H

void parse_schedule();

/* True when the schedule carries climb-only detail -- more than one stage, or
   any cap -- which is meaningless without -c and earns a warning there. */
bool schedule_is_climb_only();

#endif
