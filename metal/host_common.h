/* metal/host_common.h -- the interface between the backend-agnostic host
   (host_common.cc, which owns main()) and a backend that runs one batch of
   (key, restart) climbs: backend_cpu.cc runs the kernel body per lane on
   the CPU, backend_metal.mm dispatches climb.metal. */

#ifndef ENIGMA_HOST_COMMON_H
#define ENIGMA_HOST_COMMON_H

#include "climb_body.h"
#include "probe_body.h"

#include <stddef.h>
#include <stdint.h>

/* One batch: nkeys keys, each with params->restarts start boards, laid out
   key-major (item = key * restarts + restart, matching the kernel). rows is
   nkeys * L * 26 bytes; boards are 26 bytes per item; comps holds two
   int64 per item, isum then coin, under the target model. The tables are
   the CPU's own uint8 arrays. */
struct mc_batch
{
  const mc_params * params;
  const uint8_t * rows;
  const uint8_t * ct;
  const uint8_t * mono8;
  const uint8_t * bi8;
  const uint8_t * tri8;
  const uint8_t * quad8;
  const uint8_t * all8;
  const uint8_t * boards_in;
  uint8_t * boards_out;
  int64_t * comps;
  size_t nkeys;
  size_t nitems;
};

/* A short description for the settings echo, valid after backend_init(). */
const char * backend_name();

/* Set the backend up once; fatal() if it cannot run (no Metal device, no
   library). argv0 locates the .metallib beside the executable. */
void backend_init(const char * argv0);

/* Run one batch to completion, filling boards_out and comps. */
void backend_run(const mc_batch & b);

/* --- the probe microkernel (probe_body.h, DESIGN.md 17.6 (ii)) ---------
   One arm, one dispatch: params->units units each running params->nprobes
   toggles from board0 over ONE key's rows, writing two int64 checksums
   per unit to out.  tbl is the target model's table.  Returns false if
   the backend has no such arm (the CPU backend has only MC_PROBE_LANE);
   otherwise secs is the device time from commit to completion and cap the
   pipeline's thread limit, 0 where there is none. */
struct mc_probe_batch
{
  const mc_probe_params * params;
  const uint8_t * rows;      /* L * 26 bytes, one key */
  const uint8_t * ct;
  const uint8_t * tbl;
  const uint8_t * board0;    /* params->nboards boards of 26 bytes */
  int64_t * out;             /* 2 * units */
  int arm;
};

bool backend_probe(const mc_probe_batch & b, double * secs, int * cap);

/* The driver (probe_host.cc): m holds the key to probe, rows built.
   Returns the process exit status. */
struct machine;
int probe_run(machine & m, int lanes_cap);

#endif
