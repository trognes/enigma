/* The user-facing text: --version, --help, and the resolved-configuration echo
   every run writes to stderr before it starts work.

   show_settings() is the one that earns its keep. Every run prints what it
   actually resolved -- scoring model, language, data directory, machine
   settings, plugboard, ciphertext length -- so a surprising result is never a
   mystery about which options were in force. */

#ifndef ENIGMA_CLI_H
#define ENIGMA_CLI_H

#include <stdio.h>

/* version()/help() take the output stream: explicit -h/-v write to stdout and
   exit 0, while usage errors (no arguments, bad option) write to stderr and
   exit 1. */
void version(FILE * out);
void help(FILE * out);

void show_settings();

#endif
