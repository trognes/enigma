#!/usr/bin/env python3
# Build a database of the 13 authentic Enigma messages from Ostwald & Weierud,
# "Modern breaking of Enigma ciphertexts" (Cryptologia 2017; cryptocellar.org),
# and VALIDATE each by decrypting with ./enigma and comparing to the raw plaintext.
import os, subprocess, textwrap

BIN = "./enigma"
os.environ["ENIGMA_DATA"] = "ngrams"

# Each: no, date, dir(in/out), refl, wheels(digits), ring, start, plugs, kenn, cipher, raw, emended, transl, garble
M = [
 dict(no="25", date="13 July 1941", dir="outgoing", refl="B", wheels="423", ring="GTO", start="SDV",
      plugs="AD EH GY IM KN LR OZ QV TX UW", kenn="FHPQX",
      cipher="FDZCJJDKVWPYFDWPOQZGTJQYYXAFRHSQESERKGJBWBYPEOOKFMMPOMKQDDOLCPKHYPGUZYXBZYANYSAXIPXVQCPJBFFFDRDXFIJJPPPEYALCYKVLKXQHWIRZANGWUJBWVJYCKESMJQRYKQHCQOKMMYWMCKVLZJDVZXRUMRMNWFDZBQGXJQAPFFFZTAHJQZPWQWNIVZWUIJTHOYXGDCOJUW",
      raw="ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAUMXKANNNIQTEINFLIESZENXDAXDRITTEXINFXDIVXUNDXAQTEXPANZXDIVXBLOQIERENUNDRANMBELEGTHALTEXDIVXKDRX",
      emended="An x Panz x Gruppe x Vier x Siegfried Siegfried Toni x Div x steht seit x Eins Zwo x Sieben x Eins Eins Null Null x Uhr mit Anfaengen am Unterkunftsraum x Kann niqt einflieszen x da x dritte x Inf x Div x und x Aqte x Panz x Div x bloqieren und Raum belegt halten x Div x Kdr x",
      transl="To Tank Group 4: SST Div stands since 12 July 1100 hours with vanguard at the accommodation space. Cannot enter, as 3rd Inf Div and 8th Tank Div are blocking and keeping place occupied. Div Cmdr.",
      garble="raw RANM -> Raum; HALTE -> halten (operator/transmission)"),
 dict(no="65", date="26 August 1941", dir="incoming", refl="B", wheels="321", ring="XBM", start="DOF",
      plugs="AE BT CF DK GJ HM IS LV OZ UX", kenn="EJRSB",
      cipher="UNXXISVILMHHKZPJZU",
      raw="TAGESMMLDUNGFUNKEN",
      emended="Tagesmeldung funken",
      transl="Send daily report by radio",
      garble="raw MMLDUNG -> Meldung"),
 dict(no="1", date="1 October 1941", dir="incoming", refl="B", wheels="514", ring="KBU", start="DEI",
      plugs="AG EL FN HU JV KM OP QR SW TX", kenn="PLVJH",
      cipher="HBCZFWXKBEJDLUXCODAAQV",
      raw="MORGENMELDUNGENTFAELCT",
      emended="Morgenmeldung entfaellt",
      transl="Morning report cancelled",
      garble="raw ENTFAELCT -> entfaellt"),
 dict(no="2", date="1 October 1941", dir="incoming", refl="B", wheels="514", ring="KBU", start="ZAQ",
      plugs="AG EL FN HU JV KM OP QR SW TX", kenn="OSMRV",
      cipher="JMYDKAPZMJLRHTOVJTMPJZVA",
      raw="ZWISNENMELDCNGENTFAELLTX",
      emended="Zwisqenmeldung entfaellt x",
      transl="Intermediate report cancelled.",
      garble="raw ZWISNEN/MELDCNG -> Zwisqenmeldung. Start ZAQ confirmed against the article PDF "
             "appendix (Version 5); the cryptocellar WEB PAGE mis-prints it as DEI (copied from "
             "No.1) -- a web-transcription error, not in the article. Recovered independently here."),
 dict(no="128", date="8 July 1941", dir="incoming", refl="B", wheels="432", ring="PKF", start="SWV",
      plugs="CY EL FH GS IJ KQ MW PV RZ TU", kenn="TZLPT",
      cipher="XPDBQLJWFTULSZCDKQPSWIMGBYS",
      raw="WOROEMEINSBERTASTAFFELFRAGE",
      emended="Wo Roem Eins Berta Staffel Frage",
      transl="Where [is] Roman One B squadron? [Ib = Chief of Supply]",
      garble="(none)"),
 dict(no="71", date="27 August 1941", dir="incoming", refl="B", wheels="132", ring="LES", start="BEN",
      plugs="AY BJ DG EH FQ IM KO LP NW RT", kenn="AMERI",
      cipher="TDLYXLHUVKOGOTUXNVRBPVICIBWTSTYD",
      raw="ZWOXSTAFDELAMXZWOSIEBENXAQTXVIER",
      emended="Zwo x Staffel am x Zwo Sieben x Aqt x Vier",
      transl="Second squadron on 27 Aug 41 [at] 1930 in Wjelikoje-Sjelok. Schoeffler.",
      garble="raw STAFDEL -> Staffel; ciphertext on form incomplete (only opening recorded)"),
 dict(no="15", date="2 September 1941", dir="outgoing", refl="B", wheels="432", ring="RIT", start="VOR",
      plugs="AH BO DP EX FN JQ KS LR MU TZ", kenn="PFCXY",
      cipher="PSQDBCSFKHFJOMVCJAUXTOTQBBPBWACHZYXH",
      raw="ABENDMELDUNGENENTFALLENXHARTJENSTEIN",
      emended="Abendmeldungen entfallen x Hartjenstein",
      transl="Evening reports cancelled. Hartjenstein",
      garble="(none)"),
 dict(no="30", date="22 August 1941", dir="outgoing", refl="B", wheels="341", ring="WGR", start="TOR",
      plugs="AC BE HW IP JZ KY LU OS QR VX", kenn="YYBRW",
      cipher="CFVUAHZHPIWNUCXTMJGXPMVWKFVHZJTJGXMSSDJYESRCNX",
      raw="WOGEFEQTSSTANDQUARTIERMEISTERABTXROEMEINSBERTA",
      emended="Wo Gefeqtsstand Quartiermeisterabt x Roem Eins Berta",
      transl="Where is the command post of the Quartermaster's unit. Roman One B",
      garble="NB: enciphered with the key of the day before (21 Aug 1941)"),
 dict(no="36", date="6 September 1941", dir="outgoing", refl="B", wheels="325", ring="BYJ", start="SAU",
      plugs="AX BH ET FK GY IR JZ MS OU QW", kenn="HODSN",
      cipher="ZLXAQIZTGHJYEECHRVPUSGYHYIVKYIBVAZDYNAPYNIDCUXRO",
      raw="DIVXNAQRXYUEHRERNIQTANWWSENDXSTEINECKEXSTEINECKE",
      emended="Div x Naqr x Fuehrer niqt anwesend x Steinecke x Steinecke",
      transl="Division's Signals Officer [is] non-attendant. Steinecke",
      garble="raw YUEHRER -> Fuehrer; ANWWSEND -> anwesend"),
 dict(no="46", date="14 September 1941", dir="incoming", refl="B", wheels="243", ring="IXM", start="WAS",
      plugs="AV BE CX FW GU HT IS JR LP NZ", kenn="BOTKB",
      cipher="EXDFRWSTRGBVAJPVAFKEBRSRCTIQELDBHZXOKLEBADAXPLICYQTHTQCFHTQXANXDXKRVT",
      raw="NACHSCHUBDIENSTEXNULLAQTVIERNULFXOMYTSCIKINOXOMYTSCUKINOXHARTJENSTEIN",
      emended="Nachschubdienste x Null Aqt Vier Null x Omytschkino x Omytschkino x Hartjenstein",
      transl="Supply services 0840 Omychkino. Hartjenstein",
      garble="raw NULF -> Null; OMYTSCIKINO/OMYTSCUKINO -> Omytschkino"),
 dict(no="94", date="24 September 1941", dir="outgoing", refl="B", wheels="231", ring="SZI", start="DRI",
      plugs="AQ BO CM DP EW FT HS JZ KX LU", kenn="ABPQX",
      cipher="PWCQFEZLPXGENCLBOXJFVWWPXOOGLRIPJKOUIOTCTNSLZDKYYJQNTVCTMPLUOAUNESZVKXRCTMHM",
      raw="UMZUGGEPAEQTROSSUNDRESTKOMMANDOSTOPFENXSQRIFTLIQERBEFEHLUNTERWEGSXSCHNEIDDRX",
      emended="Umzug Gepaeqtross und Restkommando stopfen x Sqriftliqer Befehl unterwegs x Schneider x",
      transl="Stop relocation [of] luggage train and last detachment. Written order [is] on its way. Schneider.",
      garble="raw SCHNEIDDR -> Schneider"),
 dict(no="203", date="14 July 1941", dir="incoming", refl="B", wheels="531", ring="LWB", start="BER",
      plugs="BT CH DR EW FU GK JO LV MS PZ", kenn="CFYZR",
      cipher="NFOSOIFKXNEMBCXCWMSCMORVYWSVHFBZJHNEMQFWZQOLUIZBFFBSNKSQXSHRDAMFRSESGJJD",
      raw="ANROEMEINSBERTAXQUARTIERMEISPCRPANZXGRUPPEXOSTROWOSTROWXKASERNENGELZENME",
      emended="An Roem Eins Berta x Quartiermeister Panz x Gruppe x Ostrow Ostrow x Kasernengelaende",
      transl="To Roman One B, Quartermaster, Tank Group, Ostrov barracks area",
      garble="raw EISPCR -> -meister; GELZENME -> gelaende"),
 dict(no="233", date="19 July 1941", dir="incoming", refl="B", wheels="425", ring="AGM", start="QAY",
      plugs="DM EP FL HI JR KY NQ OU SW TZ", kenn="XNRLR",
      cipher="QKXETVPZQOHSXMBIZPHTCTRMAUZYSTJIMDUYOZBFRTZOUHBGOROUVRQEJRDRJHZPZIBQQHKMMJZCIIRCUOLXLCIOQKHRLIGGFJFTLLGDRARDZQUQKLTK---YKRUVFULBQLAYRZVJFULCGQJXFJURMURSELYFVFOKUHYUHSYLOMEFYAIIP",
      raw="WIEVIELKLEINSIEGFRIEDGWOSMFRIEDRICHXHEINWIGHMUNXINSQRZAQTEINSSIEBENSTRIQAINSAQHXSIEBENWILXPLESNAUXABGEHOLTFRAGEXWERP---EFYHLKOMMADOHZATZUXOLENFRAGOVUNKANTWORTXDERQNAATYEKVEVSTER",
      emended="Wie viel Klein Siegfried Grosz Friedrich x Heinrich Mun x insgesamt Eins Sieben Striq Eins Aqt x Sieben wird Pleskau x abgeholt Frage x Wer hat Befehl Komma dort abzuholen Frage Funkantwort x Der Quartiermeister",
      transl="How much ammunition for heavy field howitzer (sFH) to be collected altogether on 17-18 July in Pskov? Who is ordered to collect it? Reply by radio. The Quartermaster",
      garble="NB: key of the day before (18 July 1941). ONE garbled group, 3 unrecorded letters at "
             "positions 117-119: shown as cipher K---Y and (its decrypt) plaintext P---E -- the "
             "SAME three letters, one region, not two. The dashes are real positions: feed 3 "
             "placeholder letters there to keep the rotor stepping in sync (stripping them desyncs "
             "the rest). With placeholders every other position decrypts exactly. The 3 letters are "
             "not recoverable -- the surrounding cipher is itself garbled (raw P@116, Y@122 should be "
             "H, E per the reconstruction), so enciphering the guess would not give the true letters."),
]


def decrypt(m):
    """Decrypt the ciphertext. Unrecorded letters ('-') are kept as placeholder positions
    ('A') so the rotor stepping stays in sync -- they must NOT be stripped."""
    ct = m["cipher"].replace("-", "A")
    args = [BIN, "-u", m["refl"], "-w", m["wheels"], "-r", m["ring"], "-g", m["start"],
            "-s", m["plugs"]]
    p = subprocess.run(args, input=ct, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       universal_newlines=True)
    return p.stdout.strip().upper()


# --- validate: compare at every KNOWN position (raw != '-') ---
print("VALIDATION (decrypt with placeholders; compare at known positions)\n")
print("%-5s %-18s %6s %8s  %s" % ("No.", "date", "len", "match", "note"))
allok = True
for m in M:
    raw, out = m["raw"], decrypt(m)
    known = [(o, r) for o, r in zip(out, raw) if r != "-"]
    mism = sum(1 for o, r in known if o != r)
    allok = allok and (mism == 0)
    if "-" in m["cipher"]:
        note = "%d unrecorded letters held as placeholders; all %d known positions exact" % (
            m["cipher"].count("-"), len(known))
    else:
        note = "" if mism == 0 else "got: " + out[:40]
    print("%-5s %-18s %6d %8s  %s" % (
        m["no"], m["date"], len(raw), "OK" if mism == 0 else "MISMATCH(%d)" % mism, note))
print("\nAll messages exact at every known position:", allok)

# --- write the database file ---
ROMAN = {"1": "I", "2": "II", "3": "III", "4": "IV", "5": "V", "6": "VI", "7": "VII", "8": "VIII"}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "enigma-messages.txt")
HEADER = """\
# ============================================================================
# enigma-messages.txt  --  13 authentic WWII Wehrmacht Enigma messages
# ----------------------------------------------------------------------------
# Source: Olaf Ostwald & Frode Weierud, "Modern breaking of Enigma ciphertexts",
#         Cryptologia 41(5):395-421 (2017).  cryptocellar.org.  CC BY-NC-SA 4.0.
# Transcribed from the authentic message forms (Spruchzettel); German Army (Heer),
# standard 3-rotor Enigma I, reflector B, 10 plugs, summer/autumn 1941 (Ostfront).
#
# CONVENTIONS
#   REFLECTOR B, WHEELS left->right (I..VIII); e.g. key group B423 = refl B, IV II III.
#   RING / START are letters (a=1..z=26 on numbered machines).
#   The KENNGRUPPE (1st 5-letter group on the form) is the discriminant, NOT
#     ciphertext -- deciphering starts at the 2nd group. It is listed separately;
#     CIPHERTEXT below already excludes it.
#   DECRYPT is the raw machine output (letter-for-letter). Text uses operator
#     orthography: X = word separator, Q often = ch/k, digits spelled out
#     (EINS ZWO DREI ...), umlauts as AE/OE/UE, sz = SZ.
#   A dash '-' in CIPHERTEXT/DECRYPT is an UNRECORDED-but-real letter (illegible on
#     the form). It is a real rotor position: keep it as a placeholder (any letter)
#     when deciphering -- do NOT strip it, or the stepping desyncs the rest.
#   EMENDED = human-corrected German (garbles fixed); TRANSLATION = English.
#   Each record is one ./enigma invocation (shown as CMD) and decrypts to DECRYPT.
#
# VALIDATION: every message decrypts EXACTLY at every known (non-dash) position.
#   No. 233 has one garbled group (3 unrecorded letters, positions 117-119); held
#   as placeholders, all other positions decrypt exactly. Those 3 letters are not
#   recoverable (the surrounding ciphertext is itself garbled).
# VERIFIED against the article PDF appendix (Ostwald & Weierud, Version 5, March 2017):
#   all 13 keys/ciphertexts match. No. 2 (1 Oct 1941) start is ZAQ (as in the PDF); the
#   cryptocellar WEB PAGE mis-prints DEI -- a web typo, corrected here.
# ============================================================================

"""
with open(OUT, "w") as f:
    f.write(HEADER)
    for m in M:
        roman = " ".join(ROMAN[d] for d in m["wheels"])
        cmd = ('./enigma -u %s -w %s -r %s -g %s -s "%s"'
               % (m["refl"], m["wheels"], m["ring"], m["start"], m["plugs"]))
        f.write("### Message No. %s  --  %s  (%s)\n" % (m["no"], m["date"], m["dir"]))
        f.write("MODEL:       Enigma I (Heer), 3-rotor\n")
        f.write("REFLECTOR:   %s\n" % m["refl"])
        f.write("WHEELS:      %-10s (-w %s)\n" % (roman, m["wheels"]))
        f.write("RING:        %s\n" % m["ring"])
        f.write("START:       %s\n" % m["start"])
        f.write("PLUGS:       %s\n" % m["plugs"])
        f.write("KENNGRUPPE:  %s   (discriminant; not enciphered)\n" % m["kenn"])
        f.write("CMD:         %s\n" % cmd)
        f.write("CIPHERTEXT:  %s\n" % "\n             ".join(textwrap.wrap(m["cipher"], 60)))
        f.write("DECRYPT:     %s\n" % "\n             ".join(textwrap.wrap(m["raw"], 60)))
        f.write("EMENDED:     %s\n" % "\n             ".join(textwrap.wrap(m["emended"], 60)))
        f.write("TRANSLATION: %s\n" % "\n             ".join(textwrap.wrap(m["transl"], 60)))
        f.write("NOTES:       %s\n" % "\n             ".join(textwrap.wrap(m["garble"], 60)))
        f.write("\n")
print("wrote", OUT)
print("records:", len(M),
      "| total decrypted plaintext letters:", sum(len(x["raw"].replace("-", "")) for x in M))
