/* Turning a command line into resolved options.

   parse_args() does everything between the shell and the search: defaults,
   the getopt loop, validation, and loading the files the options name. It sets
   the opt_* globals declared in options.h and calls fatal() on anything it
   cannot accept; there is no return value because there is nothing to return.

   IT LOADS BEFORE IT READS STDIN, deliberately. The n-gram tables, the
   --crib-rerank word list and the --crib-list library are all opened here
   rather than after the ciphertext, so a missing or empty file is reported
   immediately instead of after the user has piped a message in and waited.

   Several defaults cannot be set until parsing finishes: the reflector, wheel,
   ring and start defaults depend on whether -4 was given, since M4 carries a
   fourth Greek position. Those are left null through the loop and resolved
   afterwards. */

#ifndef ENIGMA_ARGS_H
#define ENIGMA_ARGS_H

void parse_args(int argc, char * * argv);

#endif
