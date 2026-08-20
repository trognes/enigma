/* The spawn/join boilerplate every search phase shares, and nothing else.

   Header-only because it is a template on the callable, the same reason
   result.h has no .cc: there is nothing to compile once. It was a static in
   search.cc until the --ring-stride refinement moved out to refine.cc and a
   second module needed it -- keeping it in a header of its own means only the
   two units that fan out threads pay for <thread>, rather than everything that
   includes search.h. */

#ifndef ENIGMA_PARALLEL_H
#define ENIGMA_PARALLEL_H

#include <thread>
#include <vector>

/* Run per_thread(t) for t in [0, nthreads): inline when single-threaded,
   otherwise on a thread pool joined before returning. Every search phase uses
   this, so the spawn/join boilerplate lives in one place. Objects the
   per_thread lambda captures by reference outlive the join, so no std::ref
   wrapping is needed. */
template <typename F>
void run_parallel(int nthreads, F per_thread)
{
  if (nthreads <= 1)
    {
      per_thread(0);
      return;
    }
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nthreads));
  for (int t = 0; t < nthreads; t++)
    pool.emplace_back(per_thread, t);
  for (std::thread & th : pool)
    th.join();
}

#endif
