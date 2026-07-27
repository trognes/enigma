#!/usr/bin/env python3
# Generate the "wehrmacht" scoring language -- telegraphic German for real Wehrmacht
# traffic -- as ngrams/wehrmacht_*.txt, by bending the bundled prose German tables
# (ngrams/german_*.txt) toward the published telegraphic
# statistics of Sullivan & Weierud, "Breaking German Army Ciphers" (Cryptologia 2005),
# Appendix C -- Fig 17 (single-letter frequencies) and Fig 18 (top-400 trigrams), taken
# over ~20,000 letters of 1941 Enigma decrypts.
#
# Why: real HG Nord plaintext is telegraphic (X=space ~6-7%, Q for ch, spelled numbers),
# far from prose German (X~0.07%). The prose tables penalise the true plaintext, so on
# short/garbled real messages a spurious plugboard can outscore the truth. These are
# AGGREGATE published statistics -- not the plaintext of any specific validation message
# -- so using them keeps the eval/ message sets held out.
#
# Method (marginal matching, tempered by strengths A_mono, B_tri):
#   mono := Fig 17 telegraphic frequencies (verbatim)
#   bi   := prose bigram  * prod_i r1(c_i)^A
#   tri  := prose trigram * prod_i r1(c_i)^A * r3(gram)^B
#   quad := prose quad    * prod_i r1(c_i)^A * r3(ABC)^B * r3(BCD)^B
# r1 = telegraphic/prose monogram ratio (Fig 17); r3 = telegraphic/prose trigram ratio
# (Fig 18, =1 outside the top-400 or absent from prose). This makes the quad table's
# folded low-order marginals telegraphic, so -a (which folds all orders from the quad
# windows) and -q/-t/-b/-m all become telegraphic. Usage:  -l wehrmacht
#
# Defaults A=0.5, B=2.0 were the net-best strength over the 69-message held-out set
# (eval/eval_telegraphic.py); override with A=/B= env vars.
#
# W_MAX caps the per-gram reweight factor w (below). r3(g) = telegraphic/prose ratio
# is a ratio of two small counts for most of Fig 18's 400 trigrams (median prose
# trigram count in the teens), so for the minority with a near-zero prose denominator
# (e.g. QSX: prose count 2) the ratio is not a signal, it is denominator noise --
# r3 ranges from 0.26 to 4.08e6 over the 400 entries, and quadgrams multiply TWO such
# ratios (B=2.0 each), so a single quadgram's weight was observed reaching 8.3e20
# (QSXA, from a prose count of 1). Uncapped, that overflowed the 32-bit unsigned
# `load_counts()` parses counts into (enigma.cc): sscanf("%u", ...) on an out-of-range
# value is undefined behaviour in C, and on this glibc it happened to saturate to
# UINT32_MAX -- so 843 distinct quadgrams (0.23% of the table, including real
# telegraphic markers like NULL, XEIN, XKON) were silently tied at one indistinguishable
# value, together holding ~68% of the table's total probability mass. W_MAX=1000 clips
# the tail (roughly the top 1.5% of quadgram weights) while leaving the well-evidenced
# bulk untouched (median weight ~1.1, p90 ~7) and keeps every possible output count
# below ~4.7e8 -- under 11% of UINT32_MAX, comfortable headroom against the largest
# prose count (~3.7e6) at the cap. See PERFORMANCE.md 6.17.
import os

W_MAX = float(os.environ.get("W_MAX", "1000"))
HERE = os.path.dirname(os.path.abspath(__file__))
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
PROSE = NGRAMS
OUT = os.environ.get("OUTDIR", NGRAMS)
LANG = os.environ.get("LANG_NAME", "wehrmacht")   # output language name
A = float(os.environ.get("A", "0.5"))   # monogram-marginal strength
B = float(os.environ.get("B", "2.0"))   # trigram-marginal strength

# --- Appendix C, Fig 17: single-letter frequencies in 1941 Enigma decrypts (%) ---
FIG17 = dict(A=6.09,B=2.20,C=0.72,D=2.90,E=12.91,F=3.03,G=2.81,H=1.88,I=6.16,
             J=0.41,K=1.99,L=3.90,M=2.72,N=8.41,O=4.42,P=1.47,Q=2.02,R=6.87,
             S=6.23,T=5.41,U=4.47,V=1.38,W=1.68,X=6.98,Y=0.89,Z=2.05)
# --- Appendix C, Fig 18: 400 most frequent trigrams (counts per ~20,000 letters) ---
TELE3 = {"ABE":28,"AGE":18,"AJA":11,"AKX":14,"ALL":12,"ALT":13,"AMM":17,"AND":36,"ANG":24,"ANN":28,"ANX":15,"ANZ":24,"AQT":63,"AQX":10,"ARM":16,"ARS":18,"ART":19,"ASS":12,"AUF":27,"AUM":15,"AUP":11,"AUS":28,"AXE":12,"AXM":15,"AXS":15,"BEF":12,"BEN":37,"BER":46,"BES":30,"BET":18,"BEZ":13,"BIS":13,"BOR":15,"CHE":25,"CHN":11,"CHO":16,"DEN":17,"DER":67,"DIE":22,"DIV":36,"DNA":12,"DOR":18,"DRE":55,"DUN":15,"DUR":10,"EBE":55,"EBS":10,"EDS":13,"EFE":16,"EGE":36,"EGF":18,"EGU":18,"EHL":16,"EHR":33,"EID":13,"EIL":12,"EIN":194,"EIQ":13,"EIS":12,"EIT":17,"EIX":24,"ELD":21,"ELF":12,"ELL":18,"ELX":11,"EME":20,"EMX":14,"ENA":21,"END":34,"ENE":20,"ENF":80,"ENI":26,"ENM":11,"ENN":15,"ENS":34,"ENT":16,"ENX":47,"ENZ":11,"EQS":46,"EQT":15,"ERA":22,"ERB":23,"ERD":17,"ERE":41,"ERF":18,"ERI":17,"ERK":20,"ERL":16,"ERN":20,"ERP":23,"ERS":40,"ERT":28,"ERU":21,"ERV":20,"ERW":15,"ERX":60,"ERZ":12,"ESE":20,"ESP":15,"EST":46,"ETE":14,"ETR":21,"ETT":11,"ETZ":23,"EUN":23,"EXS":12,"FEH":11,"FEL":15,"FFE":15,"FLE":17,"FRI":17,"FSE":12,"FST":11,"FUE":108,"FZX":14,"GAB":20,"GEB":12,"GED":14,"GEF":13,"GEN":58,"GER":18,"GES":23,"GFR":17,"GUN":36,"HAR":11,"HAU":11,"HEI":12,"HNE":14,"HOW":10,"HRE":13,"IDE":12,"IEB":41,"IED":18,"IEG":26,"IEN":18,"IER":89,"IES":13,"IEX":19,"IGE":14,"INA":33,"IND":12,"INE":11,"INF":15,"ING":21,"INM":15,"INS":137,"INU":12,"INX":26,"IQT":31,"IQX":13,"IST":15,"ITE":14,"ITT":20,"IVX":25,"JEN":11,"KFZ":12,"KLA":29,"KOM":32,"KOR":14,"LAM":26,"LDE":12,"LEG":23,"LEI":19,"LEN":20,"LEX":10,"LFX":12,"LIE":12,"LIQ":20,"LIT":10,"LLE":25,"LLN":21,"LLX":34,"LNU":22,"LTE":13,"LXA":14,"LXU":10,"MAR":18,"MEI":14,"MEL":17,"MER":16,"MIT":28,"MME":18,"MOR":20,"MOT":13,"MUN":17,"MXE":11,"MXK":11,"MZW":10,"NAN":13,"NAQ":30,"NAU":12,"NAX":37,"NBE":16,"NDE":32,"NDX":19,"NEI":15,"NEN":16,"NEU":25,"NFS":15,"NFX":27,"NGE":39,"NGS":20,"NGX":22,"NIE":18,"NIQ":11,"NIX":11,"NMA":15,"NNA":14,"NNE":17,"NNU":10,"NOR":22,"NSA":15,"NSE":11,"NSF":11,"NSN":14,"NSS":15,"NST":32,"NSV":12,"NSX":32,"NSZ":12,"NTE":12,"NUL":87,"NUN":11,"NXD":11,"NXK":11,"NXR":12,"NXS":17,"NZE":13,"OEM":34,"OFF":11,"OKX":12,"OMA":11,"ONN":12,"ONS":13,"ONU":12,"ONX":15,"ORD":29,"ORI":20,"ORO":16,"ORP":13,"ORT":19,"ORW":13,"OSS":17,"OST":21,"OWA":12,"PAN":17,"PFL":21,"PPE":11,"PRO":11,"PRU":10,"PTR":10,"QFU":11,"QSX":20,"QTE":14,"QTX":40,"RAN":15,"RAS":13,"RAU":29,"RBE":14,"RDE":19,"REI":76,"REN":13,"RES":12,"RFU":10,"RIE":32,"RIN":35,"RIQ":36,"RKU":11,"ROE":36,"ROS":21,"RPF":20,"RPS":12,"RSQ":14,"RST":31,"RTA":17,"RTE":11,"RUE":11,"RUN":23,"RVE":11,"RVI":11,"RXA":12,"RXN":12,"RXS":13,"RYY":13,"SAU":11,"SBE":14,"SCH":47,"SEI":14,"SEN":15,"SEQ":40,"SET":18,"SFU":14,"SGA":16,"SIE":59,"SIN":11,"SNU":13,"SPR":18,"SQL":11,"SSE":28,"SSI":23,"SST":11,"STA":49,"STE":49,"STO":45,"STR":42,"STU":17,"STX":13,"SVI":13,"SXA":15,"SXE":11,"SXS":14,"SZE":16,"SZW":14,"TAF":11,"TAN":35,"TAR":12,"TEI":32,"TEL":15,"TEN":38,"TER":36,"TEX":15,"TIN":13,"TON":17,"TOP":32,"TRA":15,"TRI":45,"TRX":11,"TSC":14,"TST":11,"TTE":21,"TUN":13,"TXA":12,"TXE":13,"TXS":10,"TYY":11,"TZT":11,"UEB":11,"UEH":22,"UEN":71,"UER":18,"UHR":15,"ULL":90,"UND":53,"UNG":83,"UNI":14,"UNX":14,"UPT":11,"USG":15,"UST":13,"VER":53,"VIE":80,"VON":24,"VOR":26,"WAX":19,"WEG":13,"WES":20,"WON":15,"WOS":15,"WOX":24,"XAN":18,"XAQ":37,"XAR":13,"XBE":29,"XBO":14,"XDI":24,"XDR":29,"XEI":78,"XEL":11,"XFU":27,"XGE":22,"XHA":22,"XIN":23,"XKA":21,"XKD":11,"XKO":22,"XMA":22,"XMO":28,"XNA":13,"XNO":12,"XNU":13,"XPA":20,"XPO":12,"XRO":28,"XSC":22,"XSE":13,"XSI":19,"XST":38,"XSZ":10,"XUH":11,"XUN":14,"XVE":11,"XVI":29,"XVO":22,"XZU":12,"XZW":32,"ZEN":16,"ZUG":11,"ZUM":20,"ZWO":90,"ZYX":13}


def load(path):
    tot, d = 0, {}
    for ln in open(path):
        p = ln.split()
        if len(p) == 2 and p[1].isdigit():
            d[p[0]] = int(p[1]); tot += int(p[1])
    return d, tot


def main():
    os.makedirs(OUT, exist_ok=True)
    pm, pmt = load(os.path.join(PROSE, "german_monograms.txt"))
    pt3, pt3t = load(os.path.join(PROSE, "german_trigrams.txt"))
    r1 = {L: (FIG17[L] / 100.0) / (pm.get(L, 1) / pmt) for L in FIG17}
    tele3t = sum(TELE3.values())
    r3 = {g: (c / tele3t) / (pt3[g] / pt3t) for g, c in TELE3.items() if pt3.get(g, 0) > 0}

    # monograms: telegraphic verbatim
    with open(os.path.join(OUT, "%s_monograms.txt" % LANG), "w") as o:
        for L, pct in sorted(FIG17.items()):
            o.write("%s %d\n" % (L, int(round(pct * 100000))))

    def reweight(suffix, order):
        pin = os.path.join(PROSE, "german_%s.txt" % suffix)
        with open(os.path.join(OUT, "%s_%s.txt" % (LANG, suffix)), "w") as o:
            for ln in open(pin):
                p = ln.split()
                if len(p) != 2 or not p[1].isdigit():
                    o.write(ln); continue
                g, c = p[0], int(p[1]); w = 1.0
                if A:
                    for ch in g:
                        w *= r1.get(ch, 1.0) ** A
                if B and order == 3:
                    w *= r3.get(g, 1.0) ** B
                if B and order == 4:
                    w *= r3.get(g[0:3], 1.0) ** B * r3.get(g[1:4], 1.0) ** B
                w = min(w, W_MAX)   # clip denominator-noise outliers -- see W_MAX above
                o.write("%s %d\n" % (g, max(1, int(round(c * w)))))

    reweight("bigrams", 2)
    reweight("trigrams", 3)
    reweight("quadgrams", 4)
    write_source()
    print("wrote %s_*.txt (telegraphic German) to %s  (A=%g mono, B=%g tri, W_MAX=%g)"
          % (LANG, OUT, A, B, W_MAX))
    print("source: Sullivan & Weierud 2005, Appendix C (Fig 17 + Fig 18, 400 trigrams)")


def write_source():
    """Emit the ORIGINAL published Appendix-C tables verbatim (unmodified reference data,
    not the derived scoring tables above)."""
    with open(os.path.join(HERE, "appendix-c-fig17-monograms.txt"), "w") as o:
        o.write("# Sullivan & Weierud, \"Breaking German Army Ciphers\" (Cryptologia 2005),\n"
                "# Appendix C, Figure 17: single-letter frequencies in 1941 Enigma decrypts.\n"
                "# Format: <LETTER> <frequency %>.  Sums to 100.00.  Original published values.\n")
        for L, pct in sorted(FIG17.items(), key=lambda kv: (-kv[1], kv[0])):
            o.write("%s %.2f\n" % (L, pct))
    with open(os.path.join(HERE, "appendix-c-fig18-trigrams.txt"), "w") as o:
        o.write("# Sullivan & Weierud, \"Breaking German Army Ciphers\" (Cryptologia 2005),\n"
                "# Appendix C, Figure 18: the 400 most frequent trigrams in ~20,000 letters of\n"
                "# 1941 Enigma decrypts.  Format: <TRIGRAM> <count per ~20,000 letters>.\n"
                "# Original published values (top-400 only; not a complete 26^3 table).\n")
        for g, c in sorted(TELE3.items(), key=lambda kv: (-kv[1], kv[0])):
            o.write("%s %d\n" % (g, c))


if __name__ == "__main__":
    main()
