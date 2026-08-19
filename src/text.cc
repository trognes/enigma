#include "text.h"

#include "common.h"
#include "ngrams.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

char ciphertext[maxlen+1];
int textlength;
unsigned char num_ciphertext[maxlen];

/* Only readplaintext() ever looks at the -p comparison text, so it stays
   private to this module. */
static char altplaintext[maxlen+1];

/* Streaming UTF-8 text filter shared by the ciphertext and plaintext readers.
   Carries the decode state (cp/need) across read() calls so a multi-byte code
   point split over a buffer boundary still decodes. Each letter is folded to its
   A-Z base (fold_codepoint); whitespace is dropped silently; every other
   non-mappable code point is dropped and counted so the reader can warn. */
struct textfilter
{
  unsigned cp;                 /* UTF-8 code-point accumulator */
  int need;                    /* continuation bytes still expected */
  unsigned long accented;      /* non-A-Z letters folded to a base letter */
  unsigned long skipped;       /* non-mappable, non-whitespace code points dropped */
};

static void filter_bytes(textfilter * st, const unsigned char * buf, ssize_t len,
                         char * out, int * j, const char * toolong)
{
  for (ssize_t i = 0; i < len; i++)
    {
      unsigned char b = buf[i];
      int done = -1;
      if (st->need > 0)
        {
          if ((b & 0xC0) == 0x80)
            {
              st->cp = (st->cp << 6) | (b & 0x3Fu);
              if (--st->need == 0)
                done = static_cast<int>(st->cp);
            }
          else
            {
              st->need = 0;   /* malformed: drop partial, reprocess b as a lead */
              i--;
              continue;
            }
        }
      else if (b < 0x80)
        done = b;
      else if ((b & 0xE0) == 0xC0) { st->cp = b & 0x1Fu; st->need = 1; }
      else if ((b & 0xF0) == 0xE0) { st->cp = b & 0x0Fu; st->need = 2; }
      else if ((b & 0xF8) == 0xF0) { st->cp = b & 0x07u; st->need = 3; }
      /* else: invalid lead byte -- ignored */

      if (done < 0)
        continue;

      unsigned u = static_cast<unsigned>(done);
      int base = fold_codepoint(u);
      if (base >= 0)
        {
          if (! (((u >= 'A') && (u <= 'Z')) || ((u >= 'a') && (u <= 'z'))))
            st->accented++;
          if (*j >= maxlen)
            fatal(toolong);
          out[(*j)++] = num2char(base);
        }
      else if ((u == ' ') || (u == '\t') || (u == '\n') || (u == '\r')
               || (u == '\f') || (u == '\v'))
        { /* whitespace: silently skipped */ }
      else
        st->skipped++;   /* non-mappable, non-whitespace: skip and count */
    }
}

static void warn_filtered(const textfilter * st, const char * what)
{
  if (st->accented > 0)
    fprintf(stderr, "Note: %s contained %lu non-A-Z letter(s); folded accents to "
            "their A-Z base form.\n", what, st->accented);
  if (st->skipped > 0)
    fprintf(stderr, "Note: %s contained %lu non-mappable character(s) (skipped).\n",
            what, st->skipped);
}

void readciphertext()
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;
  textfilter st = {0, 0, 0, 0};
  char toolong[64];
  snprintf(toolong, sizeof(toolong),
           "Ciphertext too long (maximum is %d letters)", maxlen);

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0)
    filter_bytes(& st, buffer, len, ciphertext, & j, toolong);

  if (len < 0)
    fatal("Error reading ciphertext from standard input");

  ciphertext[j] = 0;
  textlength = j;
  warn_filtered(& st, "ciphertext input");
}

void readplaintext(char * filename, const char * result)
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;

  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    fatal("Unable to open plaintext file");

  textfilter st = {0, 0, 0, 0};
  char toolong[64];
  snprintf(toolong, sizeof(toolong),
           "Plaintext file too long (maximum is %d letters)", maxlen);

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(fd, buffer, sizeof(buffer))) > 0)
    filter_bytes(& st, buffer, len, altplaintext, & j, toolong);

  int read_error = (len < 0);
  close(fd);
  if (read_error)
    fatal("Error reading plaintext file");

  altplaintext[j] = 0;
  warn_filtered(& st, "plaintext file");

  if (textlength != j)
    fatal("Plaintext not same length as ciphertext");

  int identical = 0;
  for (int i=0; i<textlength; i++)
    if (result[i] == altplaintext[i])
      identical++;

  fprintf(stderr,
          "%d of %d letters (%.1f%%) identical to given plaintext\n",
          identical,
          textlength,
          100.0 * identical / textlength);
}

void alltoupper(char * text)
{
  int len = strlen(text);
  for(int i=0; i<len; i++)
    text[i] = toupper(text[i]);
}

void removespaces(char * p)
{
  char * q = p;
  while(char c = *p++)
    if (c != ' ')
      *q++ = c;
  *q=0;
}
