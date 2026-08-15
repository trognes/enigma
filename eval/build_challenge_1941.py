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
  "2 garble dashes; form group-count (104) disagrees with the transcription (112) -- form miscount, cf. Nr 136/153; the 01 Aug 2026 list still shows 104/99, so the discrepancy is unresolved upstream"),
 ("81", "28 Aug 1941", "ALQFI", 92,
  "ALQFIGEELUIBOXEINKBYDDHXIFALWDLTINTZIPGMLFMYZKAGJDWGOCBPEZTERTJIAGVINPRJIXTENBUDFXQYIDZMAPLG", ""),
 ("38", "09 Sep 1941", "GEHRG", 79,
  "GEHRGLGKQIOKNGSMRUXRZCGVNGYIRWVIISZXWRUUNDFWYUPWCGWRGFNETXXXGXINCIVXSYRGIGUWLOE",
  "position 31 corrected N -> V per the German Army Messages page (updated 01 Aug 2026); the old letter is very likely why this failed to break for twenty years"),
 ("8-Oct", "02 Oct 1941", "ALGXZ", 72,
  "ALGXZBOKTUGXSINFSOUZDTEXBPDTWENWBJMRMMLNUGIKXTBVZPMAPFRTNSOMUGPVXXDYWTJG", ""),
 ("6-C",  "29 Sep 1941", "QTXMA", 160,
  "QTXMAJVMOYCZAYMRVLCBSOQXYBATSXJBQLAEJKYTYXJOEMYBLOEMYOKSRMTAVLBCXJAMOESRXYTVAOEYAVYXKCJVCMEISHTBAYVXXAJWCZQCYPXMEHABLKYJYASOEIJYXOQXYTLBASYEESTAQXJVNWCBJZBYQYTM",
  "Batch C, and the SECOND-LONGEST unbroken message in the collection. From "
  "the Ultimate Enigma Challenge page (27 Jul 2026); it was absent here only "
  "because it had never been transcribed. Same batch caveat as BYQMZ: the "
  "authors are not sure Batch C is Enigma at all. Indicator LDP WRX, sent "
  "2240, to 2pn on 323 kHz"),
 ("7-C",  "29 Sep 1941", "SZAEJ", 56,
  "SZAEJTOMBYXCZEOJKSAMGEYPWXZWJMEVBZYZAEJVHSEMNWEYTMEOMTCG",
  "Batch C, same caveat. From the Ultimate Enigma Challenge page. Short (51 "
  "cipher letters), around twice the unicity distance, so breakable only if "
  "its day key comes from elsewhere. Indicator GAR PLD, sent 2314, to o37 on "
  "716 kHz"),
 ("8-C",  "30 Sep 1941", "BYQMZ", 172,
  "BYQMZNYZKYDOEMGPSDUHMLHJATWMYCHIF-YMAESTAVLCGCNLGMZIQUSQNRAIKYJDETUEXOJQPGXQSCEXENOSFASJVTGBHXTVGQTWKEWPPRIVYJEHEWNGPFUEAZTUWZUQBLNBYETZVSUAJSEASZXYFTUMOSHURQESSTQMPAOPBFTY",
  "Batch C -- the authors are NOT sure this is Enigma at all. Its J-rate is the one point in its favour: 6 J in 167 letters against the 6.4 a 26-letter cipher predicts. 1 dash. Longest unbroken message in the collection"),
 ("11-C", "30 Sep 1941", "FKQLZ", 112,
  "FKQLZDNXLIAGVIQBUWMHYCAMDFBAEQVGXMRCEPGARIHRKRTDLNYVCSWUFHIXLXPUCESNOLNAHZDKPNVBFSOKBPCTGDKOFMTWGSYOUTQRMPWWZORK",
  "Batch C -- may not be Enigma. The letter J does NOT occur in its 107 letters (4.1 expected), which is exactly what a 25-letter manual alphabet predicts; attack it only after BYQMZ and RXPSB"),
 ("12-C", "30 Sep 1941", "XFEDT", 102,
  "XFEDTZYOQHTSAFRLQCHZURCWOILRXGMCBFZKAPYDUMHVCATDPSEAKYSZEGFKGINXWRNQVOIDFANGLRXNUHGTVFCNEXBPWYMZFBXOAU",
  "Batch C -- may not be Enigma. Same as Nr 11-C: zero J in 97 letters (3.7 expected); pooled with FKQLZ that is 0 of an expected 7.8, p = 3e-4"),
 # --- July "Batch A": none of these is marked TS (Truppenschluessel) on the
# --- 01 Aug 2026 list, so the old blanket hand-cipher warning does not apply ---
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
 ("242", "20 Jul 1941", "JBIYH", 60,
  "JBIYHNVYMIVLOGGKTDKKOYXWRDLBHRRZYPILVVXOGBEFXAXCWBNGILRWARXO", ""),
 ("285", "31 Jul 1941", "FMNGI", 63,
  "FMNGIFGROVFDIVQMNMNILIFZBQVNQWLGBLJVRLEBXIQEXCSAQPEKFHEKFBIKMCF", ""),
]

# Status checked against the BGAC 1941 Message List (cryptocellar.org/bgac/
# 1941-msg-list.html), "Status on: 01 August 2026". FIVE of the ciphertexts
# above have been broken since this file was first assembled -- four of them in
# July 2026 -- so a run against one of them is wasted compute. Nr 138 is listed
# too: it is not itself broken, but the list records it as the same message as
# Nr 140, which is, so its plaintext is known. The keys are not
# reproduced here: the list records only that each was broken and by whom, and
# the recovered keys live on the collection's key pages. Fetch a key and the
# message can move to the validation set, where it is worth more than it is
# here (an authentic instance of known length with ground truth).
#
# Nr 214 (FTNBK) HAS made that move and is no longer in the list below: its key
# is B / III I IV / ring AHV / start FQR / AH CN DF EI KY MP OZ RU SW VX, and it
# now lives in enigma-army-messages-1941.txt with its verified plaintext. Its
# ciphertext HERE was wrong in 13 of 101 letters and did not decrypt under that
# key -- see MODERN_BREAKING_NOTES.md 5d, which is the more important half of
# that finding. The erroneous transcription is kept there verbatim, and is worth
# keeping: with those 13 letters wrong the true plaintext is no longer the
# highest-scoring one under ANY model (trigram through fused), so it is an
# authentic real-traffic scoring-failure instance -- unrecoverable in principle
# rather than merely hard.
SOLVED = {
  "81":  "BROKEN 14.07.2026. The key IS now in this repo -- but for the Bundesarchiv copy Nr 55 NF in enigma-army-messages-1941.txt, not for this transcription, which is too corrupt to decrypt under it",
  "38":  "BROKEN on 12.07.2026 elsewhere with no key published; the KEY IS NOW IN THIS REPO -- B 342 ring ALZ start UXT, plugs AZ DV ET FS GQ JP LX MY NR OW, recovered here from the 09.09.1941 day key that ALVPM and ALRHG gave up, and held with the plaintext as Nr 38 in enigma-army-messages-1941.txt",
  "8-Oct": "BROKEN on 31.07.2026; the KEY IS NOW IN THIS REPO -- B 452 ring DVM start WAS, plugs AP BU CX DH ER FQ IW KO LZ MS, published on Frode Weierud's June-October 1941 key page (01 Aug 2026) and held with the decrypt as Nr 8-Oct in enigma-army-messages-1941.txt. The transcription is heavily garbled: the key is right (ZWISQE[N] ... HARTJ[E]NS[T]EIN) but the middle does not read",
  "140": "BROKEN by Michael Craig on 17.07.2007 (key not recorded here)",
  "138": "same message as Nr 140, which is broken -- see Nr 140",
}

# Unbroken messages on that list whose ciphertext this repo does NOT hold. Both
# are 29 Sep 1941 and both are Batch C, so the batch-level caveat above applies
# to them as much as to BYQMZ: the authors are unsure Batch C is Enigma at all.
# Neither carries footnote *3 individually. QTXMA is the second-longest unbroken
# message in the collection and is absent from the challenge set only because it
# was never transcribed here.
MISSING = [
  # (now empty -- QTXMA and SZAEJ were transcribed on the "Ultimate Enigma
  # Challenge" page, updated 27 Jul 2026, and are carried as records below.)
]

HEADER = """\
# ============================================================================
# enigma-challenge-1941.txt  --  authentic 1941 German Army ciphertexts, UNBROKEN
# ----------------------------------------------------------------------------
# Source: Geoff Sullivan & Frode Weierud, "Breaking German Army Ciphers"
#         (Cryptologia 29(3):193-232, 2005); cryptocellar.org/bgac. HG Nord,
#         Operation Barbarossa, Jun-Oct 1941. CC BY-NC-SA.
#
# Presented as a standing challenge (a companion to the solved sets in
# enigma-messages.txt (13) and enigma-army-messages-1941.txt (61)). There is no
# plaintext or key here to verify against; build_challenge_1941.py only checks that
# each transcription matches the letter count written on the message form.
#
# STATUS: checked against the BGAC 1941 Message List, "Status on: 01 August 2026".
# FOUR of the ciphertexts below are NO LONGER UNBROKEN, and a fifth (Nr 138) is a
# second transcription of one of them -- all five are marked on their KEY: line.
# Do not spend compute on them. Their keys are not reproduced here. A sixth,
# Nr 214 (FTNBK), has been solved and MOVED to enigma-army-messages-1941.txt.
#
# CAVEATS (read before attacking):
#   * The 1st 5-letter group is the KENNGRUPPE (discriminant), NOT ciphertext --
#     remove it before deciphering. LEN below is the form count, which INCLUDES it.
#   * A dash '-' is an unrecorded-but-real letter (illegible on the form): a real
#     rotor position -- keep it as a placeholder, never strip it, or stepping desyncs.
#     NOTE the tool keeps only A-Z, so a dash is SILENTLY DROPPED and the stepping
#     desyncs from that point on: substitute a letter, do not pass the dash through.
#   * The manual cipher in this collection is the TRUPPENSCHLUESSEL, and the
#     message list marks it per message (cipher length shown as "TS"). None of the
#     messages below is marked TS. The older blanket warning here -- that "July
#     Batch A" is largely hand cipher "and the Enigma ones are short" -- is
#     superseded by that per-message mark, and was actively misleading: Nr 214
#     (FTNBK, 101 letters) is the longest message in that batch and it broke as
#     Enigma.
#   * "Batch C" (BYQMZ, FKQLZ, XFEDT) MAY NOT BE ENIGMA. The authors say so
#     directly on the German Army Messages page: "We are not quite sure if these
#     messages are Enigma messages or if they are messages enciphered with another
#     machine or system." An earlier revision of this file claimed that caveat was
#     misattributed, on the grounds that the message list's footnote *3 sits on
#     other Batch C messages; that inference was wrong and is withdrawn -- the
#     authors' own statement is the authority, not the placement of a footnote.
#     A test of *3's specific hypothesis (a 25-letter alphabet without J) SPLITS
#     the three, and is offered as evidence, not as a refutation of the caveat:
#     BYQMZ carries 6 J in 167 letters against the 6.4 a 26-letter cipher
#     predicts, while FKQLZ and XFEDT carry ZERO in 107 and 97, pooling to 0 of an
#     expected 7.8 (p = 3e-4). See MODERN_BREAKING_NOTES.md 5b.
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
            f.write("KEY:         %s\n" % SOLVED.get(no, "UNKNOWN -- unbroken"))
            f.write("CIPHERTEXT:  %s\n" % wrap(ct))
            f.write("NOTES:       %s\n\n" % (caveat if caveat else "(none)"))
        f.write("# ---------------------------------------------------------"
                "-------------------\n")
        f.write("# UNBROKEN ENIGMA MESSAGES NOT TRANSCRIBED HERE\n")
        f.write("# The 01 Aug 2026 message list carries two unbroken Enigma "
                "messages whose\n# ciphertext this repo does not hold. Neither "
                "carries footnote *3. QTXMA is the\n# SECOND-LONGEST unbroken "
                "Enigma message in the collection, so it is the most\n"
                "# valuable missing item here -- transcribe it from the message "
                "form (the list\n# links a 'Spruch' scan) to add it.\n")
        for no, date, kenn, slen, clen in MISSING:
            f.write("#   Nr %-4s %s  %s  %d / %d (form / cipher)\n"
                    % (no, date, kenn, slen, clen))
    live = len(C) - len(SOLVED)
    print("wrote %s  (%d ciphertexts: %d still a challenge, %d resolved)"
          % (OUT, len(C), live, len(SOLVED)))
    if mism:
        print("  transcription != form count (known form miscounts):",
              ", ".join("%s(%d!=%d)" % m for m in mism))
    else:
        print("  all transcriptions match the form counts")


if __name__ == "__main__":
    main()
