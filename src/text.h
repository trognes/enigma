/* Reading and normalising the input text: stdin for the ciphertext, a file for
   the -p comparison plaintext. Only A-Z survives -- case is folded up, accented
   Latin letters fold to their base (fold_codepoint, shared with the n-gram
   table loader so input and statistics are folded identically), whitespace is
   dropped silently, and anything else is dropped and counted so the reader can
   warn about it.

   The three globals below are the SHARED INPUT: read-only for the whole run
   once readciphertext() has returned, and read by the scoring hot path
   (num_ciphertext per character, textlength as the loop bound). */

#ifndef ENIGMA_TEXT_H
#define ENIGMA_TEXT_H

#include "common.h"

extern char ciphertext[maxlen+1];
extern int textlength;
extern unsigned char num_ciphertext[maxlen];

void readciphertext();
void readplaintext(char * filename, const char * result);
void alltoupper(char * text);
void removespaces(char * p);

#endif
