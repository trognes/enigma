/* metal/host_common.h -- the interface between the backend-agnostic host
   (host_common.cc, which owns main()) and a backend that runs one batch of
   (key, restart) climbs: backend_cpu.cc runs the kernel body per lane on
   the CPU, backend_metal.mm dispatches climb.metal. */

#ifndef ENIGMA_HOST_COMMON_H
#define ENIGMA_HOST_COMMON_H

#include "climb_body.h"

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

#endif
