#!/usr/bin/env python3
# Build a CHALLENGE file of authentic 1941 German Army ciphertexts that remain UNBROKEN
# (no rotor key recovered). These have no plaintext to verify against; the only automatic
# check is that the transcribed length matches the length written on the message form
# (which INCLUDES the leading discriminant group). Source: cryptocellar.org/bgac
# (Sullivan & Weierud, "Breaking German Army Ciphers"). No keys exist for these.
import os, textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "enigma-challenge-1941.txt")

# (no, date, designator, stated_len_incl_designator, ciphertext-incl-designator, caveat)
C = [
 # --- "messages we failed to break" page: unbroken, no key ---
 ("3",  "22 Jun 1941", "EHSTQ", 57,
  "EHSTQJJAPYGERSYFZJSIABYQEKGZMGRNLZCLRDEEFTCUNFGVQYNKIYPIP", ""),
 ("53", "28 Jun 1941", "RXPSB", 104,
  "RXPSBRDHLUZNUQXTFYJJGX-WEOUBHLKSCGYMUTUPIGYGQGANNMPMQGASEQGTMLGHTUAFAMSNRDDXHXRQOQAZLGWTUHJGPDDFJLUCMXXYMUNU-TED",
  "2 garble dashes; form group-count (104) disagrees with the transcription (112) -- form miscount, cf. Nr 136/153"),
 ("81", "28 Aug 1941", "ALQFI", 92,
  "ALQFIGEELUIBOXEINKBYDDHXIFALWDLTINTZIPGMLFMYZKAGJDWGOCBPEZTERTJIAGVINPRJIXTENBUDFXQYIDZMAPLG", ""),
 ("38", "09 Sep 1941", "GEHRG", 79,
  "GEHRGLGKQIOKNGSMRUXRZCGVNGYIRWNIISZXWRUUNDFWYUPWCGWRGFNETXXXGXINCIVXSYRGIGUWLOE", ""),
 ("8-Oct", "02 Oct 1941", "ALGXZ", 72,
  "ALGXZBOKTUGXSINFSOUZDTEXBPDTWENWBJMRMMLNUGIKXTBVZPMAPFRTNSOMUGPVXXDYWTJG", ""),
 ("8-C",  "30 Sep 1941", "BYQMZ", 172,
  "BYQMZNYZKYDOEMGPSDUHMLHJATWMYCHIF-YMAESTAVLCGCNLGMZIQUSQNRAIKYJDETUEXOJQPGXQSCEXENOSFASJVTGBHXTVGQTWKEWPPRIVYJEHEWNGPFUEAZTUWZUQBLNBYETZVSUAJSEASZXYFTUMOSHURQESSTQMPAOPBFTY",
  "Batch C: authors uncertain whether Enigma or another machine/system; 1 dash"),
 ("11-C", "30 Sep 1941", "FKQLZ", 112,
  "FKQLZDNXLIAGVIQBUWMHYCAMDFBAEQVGXMRCEPGARIHRKRTDLNYVCSWUFHIXLXPUCESNOLNAHZDKPNVBFSOKBPCTGDKOFMTWGSYOUTQRMPWWZORK",
  "Batch C: authors uncertain whether Enigma or another machine/system"),
 ("12-C", "30 Sep 1941", "XFEDT", 102,
  "XFEDTZYOQHTSAFRLQCHZURCWOILRXGMCBFZKAPYDUMHVCATDPSEAKYSZEGFKGINXWRNQVOIDFANGLRXNUHGTVFCNEXBPWYMZFBXOAU",
  "Batch C: authors uncertain whether Enigma or another machine/system"),
 # --- July Batch A: unbroken (source notes many in this batch are hand cipher) ---
 ("87",  "03 Jul 1941", "KLJBO", 55,
  "KLJBOYNGZOWCIRESGVEVKFGCNXDTLIKINLBOYL-NTNYBD-NWK-A-UVV", "several garble dashes"),
 ("100", "05 Jul 1941", "LXACA", 25,
  "LXACAZIXAGNQRKOHBPNKXRLFU", "does NOT break on the 5 Jul key; header time suggests it belongs to 4 Jul"),
 ("138", "09 Jul 1941", "WEUWY", 53,
  "WEUWYWCZIEDSYTCDXOI-CDSXOXASIMEIORSRKRISSPCCOUIMDZYDM", "does NOT break on the 9 Jul key; 1 dash"),
 ("140", "09 Jul 1941", "WEUWY", 53,
  "WEUWYTCTICBSEYTHDHXOXUSIMIEORHRKVEIXFICQUIMBZIDZMFEQM", "does NOT break on the 9 Jul key"),
 ("172", "10 Jul 1941", "MVUEH", 87,
  "MVUEHIDEVSARMCCNQTATYEVFCDBZGGSMXWLPSYWZYTCBSWURRTBZCVGODVJUSLSOOMJQJZSXSEBZPEYMDNXJFTC",
  "does NOT break on the 10 Jul key; suspected different network/key"),
 ("187", "11 Jul 1941", "AWTZK", 54,
  "AWTZKTBXVAKXKLZIPPCZIPUCCRXHRKQUTDEGMGIKGCWEKLQNUMCWSS", ""),
 ("189", "11 Jul 1941", "ZNLZT", 74,
  "ZNLZTKCBDBJDLAVPWLLUTSSHBWEYOWSQNB-NORNKDTZJHPQFYAXCQQYFLSSKDZCGLSWYMBQBMF", "1 dash"),
 ("214", "16 Jul 1941", "FTNBK", 106,
  "FTNBKXNQAEQNZLWFMQGTXOQZVXJKBOJKPCLJQZOVFLSVJBSRIYMRYWNUJVWYKXAKKFMSQFBBARNKNHBRHQSLIUVNEHMJKAZRXLJLWISNZZ", ""),
 ("242", "20 Jul 1941", "JBIYH", 60,
  "JBIYHNVYMIVLOGGKTDKKOYXWRDLBHRRZYPILVVXOGBEFXAXCWBNGILRWARXO", ""),
 ("285", "31 Jul 1941", "FMNGI", 63,
  "FMNGIFGROVFDIVQMNMNILIFZBQVNQWLGBLJVRLEBXIQEXCSAQPEKFHEKFBIKMCF", ""),
]

HEADER = """\
# ============================================================================
# enigma-challenge-1941.txt  --  authentic 1941 German Army ciphertexts, UNBROKEN
# ----------------------------------------------------------------------------
# Source: Geoff Sullivan & Frode Weierud, "Breaking German Army Ciphers"
#         (Cryptologia 29(3):193-232, 2005); cryptocellar.org/bgac. HG Nord,
#         Operation Barbarossa, Jun-Oct 1941. CC BY-NC-SA.
#
# These ciphertexts have NO recovered rotor key -- they remain unbroken. Presented
# as a standing challenge (this is a companion to the solved sets in
# enigma-messages.txt (13) and enigma-army-messages-1941.txt (56)). There is no
# plaintext or key here to verify against; build_challenge_1941.py only checks that
# each transcription matches the letter count written on the message form.
#
# CAVEATS (read before attacking):
#   * The 1st 5-letter group is the KENNGRUPPE (discriminant), NOT ciphertext --
#     remove it before deciphering. LEN below is the form count, which INCLUDES it.
#   * A dash '-' is an unrecorded-but-real letter (illegible on the form): a real
#     rotor position -- keep it as a placeholder, never strip it, or stepping desyncs.
#   * "July Batch A" (Nr 87..285): the source notes MANY messages in this batch are
#     hand cipher (a Doppelkasten variant), NOT Enigma, and the Enigma ones are short.
#   * "Batch C" (BYQMZ, FKQLZ, XFEDT): the authors are unsure these are Enigma at all.
#   * A few carry a known day-key that they do NOT break on (noted per message) --
#     evidence of a different key/network or a non-Enigma system.
#   * Most are short (below the ~23-letter unicity distance for HG Nord traffic),
#     so several may be statistically unbreakable even as Enigma.
# ============================================================================
"""


def wrap(s, width=60):
    return "\n             ".join(textwrap.wrap(s, width))


def main():
    mism = []
    with open(OUT, "w") as f:
        f.write(HEADER + "\n")
        for no, date, kenn, slen, ct, caveat in C:
            got = len(ct)
            flag = "" if got == slen else "  <-- transcription %d != form %d" % (got, slen)
            if got != slen:
                mism.append((no, got, slen))
            f.write("### Message No. %s  --  %s  (%s)\n" % (no, date, kenn))
            f.write("KENNGRUPPE:  %s   (discriminant; strip before deciphering)\n" % kenn)
            f.write("LEN:         %d  (form count, incl. Kenngruppe)%s\n" % (slen, flag))
            f.write("KEY:         UNKNOWN -- unbroken\n")
            f.write("CIPHERTEXT:  %s\n" % wrap(ct))
            f.write("NOTES:       %s\n\n" % (caveat if caveat else "(none)"))
    print("wrote %s  (%d challenge ciphertexts)" % (OUT, len(C)))
    if mism:
        print("  transcription != form count (known form miscounts):",
              ", ".join("%s(%d!=%d)" % m for m in mism))
    else:
        print("  all transcriptions match the form counts")


if __name__ == "__main__":
    main()
