#include "common.h"

#include <stdio.h>
#include <stdlib.h>

[[noreturn]] void fatal(const char * message)
{
  fprintf(stderr, "\nFatal error: %s\n", message);
  exit(1);
}
