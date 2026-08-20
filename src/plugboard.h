/* Recovering the plugboard: the greedy climb, the gain cascade finishers,
   simulated annealing, and the fixed-plug bookkeeping they all respect.

   WHY THIS MODULE IS DRAWN WHERE IT IS. plug_fixed (the -s / --no-plug mark)
   and plug_fixed_ex (the per-worker copy carrying additionally forced pairs)
   are read inside the climb's move loop, and their STORAGE FORM is measured to
   matter by ~18%: clang wants plug_fixed_ex a static thread_local, g++ wants it
   a machine member, and routing either through a struct member, an opaque
   pointer parameter, or -- now -- an extern declaration in a shared header
   costs one compiler or the other. So everything that reads them lives here,
   including simulated annealing, whose apply_toggle() would otherwise need
   plug_fixed exported.

   That is also why the climb chain is a template on EX rather than taking the
   pin set as an argument: the common EX=false instantiation folds to the plain
   global. Both instantiations are named explicitly in plugboard.cc, so callers
   elsewhere can use them without the definitions leaving the file. */

#ifndef ENIGMA_PLUGBOARD_H
#define ENIGMA_PLUGBOARD_H

#include "common.h"
#include "machine.h"

#include <stdint.h>
#include <stddef.h>

/* Board setup. init_plug_fixed() parses -s and --no-plug once, before the
   threaded search starts; the other two act on one machine's board. */
void init_plug_fixed(const char * steckerbrett_string,
                     const char * no_plug_string);
void init_steckerbrett(machine & m, const char * steckerbrett_string);
void apply_soft_plug(machine & m);

/* Inject k random plug pairs (the --random kick) among the letters that are
   both unplugged and unfixed. */
void perturb_steckerbrett(machine & m, uint64_t * rng, int k);

/* One climb to convergence, capped at max_pairs plugs. EX selects the pin set:
   false = the global -s/--no-plug mark, true = this worker's copy plus whatever
   --exhaust or a crib deduction pinned into it. */
template<bool EX> double hillclimb(machine & m, int max_pairs);

/* The staged climb: every --score stage in order, each capped, returning the
   target model's score. */
template<bool EX> double run_stages(machine & m);

/* Only the two instantiations other modules actually call are exported:
   search.cc climbs with hillclimb<false>, and --exhaust / the crib hybrid /
   the self-crib seeder all enter through run_stages<true>. The other two
   (hillclimb<true>, run_stages<false>) are called only from inside
   plugboard.cc -- by run_stages<true> and optimize_once respectively -- so
   they are instantiated implicitly there and need no declaration here. */
extern template double hillclimb<false>(machine & m, int max_pairs);
extern template double run_stages<true>(machine & m);

/* Recover the board for the current key from the current seed: simulated
   annealing under -A, the staged greedy climb otherwise. */
double optimize_once(machine & m, uint64_t * rng);

/* A read-only view of the -s / --no-plug knowledge, for the crib deduction,
   which seeds its closure from whatever is already known so a contradicting
   hypothesis dies before it is climbed. have_known_plugs() is false in the
   ordinary case and gates the other two -- that gate is load-bearing, since an
   unconditional 26-letter prologue in crib_try cost +50% on a crib sweep. */
bool have_known_plugs();
const bool * known_plug_mark();
const unsigned char * known_plug_partner();

/* Independent RNG seed for one restart, mixed from opt_seed, the flat key
   index and the restart index. Each restart draws from its OWN stream rather
   than a single stream advanced sequentially, which is what lets the restarts
   run in any order on any thread and still give the same answer -- the
   precondition for parallelising them. */
uint64_t restart_seed(size_t key_index, int restart);

/* Install extra pins for an EX=true climb. --exhaust, the crib hybrid and the
   self-crib seeder call reset() then pin() per deduced letter; they must go
   through these rather than name the storage, which is a static thread_local
   under clang and a machine member under g++ (see the note above). */
void plug_fixed_ex_reset(machine & m);
void plug_fixed_ex_pin(machine & m, int letter);

#endif
