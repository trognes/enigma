/* The hard-coded Enigma wiring: reflector and rotor substitution alphabets and
   the turnover notches, exactly as wired in the physical machines. Read once by
   init(), which derives the numeric permutation tables the search uses; nothing
   here is touched again after that.

   Index conventions, relied on throughout: reflectors 0-2 = A/B/C, 3 = Norway,
   4-5 = M4 thin UKW-b/c; rotors 0-7 = I-VIII, 8-12 = Norway 1-5, 13-14 =
   Beta/Gamma.

   The array bounds are spelled out here so reflector_count and rotor_count stay
   compile-time constants (they are used as array bounds themselves). wiring.cc
   static_asserts them against the real definitions, so adding a rotor without
   updating this header is a build error rather than a silent truncation. */

#ifndef ENIGMA_WIRING_H
#define ENIGMA_WIRING_H

extern const char * reflector_string[6];
extern const char * rotor_string[15];
extern const char * notch_string[15];

static const int reflector_count = sizeof(reflector_string) / sizeof(char *);
static const int rotor_count = sizeof(rotor_string) / sizeof(char *);

/* Layout of the reflector[] / rotor[] wiring tables for Norway Enigma mode:
   reflector index 3 is UKW-N, rotor indices 8-12 are Norway wheels 1-5. */
static const int norway_reflector_index = 3;
static const int norway_rotor_base = 8;

#endif
