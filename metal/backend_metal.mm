/* metal/backend_metal.mm -- the Metal backend: uploads a batch, dispatches
   enigma_climb (climb.metal) with one threadgroup per (key, slice of its
   restarts), downloads the boards and components. Objective-C++; macOS
   only. The .metallib is loaded from beside the executable, or from
   $ENIGMA_METALLIB.

   No pipelining yet: each batch is uploaded, run and downloaded before the
   host builds the next (DESIGN.md 7 leaves that to milestone 5). The
   tables go up once. */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "host_common.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>

static id<MTLDevice> g_dev = nil;
static id<MTLCommandQueue> g_queue = nil;
static id<MTLComputePipelineState> g_pso = nil;
static id<MTLBuffer> g_tables[5] = { nil, nil, nil, nil, nil };
static std::string g_name;
static std::string g_libdir;   /* where the metallibs sit, for the probe */

const char * backend_name()
{
  return g_name.c_str();
}

void backend_init(const char * argv0)
{
  @autoreleasepool
    {
      g_dev = MTLCreateSystemDefaultDevice();
      if (g_dev == nil)
        fatal("no Metal device");

      NSString * path = nil;
      const char * env = getenv("ENIGMA_METALLIB");
      if ((env != nullptr) && (*env != 0))
        path = [NSString stringWithUTF8String:env];
      else
        {
          NSString * exe = [NSString stringWithUTF8String:argv0];
          NSString * dir = [exe stringByDeletingLastPathComponent];
          if ([dir length] == 0)
            dir = @".";
          path = [dir stringByAppendingPathComponent:@"climb.metallib"];
          g_libdir = [dir UTF8String];
        }

      NSError * err = nil;
      id<MTLLibrary> lib =
        [g_dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:& err];
      if (lib == nil)
        {
          fprintf(stderr, "cannot load %s: %s\n", [path UTF8String],
                  [[err localizedDescription] UTF8String]);
          fatal("no Metal library (build it with `make -C metal metal`, or "
                "point $ENIGMA_METALLIB at it)");
        }
      id<MTLFunction> fn = [lib newFunctionWithName:@"enigma_climb"];
      if (fn == nil)
        fatal("enigma_climb is not in the Metal library");
      g_pso = [g_dev newComputePipelineStateWithFunction:fn error:& err];
      if (g_pso == nil)
        {
          fprintf(stderr, "%s\n", [[err localizedDescription] UTF8String]);
          fatal("cannot build the Metal pipeline");
        }
      g_queue = [g_dev newCommandQueue];

      g_name = std::string("Metal on ") + [[g_dev name] UTF8String]
        + " (" + std::to_string(g_pso.maxTotalThreadsPerThreadgroup)
        + " threads per threadgroup, simd width "
        + std::to_string(g_pso.threadExecutionWidth) + ")";
    }
}

static id<MTLBuffer> upload(const void * data, size_t bytes)
{
  if (bytes == 0)
    bytes = 1;
  id<MTLBuffer> buf = [g_dev newBufferWithBytes:data
                                         length:bytes
                                        options:MTLResourceStorageModeShared];
  if (buf == nil)
    fatal("Metal buffer allocation failed");
  return buf;
}

void backend_run(const mc_batch & b)
{
  @autoreleasepool
    {
      mc_params p = *b.params;
      const NSUInteger maxthreads = g_pso.maxTotalThreadsPerThreadgroup;
      if (static_cast<NSUInteger>(p.lanes_per_tg) > maxthreads)
        p.lanes_per_tg = static_cast<int64_t>(maxthreads);
      const size_t restarts = static_cast<size_t>(p.restarts);
      const size_t lanes = static_cast<size_t>(p.lanes_per_tg);
      const size_t tgs_per_key = (restarts + lanes - 1) / lanes;
      const size_t L = static_cast<size_t>(p.L);

      if (g_tables[0] == nil)
        {
          g_tables[0] = upload(b.mono8, MC_ASIZE);
          g_tables[1] = upload(b.bi8, MC_ASIZE * MC_ASIZE);
          g_tables[2] = upload(b.tri8, MC_ASIZE * MC_ASIZE * MC_ASIZE);
          g_tables[3] = upload(b.quad8, MC_ASIZE * MC_ASIZE * MC_ASIZE
                                        * MC_ASIZE);
          g_tables[4] = upload(b.all8, MC_ASIZE * MC_ASIZE * MC_ASIZE
                                       * MC_ASIZE);
        }

      id<MTLBuffer> params = upload(& p, sizeof p);
      id<MTLBuffer> rows = upload(b.rows, b.nkeys * L * MC_ASIZE);
      id<MTLBuffer> ct = upload(b.ct, L);
      id<MTLBuffer> boards_in = upload(b.boards_in, b.nitems * MC_ASIZE);
      id<MTLBuffer> boards_out =
        [g_dev newBufferWithLength:b.nitems * MC_ASIZE
                           options:MTLResourceStorageModeShared];
      id<MTLBuffer> comps =
        [g_dev newBufferWithLength:b.nitems * 2 * sizeof(int64_t)
                           options:MTLResourceStorageModeShared];
      if ((boards_out == nil) || (comps == nil))
        fatal("Metal buffer allocation failed");

      id<MTLCommandBuffer> cmd = [g_queue commandBuffer];
      id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
      [enc setComputePipelineState:g_pso];
      [enc setBuffer:params offset:0 atIndex:0];
      [enc setBuffer:rows offset:0 atIndex:1];
      [enc setBuffer:ct offset:0 atIndex:2];
      for (int i = 0; i < 5; i++)
        [enc setBuffer:g_tables[i] offset:0 atIndex:3 + i];
      [enc setBuffer:boards_in offset:0 atIndex:8];
      [enc setBuffer:boards_out offset:0 atIndex:9];
      [enc setBuffer:comps offset:0 atIndex:10];
      [enc dispatchThreadgroups:MTLSizeMake(b.nkeys * tgs_per_key, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(lanes, 1, 1)];
      [enc endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];
      if (cmd.error != nil)
        {
          fprintf(stderr, "%s\n",
                  [[cmd.error localizedDescription] UTF8String]);
          fatal("the Metal dispatch failed");
        }

      memcpy(b.boards_out, [boards_out contents], b.nitems * MC_ASIZE);
      memcpy(b.comps, [comps contents], b.nitems * 2 * sizeof(int64_t));
    }
}

/* --- the probe microkernel (probe.metal, DESIGN.md 17.6 (ii)) ---------- */

static id<MTLComputePipelineState> g_probe_pso[MC_PROBE_ARMS];
static const char * const g_probe_fn[MC_PROBE_ARMS] =
  { "enigma_probe_lane", "enigma_probe_g32s", "enigma_probe_g32",
    "enigma_probe_g16", "enigma_probe_g8" };
static const int g_probe_k[MC_PROBE_ARMS] = { 1, 32, 32, 16, 8 };

static void probe_init()
{
  if (g_probe_pso[0] != nil)
    return;
  NSString * path = nil;
  const char * env = getenv("ENIGMA_PROBELIB");
  if ((env != nullptr) && (*env != 0))
    path = [NSString stringWithUTF8String:env];
  else
    {
      NSString * dir = [NSString stringWithUTF8String:
                          g_libdir.empty() ? "." : g_libdir.c_str()];
      path = [dir stringByAppendingPathComponent:@"probe.metallib"];
    }
  NSError * err = nil;
  id<MTLLibrary> lib =
    [g_dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:& err];
  if (lib == nil)
    {
      fprintf(stderr, "cannot load %s: %s\n", [path UTF8String],
              [[err localizedDescription] UTF8String]);
      fatal("no probe library (build it with `make -C metal metal`, or "
            "point $ENIGMA_PROBELIB at it)");
    }
  for (int a = 0; a < MC_PROBE_ARMS; a++)
    {
      id<MTLFunction> fn =
        [lib newFunctionWithName:
               [NSString stringWithUTF8String:g_probe_fn[a]]];
      if (fn == nil)
        {
          fprintf(stderr, "%s is not in the probe library\n", g_probe_fn[a]);
          fatal("probe library incomplete");
        }
      g_probe_pso[a] = [g_dev newComputePipelineStateWithFunction:fn
                                                            error:& err];
      if (g_probe_pso[a] == nil)
        {
          fprintf(stderr, "%s: %s\n", g_probe_fn[a],
                  [[err localizedDescription] UTF8String]);
          fatal("cannot build a probe pipeline");
        }
    }
}

bool backend_probe(const mc_probe_batch & b, double * secs, int * cap)
{
  @autoreleasepool
    {
      probe_init();
      if ((b.arm < 0) || (b.arm >= MC_PROBE_ARMS))
        return false;
      id<MTLComputePipelineState> pso = g_probe_pso[b.arm];
      const NSUInteger maxthreads = pso.maxTotalThreadsPerThreadgroup;
      *cap = static_cast<int>(maxthreads);

      mc_probe_params p = *b.params;
      const int K = g_probe_k[b.arm];
      size_t lanes = static_cast<size_t>(p.lanes_per_tg);
      if (lanes > maxthreads)
        lanes = maxthreads;
      if (K > 1)
        {
          lanes -= lanes % 32;    /* whole simdgroups, for the shuffles */
          if (lanes < 32)
            lanes = 32;
        }
      p.lanes_per_tg = static_cast<int64_t>(lanes);
      const size_t units = static_cast<size_t>(p.units);
      const size_t per_tg = lanes / static_cast<size_t>(K);
      const size_t tgs = (units + per_tg - 1) / per_tg;
      const size_t L = static_cast<size_t>(p.L);

      id<MTLBuffer> params = upload(& p, sizeof p);
      id<MTLBuffer> rows = upload(b.rows, L * MC_ASIZE);
      id<MTLBuffer> ct = upload(b.ct, L);
      id<MTLBuffer> tbl = upload(b.tbl, MC_ASIZE * MC_ASIZE * MC_ASIZE
                                        * MC_ASIZE);
      id<MTLBuffer> board0 =
        upload(b.board0, static_cast<size_t>(p.nboards) * MC_ASIZE);
      id<MTLBuffer> out =
        [g_dev newBufferWithLength:units * 2 * sizeof(int64_t)
                           options:MTLResourceStorageModeShared];
      if (out == nil)
        fatal("Metal buffer allocation failed");

      id<MTLCommandBuffer> cmd = [g_queue commandBuffer];
      id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
      [enc setComputePipelineState:pso];
      [enc setBuffer:params offset:0 atIndex:0];
      [enc setBuffer:rows offset:0 atIndex:1];
      [enc setBuffer:ct offset:0 atIndex:2];
      [enc setBuffer:tbl offset:0 atIndex:3];
      [enc setBuffer:board0 offset:0 atIndex:4];
      [enc setBuffer:out offset:0 atIndex:5];
      [enc dispatchThreadgroups:MTLSizeMake(tgs, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(lanes, 1, 1)];
      [enc endEncoding];

      /* Device time is commit to completion: the uploads above are a few
         hundred KB and belong to neither arm. */
      const auto t0 = std::chrono::steady_clock::now();
      [cmd commit];
      [cmd waitUntilCompleted];
      *secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
      if (cmd.error != nil)
        {
          fprintf(stderr, "%s\n",
                  [[cmd.error localizedDescription] UTF8String]);
          fatal("the probe dispatch failed");
        }
      memcpy(b.out, [out contents], units * 2 * sizeof(int64_t));
      return true;
    }
}
