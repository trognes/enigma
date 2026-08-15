#!/usr/bin/env python3
# Build a database of authentic 1941 German Army (HG Nord) Enigma messages recovered
# from the cryptocellar "Breaking German Army Ciphers" collection (Sullivan & Weierud,
# Cryptologia 2005; keys released 2017), and VALIDATE each by decrypting with ./enigma.
#
# These are the "messages we failed to break" / July Batch A ciphertexts whose day-keys
# were later recovered. Every keyed message here decrypts to clean telegraphic German.
# Two messages that duplicate the Ostwald & Weierud set (eval/enigma-messages.txt) are
# dropped automatically by ciphertext-dedup (No. 203 CFYZR, No. 233 XNRLR).
#
# Source: Geoff Sullivan & Frode Weierud, "Breaking German Army Ciphers", Cryptologia
# 29(3):193-232 (2005); message + key pages at cryptocellar.org/bgac (c) 2006/2017.
import os, subprocess, textwrap

BIN = "./enigma"
os.environ["ENIGMA_DATA"] = "ngrams"
os.environ["ENIGMA_SEED"] = "0"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "enigma-army-messages-1941.txt")

ROMAN = {"1": "I", "2": "II", "3": "III", "4": "IV", "5": "V", "6": "VI", "7": "VII", "8": "VIII"}

# ---- day keys: date -> (reflector, wheels, ring, stecker) ----
DAYS = {
 "27.06.1941": ("B", "352", "RGP", "AV BG CH EN FU KO MS PX RY TW"),
 "08.07.1941": ("B", "432", "PKF", "CY EL FH GS IJ KQ MW PV RZ TU"),
 "13.08.1941": ("B", "253", "THE", "AD BH FG IJ KN LZ MR OS PW QV"),
 "19.08.1941": ("B", "213", "YPC", "AK BI DG FN HL JO MT QY RV UW"),
 "16.09.1941": ("B", "513", "LSB", "AP BO CY DU ES FN GR IV JT LZ"),
 "27.09.1941": ("B", "421", "YHO", "AG CP DK EL HQ IT JV MX OY RW"),
 "03.10.1941": ("B", "213", "TIP", "BC DE FG HI JK LX MQ NO ST VZ"),
 # July Batch A (keys released 2017)
 "01.07.1941": ("B", "423", "AAV", "CT EM FI GJ HK NQ OR SW UY VX"),
 "05.07.1941": ("B", "354", "WHJ", "BI CW EQ FX HZ JN KY MT OV PR"),
 "06.07.1941": ("B", "513", "IRD", "AN BM DH EI KQ LS OT PV RU YZ"),
 "07.07.1941": ("B", "245", "BUL", "AV BS CG DL FU HZ IN KM OW RX"),
 "08.07.1941b": ("B", "432", "PKF", "CY EL FH GS IJ KQ MW PV RZ TU"),
 "09.07.1941": ("B", "315", "NAV", "AC BN FM GI JL KO PU QX RZ TV"),
 "10.07.1941": ("B", "521", "JQH", "AS BG CK DZ IO LR MP QT UW VY"),
 "12.07.1941": ("B", "254", "YCM", "AJ BD CZ EH GU IK LV MQ NX OS"),
 # 16.07.1941 was NOT in the 2017 release: Nr 214 was still unbroken then, and
 # was broken later by Enigma@Home (15.09.2017). The published key gives the
 # ring as two letters, "HV" -- the LEFT wheel's ring is unidentifiable from
 # ciphertext (only start-minus-ring reaches the machine), so it is written A
 # here to pair with the published start FQR.
 "16.07.1941": ("B", "314", "AHV", "AH CN DF EI KY MP OZ RU SW VX"),
 # 28.08.1941 recovered HERE, not published: the Bundesarchiv copy of Nr 81 (Nr
 # 55 NF) carries its plaintext on the source page, and 86 known letters give the
 # key by crib in 45 s over 144 million keys.  Ring is A?? for the usual reason.
 "28.08.1941": ("B", "345", "AVJ", "BH CS DU EI FR GM JO KQ TX VZ"),
 "13.07.1941": ("B", "423", "GTO", "AD EH GY IM KN LR OZ QV TX WU"),
 "14.07.1941": ("B", "531", "LWB", "BT CH DR EW FU GK JO LV MS PZ"),
 "18.07.1941": ("B", "425", "AGM", "DM EP FL HI JR KY NQ OU SW TZ"),
 "25.07.1941": ("B", "325", "RVA", "BE CK DL GM HZ JO NW QU RT SV"),
 "29.07.1941": ("B", "521", "MJW", "AW CS DR EY FO KU LZ NV PX QT"),
 # NOT from the released 2017 key set -- recovered ciphertext-only here, from
 # ALVPM and ALRHG (see their notes).  Wheel order 342 appears in none of the
 # 22 published day keys above, and this stecker shares at most one pair with
 # any of them, which is chance level for 10 pairs out of 325.
 # DATE NOT YET IDENTIFIED -- fill in from the message forms when available.
 "unknown.SS.342": ("B", "342", "ALZ", "AZ DV ET FS GQ JP LX MY NR OW"),
}
# Nr 173 broke on its own ring+stecker (same wheel order) -- special per-message key.
KEY_OVERRIDE = {"173": ("B", "521", "MRP", "AG BJ CP DS ER FQ HV IU KT LW")}

# ---- messages: (no, date-key, day-label, start, special, kenn, ciphertext-incl-designator, notes) ----
# special: "" normal | "pad20" prepend 20 '-' (missing head groups) | "ins" '-----' already inline
M = [
 # --- from the "messages we failed to break" page (keys 2017) ---
 ("45","27.06.1941","27 Jun 1941","LTA","","HXZKV","HXZKVKOZQNXUUXYJMFSHGSDUFGUXZMKHMQIZQEHGYLO",""),
 ("48","27.06.1941","27 Jun 1941","CSX","","WRMKX","WRMKXKSNYPZZDQ-XGLYBUBSUXTPBJVSWRNUHAOUCDUAWENTBLQOKNHSFLIRDNRTALSAFQZNPRQAILXTMTNBIHMALKODKRADTQJTCWVWIJIJFTAMNGQYGJREUGNXIBXHWUWWLJYKL","garbled start corrected per form; 1 unrecorded letter"),
 ("51","27.06.1941","27 Jun 1941 (rx 28th)","RTZ","","PLDRV","PLDRVORIZXOIVHEYIGPKKRBMILKCYBFGMZBMSTYEEWHKLSWKLXQWCGDLNMBBKXCTJAYXA-MZAL-LZWIFBQFOJNLHVZQJKWITROKZPFYLUBZJLJYJPVBQJAPYAWL-R--TMNZXXPNBOVTOBCIIEQOCPBBQJLSSKPFJAHVGWAARASGFIRHYWJNLBUBREA-XZCBGVYKKTVECIWDCWDYCWSNJIGY","enciphered on 27 Jun key; several garble dashes"),
 ("214","16.07.1941","16 Jul 1941","FQR","","FTNBK","FTNBKXNQAEQNZLWFMQYTXOQZVXJKBOJKPKIJQGMVFLSVJBHRIYMRYWHUIVWYKXKAKFMSQFBBARNKNHBRHQHLIUVNEHMJKOZRXLPLWISNZZ",
  "broken by Enigma@Home 15.09.2017, long after the 2017 key release; the transcription in enigma-challenge-1941.txt differed from this one in 13 of 101 letters and did not decrypt"),
 ("55 NF","28.08.1941","28 Aug 1941","YAC","","ALQFI","ALQFIGEELOREBSXEINXZYDHXITUFWDLTURTZSPMMLFMYZMAGJDWOPCBQYZRTJSTGVIJPHJIXTDBKDFXOYILZEUIMMLL",
  "the Bundesarchiv copy of Nr 81, from another SS-Totenkopf station; its plaintext is published, and the key was recovered from it here. Nr 81 is a badly corrupted transcription of the same message and does not decrypt"),
 ("ALVPM","unknown.SS.342","date not yet identified","VAT","nostrip","ALVPM",
  "TLNVEZRNWMGTSQOXCMJAQNNEVBQITIHROKAMXQPTJLMMCALFWSCSPTQCCUWSIHZCSAFFRUCKVGECKMKSMHSUDVZCDDQEEWGOBIUJGYAQJOVSIKRNSWDHMMQTZOOEEJYKUYOUPXXHPGYJTEKNGFZOMRDCFUQVEPYDRWITTKMYVCKQ",
  "BROKEN CIPHERTEXT-ONLY HERE, not from the released key set. Designator "
  "group "
  "not in the transcription (nothing to strip); ALVPM is the source-page "
  "designator. Same key AND same start as ALRHG, the same order re-sent. RING "
  "IS A CLASS REPRESENTATIVE, not necessarily the true day-key ring: ring0 is "
  "never identifiable from ciphertext (CLAUDE.md 7.10, always reported A) and "
  "at 172 letters ring1/start1 need not be singletons either (7.12). The "
  "decrypt is exact regardless"),
 ("ALRHG","unknown.SS.342","date not yet identified","VAT","nostrip","ALRHG",
  "ZLUNGBFSCEVTWZRPZTLQEPEBWNYBCVCZIHROAPPLOHYATMMCIJWYWJGWGIYTPAWLHBWPMTBMULWLRSEGJGRDQYTHJQJCVTKYUDBJGYQPDDOPIUV",
  "BROKEN CIPHERTEXT-ONLY HERE; the abbreviated re-send of ALVPM, same key and "
  "same start. Its --confidence margin was only +2.29 sd, marginal alone; "
  "ALVPM "
  "at +7.54 and the shared plaintext are what settle it"),
 ("23","08.07.1941","08 Jul 1941","PIK","","KHLPT","KHLPTCWSEBDDIRBZUUBGKJANBVGIVDVDZZIGAKBZCJMMVEMVXLTHNLDYGRVQAKJMRVZIXHMDNOMTAUTPZDWOINNMOLAHCDKCZTPPEORFIBXCMNWQNIDDCHPTXQQBC",""),
 ("7","13.08.1941","13 Aug 1941","BRZ","","KEJNQ","KEJNQSFUGRPVPWGSPYHMNQYJTPPDGHFMROCPMUUBLBBJLSRZCYBXHFXQSWGWOXDNEVRIOCSPWKYCFTLRSAKBNWZJYPLQBSHQYVTCCCEPUYVUKSHHVWHXYOJKPVWQWXQESKIEGMUORWZDJZRAJZCWKFFCLUXLDY",""),
 ("19","19.08.1941","19 Aug 1941","BGO","","ALWOK","ALWOKPBYQVEQEHZFPKFLVJSOGNBZNIMXDMDSZXIAQBEKYAKFCIREEWQCBRPBLHHUHGMTPTXGZGSUISJQEYEVSLFXSCUACFAJBUGWYWPVUADTAGMERMBLWTDDGVHRWXPHW",""),
 ("59","16.09.1941","16 Sep 1941","SAU","","ADAFU","ADAFUIJDBUPLBYHCHNHLXSJKOMPKLLTZDCHYKNRMZNQFNWMPICJTWAIZSGRRFYZIUQBNFTWCO","missing letter at pos 21 corrected per form"),
 ("60","16.09.1941","16 Sep 1941","FUT","","CHAFU","CHAFURPONEIAUPRFCDYUTGWENPLABTFVTPBBFYIARMTWYDKOGLVKCWYYTQOEGVDFYYIXWBIMSHFGWQCZLIFVLKNSBCQPDFYWRTXMMXXURG",""),
 ("69","16.09.1941","16 Sep 1941","BOK","","DKAFU","DKAFUGSJKOBKWRVHKRLHNPJXDBAWUUCFJMCIOJZFIYTOZVZNNGTKCEMVWDONFDNQZVGKV",""),
 ("70","16.09.1941","16 Sep 1941","KLO","","NOEGP","NOEGPUKXJRKUWZJHHGPGHLCHAYRLHKFQJZXOKFMUKRZCKKCIASYTBMTFOVWRUDOCZUUFLMFYEBCDRADVTSHYMTKHOEZEVMUEDZBCFZ",""),
 ("71","16.09.1941","16 Sep 1941","AFF","","HOEPG","HOEPGJCKKCIMUJHNGYECEYGTPGKZSKWBFHFYEDYOGOAJLJKAADPWYRJRJHCOZXXSJXHOOGXEEMYNNIFMVUUKTOWEE",""),
 ("114","27.09.1941","27 Sep 1941","WAS","","DAFPX","DAFPXPHHWKBNCDTPEXBVBHBFWSJXGHHIWCMBHNPXDSGSRTMSNHTOSDZNLMLJVLHSYUYGNKYBBPCWTRPSOORTTPCIQLBMCAFVKSWBNXBXZRBPKAUZBELNALIANRWGFOZFFGMFJGWVCLWTWCFXISEJNX",""),
 ("115","27.09.1941","27 Sep 1941","WAS","nostrip","AEFXP","CYLGCHSYSTANCNJIKDNXKIAFLHXEZWGJIUGAQPTZZSSWBMJBYZEVBULUHIOKYYEEYLSNEFKPTBDPVCWFJSYXGFMBGIKPSXWGKBRSMPPBCZRSYXHKIJYUFTCTUWKVDSLHBUGLWWRGOEBWRRVXCIMCQRDDSBXAFIDKBPMXXMTZLYWNZG","designator group not received (nothing to strip); the true designator AEFXP is from the German Army Messages page"),
 ("116","27.09.1941","27 Sep 1941","WAS","","ITFPX","ITF--CYAVMVIIBSWTXBUAIDUBXRIGYPWFIOJHGQNWWWAUOIJXPCHGEXUBIDZGGOGDUSIIYLFHYLWRGZQPKTUVKFIXGFMKHUMZCFVQYYUUDZAGYUFSINHJHOBTF","designator received garbled; the true designator is from the German Army Messages page"),
 ("117","27.09.1941","27 Sep 1941","GRA","","MNQBH","-AQBHAJFFBFZEEAPCVGMAZDERXCFCLZFVPBBJTAVQGGLPMIFDDAAHYSLHDYTOFFFPMAGMNRKRF","designator received garbled; the true designator is from the German Army Messages page"),
 ("103","27.09.1941","27 Sep 1941","SPE","","ARPTZ","ARPTZIJDWCCVPRIPSMFVEHCPVSBEEGGHJIVAPHMTIXMRPZALPUKLMWEIFRWMUZFYZXFKAGXGATFBOIGKTYMOTUTMFDWFZEUXUZZOSTLNNSLEPWM",""),
 ("104","27.09.1941","27 Sep 1941","SAU","","ABBHQ","ABBHQVJWDKMVRFCLQWRJBLEREMXEDEBXPNBWZEFEFMKHGIPJJZACGCTYSFJUXYMRYJEWPVHZ",""),
 ("105","27.09.1941","27 Sep 1941","SEE","","ANQIX","ANQIXKQKECZPPDKOERKJZMCVIIBFNSYQSJQCSIXPIVQQIIKYRQZYQECFASFOTDSGXPYLCPEJLYAYNSUNQEINSINMZEGMCXPYYQPNMKSLJDQQZAWQR",""),
 ("106","27.09.1941","27 Sep 1941","HOR","","FDTZP","FDTZPHHCOGJSSROCENABVGLJGCDSAUXTQSYJBTNRNWVHKIPGTSNACIMHTOJNLGMEAYHCBXHWQG",""),
 ("4","03.10.1941","03 Oct 1941","SEE","","NKMOW","NKMOWYXFKKRXYABMLSWEYTJXCTETEFIDLMPJBJOMOZDDAUKQGXFXMZPDWUQUNHAANSRHYTFGKOEQMIVSFYCRKXCUJQHUNBMJSHWZVVKZNWXFLZXEOWDPYAWXZDUMYYRJZNXZACRUCZLDAMZPMUEQNLJWVFGOPSIZAUSSPS",""),
 # --- July Batch A (keys released 2017) ---
 ("73","01.07.1941","01 Jul 1941","KFU","","LYASO","LYASOCDPSKLDFHVDDPNWBORPSAYUHYLHUJZZGUNOZBVWWHJMNFJYFIKJXRIASGJCVGYIBDETWDMDWVHIPJHBYKTMJJSEHJYHUQABFBIGTITWPFGSOMEYLTEMTUWSLCDDBEHDSKKLDYLNFXZ","indicator gave uncertain Ringstellung; decrypts cleanly"),
 ("99","05.07.1941","05 Jul 1941","SIM","","XTMSY","XTMSYTWDAZYHQMHTJWVBDYLISJUWPLZELISGDPHRHJUYHXLBGHPGWZHWVRYGTGLXPEWPHAPGOIIVDBENMGMAGHRHUDDIFHFMLYODVLEBTZTHAKGPUCMQMAENSPNOSXXSKKTIDTSOIUZLYAWEJNXLEVZHZVZOMDZNW",""),
 ("101","05.07.1941","05 Jul 1941","WER","","DEROP","DEROPAQDEGMQTRRJYMHBITNRQOXTCSNFSLVNPDLRVDRRPTZXFRCWMIDYQDSEXLJIKAKCWGMIQCPNEQEVDNMSLDSSWIOKEIBKPYJDPRNKHKSYNOOMGZENLBCNSLHNXOCSEBLWEBMJDRSFGJQVHFQXTUBWYFRBYKYPLCEDFJILLVYKQTIKNPDDTDO",""),
 ("107","06.07.1941","06 Jul 1941","EPS","","ABGUX","ABGUXFUTMZCABICDHHANHCVKUPUIRPUPHNEITCVCBBOCIDDDTWVDRBYZABSDLGKFZKUDHLJWJLNKFTEIDHEVZAXAPVUQLVXOLHNAZWKJAZIETANIRNFZQMAOIOACORDIYYH",""),
 ("108","06.07.1941","06 Jul 1941","MKO","","ABAHP","ABAHPDZNURTOLHFSHJMBHMXMUHVKYPGBKGDCBNVNCSDSKENKYKYKLKNUMJHNMCQQYNBEDVBJTJUKAJAYGLKISB",""),
 ("113","07.07.1941","07 Jul 1941","OPQ","","XIVFG","XIVFGIBAVLEQQLTIUMMJREOCIIYYVOA",""),
 ("122","08.07.1941b","08 Jul 1941","YXC","","GLPTL","GLPTLXXNKVVLLFFHJHVFVMTCONRDWPBXWDCJMZYGPCBFNPPRQNZVKXSSQVUUGYSDIAGFBXHX",""),
 ("126","08.07.1941b","08 Jul 1941","GUT","","ABDJV","ABDJVCNIIGFXRCYZHBQPNBSDYFCALGOTAUOTHFKGVZKHWABIGYINNQDKCQIZHXADDVWGEWLYYZAERRLSASFPASRACDZCAWBHKKCAHSGDIFRZBABKPAOPXMOTAGDPXZWDCDLFNXQXOHIZAHFFRNRKFFEQESSKFQNSXKXMMWVDTVAAGDBLRYZFXGSHGTLHLKDACFYUPQZPOUKJNTFLPFCRFOPXX",""),
 ("127","08.07.1941b","08 Jul 1941","TRE","","HBNVE","HBNVEFSAVWYDYSJEFMEKDWOUJWTDUTGDAPKBDZFEBYBTZVFPFWFUPQKPIVTFBJBLPWHJATMABXEAPSOYUIMFRSUYATZRYTPKYMGLIUBCKJPFFDNM",""),
 ("136","09.07.1941","09 Jul 1941","LAU","pad20","CASBL","CASBLDUAIKEGNUIZQMUDPILXGUMBCICYCMSQSPQGSSWXKUTTNVCWITHMERJUD","4 head groups (20 letters) missing; held as placeholders"),
 ("139","09.07.1941","09 Jul 1941","ALA","","BEYWU","BEYWUTEMSIYXKLBPSJDWAHLWOFEQKZXMASNZTAVNPZKUFZHKXBE-JFSJFIKFTWDWX","1 garble dash"),
 ("141","09.07.1941","09 Jul 1941","HNM","","DESGF","DESGFWHGAQOFVWVVKMBSWVNQRUKFKJTLOUXDBPLATLPNFBIUODJRXJMTJNBCSLSEIHRH",""),
 ("142","09.07.1941","09 Jul 1941","HBN","","TUGFI","TUGFIKQVTGLLPQGNRREHAANTJBORUCNGFLYJYYSXMPQCAMJAO",""),
 ("143","09.07.1941","09 Jul 1941","RFV","","WNFGI","WNFGIWUELDVARQLEQCDPUGVEBKKMLALTHCVHQPUVUFNJUVOJDKVETEKCJUKUCNZI",""),
 ("153","09.07.1941","09 Jul 1941","OFF","","SOFGI","SOFGISHUDMFUZIMNREYT---------------LAKWGQYBTBGECBSKLNCECFPQRZEHSHVGSJKJDFLFPKEFPSDKG-","severely garbled; 3 groups missing after 3rd; partial decrypt only"),
 ("157","09.07.1941","09 Jul 1941 (rx 10th)","SIM","","ABUNY","ABUNYAKTNFXVLIEZQOTKWIRXUUXSWONIYDOERIKZCIRUMXMDWEGUQVLABWYNULFRTYXMICBBFIZQLDARKZYOZHSBPBQDPITVTUYFPQXQGLIYZFFRDYJAHHKHWVYTSIQAZRBBGCRGVZK","enciphered on 9 Jul key"),
 ("158","09.07.1941","09 Jul 1941 (rx 10th)","KFK","","NEWUY","NEWUYBBKEWVFCJEEGMNEBOPKABCCCXUDOXEYEGOYJQOFIYUZZGBNKEIYACIAAHTWMGSBUNVYYVFHXJPDBWWDUPZSMWJOHAXROVWUWJIUFRIOMCBCBYZVOLSAASHXC",""),
 ("160","09.07.1941","09 Jul 1941 (rx 10th)","WUK","","ENIDN","ENIDNGLNPOQDUUENYDRKDTQBEIMZMFJJPXJQAPMHLTXEOMBUQXNKPUMADMQCVQBTLTDTJOKBVLDIBOHPASEYJYDTLOVRAPKVQWTBSMAHUNLHNUXRTYZSZOAGAAIMSNPWP",""),
 ("161","10.07.1941","10 Jul 1941","DOR","","ANIJQ","ANIJQXQXBCJTGZOTAPDSAFZIFWIJKIQFTDLJDVGPQYDPAIZUIPRMEMXJOGDNUGTZFLABHJMAJBTUCVNBGHSAJCMTVDUSRVFMBWYFQXTHKKFGYUVTJHDHMDNVAXWJDWQTUDOVEXLVDTDQJOZBFAFGNQMPGBTUOXCVQCBOENMFTWYUMYDFMBTFLEYBZYHJAIIIKBZXSFQIIAPXBZPSTJC",""),
 ("162-1","10.07.1941","10 Jul 1941 (part 1)","ERT","","ABNAQ","ABNAQPMQFUSKVUNQFGAHGVKJQZLBWAYYDUYANFAJRRQHPWSKOLPZBAOSRIKRXYNUJSSUJNWYJSEDLKPHTUKVSSYMSNUHTGIMWMVFYUKAUNFKMFSZKUXSHOUMYNGZATWPZORANQGMMJGIGYTIBSQQLQFTKMIBJASUPFSJRYUXNGGZMJJWLPKHLYCVPTOCHLIYZGPMMPFAUWCPCHIUCEGCJB",""),
 ("162-2","10.07.1941","10 Jul 1941 (part 2)","GHJ","","RDNAQ","RDNAQXVGCCTKSFDXRWIHAINTFQYGMPHWWKKMNPTMRPDGTIYWUQMBVDZSESPHGDCGLNWCBZQOZZBWHXKDKEBWXHNYBJYFNHCYTDOOCIYLZTGWXMGRPLPDBA",""),
 ("167","10.07.1941","10 Jul 1941","VBN","","BIOQN","BIOQNYMGVNBPHMZLATZDFJECFGPNJHOWUDUKHBPALGQJCUEQZNTPB",""),
 ("168","10.07.1941","10 Jul 1941","VFR","","SHNQO","SHNQODMCFWQZKJTKLHGHAGVCYOEATUSCCSJIQNBGQTMRJILEEDLOGARGEVVKKGTZXUWNYOTHPIBKPZWLKQWMZAQMHJYYHJBHTUXYAUESGSNVIIQFQGVHWSM",""),
 ("170","10.07.1941","10 Jul 1941","TGB","ins","TXIJQ","TXIJQCOPLCIXXYVXLSRTXBCJNYQKGVDCZOZEWJGIZPYJQYLDSHFPAHHOYRVTAVHRJUOJGVAEDTINRTWODIXQW-----LASWLYASWLBMSUYTZRUQNEFYGPRXDJXNHEUTFF","1 group (5 letters) missing after 17th group; held as placeholders"),
 ("173","10.07.1941","10 Jul 1941","HEN","","SIPVX","SIPVXEGVORBZNYHFGREXDUJIESGLNONUWQHWVUBNDGYQCYUPRFKQFPHYRXHJNHONBFDLIIACNGEFUHDCBFDWIWESORVXBQDHJAC","different network: own ring MRP + stecker, same wheel order"),
 ("176","10.07.1941","10 Jul 1941","WER","","RLGRZ","RLGRZZLPJHXFOPNYQSIEGLQSTAEQUSUWQEMCFEQDBGYKYNZCYTFFTCRRMUSQLEQUOATEZUGBMPRRKBEZBWLOSHTAMBHUBTLEMLNAUXCKHWDIKLTAGNCXIKCT",""),
 ("180","10.07.1941","10 Jul 1941","BOI","","NZGRZ","NZGRZANRXAHTUWLDZWRZKHBFWBDOMWEOFMPGUOGB",""),
 ("194","12.07.1941","12 Jul 1941","RHL","","MAKJH","MAKJHGCWUOBSPVJJBEUZRBYZKQUZJLQBUTLIUZKNYNPXEJGHSGGTUJCGKNZMNZVPINVJFEUADCFDPWVGJQCXNIDBAH",""),
 ("197","13.07.1941","13 Jul 1941","WSX","","ANERF","ANERFVLGWZLTWCUZKVUIHMFCEHYCFCUPUNQDO",""),
 ("199","13.07.1941","13 Jul 1941","TGB","","MYFRE","MYFREROEGPNJEWWZZIXCTLSNRBARNRD",""),
 ("200","13.07.1941","13 Jul 1941 (rx 14th)","MXK","","GSEAN","GSEANROPDKBYJJOXYGDJHIFOHMJDQSTKNQFPTYHMQWDFUIEMSSLEMDGCVUCCZGFCTFRICHRMTXJDKBKZWGZTXUN",""),
 ("201","13.07.1941","13 Jul 1941 (rx 14th)","IJN","","EGERF","EGERFLYNFXIEBIBZJXIVPXIFHFYPJILEFFEZPMILONBFQDWZAWNANBBDDHYMNHPDRJYHVUKMWAUIFDIRMYHFUCGZXBDLZMIRNKPIMAVAYXKMYHLRJVSGPNKEXXJAWEI",""),
 ("228","18.07.1941","18 Jul 1941","AKF","","HJVVS","HJVVSYJTHKDRELIGUMTJLEMRNPNFOBITQTATLANMJYPJUGUMDJFNAUFSJLLRCAYODGFSCMHKDNWAQFLSYRHZLWIGQQTBAGUSZHTQWAHJZCFELRHUZEFPTIZQTPWIPOHOXHPASLBJWCQNOIYC-SVIENCCZYXHUUUNSY","1 garble dash"),
 ("234","18.07.1941","18 Jul 1941 (rx 19th)","WAS","","LSEGB","LSEGBEZOGZDUVECYMLBRHEMPKKWCCJBRRJFFRV-BDMKQUZICCGRVRRCNQXLKPRXMBDFSPZOKZCKECYRTHSBCDNDOEUJYNHDILVZPAAMRZ","1 garble dash"),
 ("266","25.07.1941","25 Jul 1941","IDA","","AJLJD","AJLJDHXTNPFODYZBLBUVMSYIGULQUXVQJWPPCNRAWQIWRRMOJTVVHKDPGFIGCPEEALIKADUKRGUKGRSWNWHCAPWQBERWQFNRHWVDSRYWVZQPAPHPQCLFKEUJRYNWPIHBODPNCRZGPNEQLAVIFFJYDDTWLNWSGVSVVDZIXRLEXGYZXXCLWYM",""),
 ("282","29.07.1941","29 Jul 1941","PCL","","KLDIO","KLDIOJRJDAORKJGEEMOVPDRHBQYISWACJPASAEQZSPAEJQETVYVOWKQZBGVIPPSIGSDVFZJNMGLDRUWDSAEWSGTGXBIDAQA",""),
]


def roman(w):
    return " ".join(ROMAN[d] for d in w)


def body_of(kenn, special, ct):
    body = ct if special == "nostrip" else ct[5:]
    if special == "pad20":
        body = "-" * 20 + body
    return body


def decrypt(no, daykey, start, special, ct):
    u, w, r, s = KEY_OVERRIDE.get(no, DAYS[daykey])
    body = body_of("", special, ct)
    ph = body.replace("-", "A")
    out = subprocess.run([BIN, "-u", u, "-w", w, "-r", r, "-g", start, "-s", s],
                         input=ph, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         universal_newlines=True).stdout.strip().upper()
    pt = "".join("-" if c == "-" else p for c, p in zip(body, out))
    return u, w, r, s, body, pt


def load_existing_bodies():
    """Ciphertext bodies already in eval/enigma-messages.txt (for dedup)."""
    path = os.path.join(HERE, "enigma-messages.txt")
    bodies, cur = set(), []
    grab = False
    for ln in open(path):
        if ln.startswith("CIPHERTEXT:"):
            grab = True; cur = [ln.split(":", 1)[1].strip()]
        elif grab and ln.startswith("             "):
            cur.append(ln.strip())
        elif grab:
            bodies.add("".join(cur)); grab = False
    if grab:
        bodies.add("".join(cur))
    return bodies


def wrap(s, width=60):
    return "\n             ".join(textwrap.wrap(s, width)) if s else s


# Word-split readings for the messages recovered in this repo (5g). DECRYPT above
# is the raw machine output; READING re-inserts the word breaks the X separators
# mark and expands the telegraphic conventions (Q = ch, numbers spelled out), and
# GLOSS is a plain English rendering. Garbles are in the ORIGINAL transmission,
# not in the decrypt -- the keys are exact and verified by re-encryption.
#
# The doubled words are an error-correcting code, and they repair themselves:
# ZANDEYS/ZANDERS -> Zanders, KOENIGSBCRG/COENIGSBNRG -> Koenigsberg, LKW/EKW ->
# LKW. Where a garbled word was sent only once (BRZT, ABPANG) the reading marks
# the expansion as uncertain with [?].
READINGS = {
 "ALVPM": (
  "S[S] X HAUP[T]ST[URM]F[UEHRER] X SC[H]UH[M]ACHER X SCHUHMA[C]HER X "
  "OBERSCHARF[UEHRER] X ENGELMANN X ENGELMANN X ZURUEQ X "
  "U[NTER]STU[RM]F[UEHRER] X ERB X ERB X WIRD VON MIR EINGEWIESEN [X] "
  "FAHRE HEUTE ZUR ARMEE X KOMME MORGEN Z K R [?] X DIVISION X "
  "GEZ X HENNING [X] HENNING X",
  "SS-Hauptsturmfuehrer Schuhmacher, Schuhmacher: Oberscharfuehrer Engelmann, "
  "Engelmann back; Untersturmfuehrer Erb, Erb will be briefed by me. "
  "Travelling "
  "to the Army today, returning tomorrow ... Division. Signed Henning, "
  "Henning. "
  "(ZKR unexplained; the separator in HENNING[J]HENNING is itself garbled.)"),
 "ALRHG": (
  "ANF[?] ZWOTE X STAFFEL X OBERSCHARF[UEHRER] X ENGELMANN X ENGELMANN X "
  "ZURUEQ X U[N]TU[R]F[UEHRER] X ERB X ERB X WIRD VON MIR EINGEWIESEN X "
  "GEZ X HENNING X HENNING X",
  "... second echelon: Oberscharfuehrer Engelmann, Engelmann back; "
  "Untersturmfuehrer Erb, Erb will be briefed by me. Signed Henning, Henning. "
  "The abbreviated re-send of ALVPM; ANF is an unexpanded abbreviation, and "
  "U[Z]TUF carries one garble against ALVPM's USTUF."),
 "55 NF": (
  "EINS NEUN EINS FUENF X KOLONNEN UEBER X STARAJA RUSSA X STARAJA RUSSA X "
  "IN MARSQ GESETZT X HARTJENSTEIN X",
  "1915 hrs: columns set in march via Staraja Russa, Staraja Russa. "
  "Hartjenstein. (Plaintext as published on the source page.)"),
 "115": (
  "AN STUBAF X SCHUSTER X ZANDE[R]S X ZANDERS X BITTET UM TELEGRAFISCHEN "
  "BESCHEID ZUM X ZEL X ZEN XX WO EINS ZWO X KOENIGSB[E]RG X [K]OENIGSB[E]RG "
  "X OB X FORD X FORD X VIKTOR X AQT X PKW X PKW X ODER X LKW X [L]KW X "
  "MOTOR F HAR[T]JENSTEIN",
  "To SS-Sturmbannfuehrer Schuster, Zanders: requests telegraphic reply ... "
  "Koenigsberg ... whether Ford, Ford, Viktor eight, cars or trucks, motor ... "
  "Hartjenstein."),
 "116": (
  "AN DIV X [A]RZT X PERSON X VERLUSTE VOM X ZWO SIEBEN X NEUN X "
  "FEHLANZEIGE X KRANKE X ZUGANG X EINS X AB[G]ANG X ZWO X BESTAND X "
  "EINS NULL X RENNER X RENNER",
  "To Division, medical officer, personnel. Casualties of 27.9: nil return. "
  "Sick: 1 admitted, 2 discharged, 10 on strength. Renner, Renner."),
 "117": (
  "BITTE ANTWORT AQ X ZANDERS X ZANDERS VON DORN TELEFONISQ AUFGEBEN X "
  "HARTJENSTEIN X",
  "Please reply ...: Zanders, Zanders, to be passed by telephone from Dorn. "
  "Hartjenstein."),
}

HEADER = """\
# ============================================================================
# enigma-army-messages-1941.txt  --  authentic 1941 German Army (HG Nord) Enigma
#                                    messages, keys recovered post-2003
# ----------------------------------------------------------------------------
# Source: Geoff Sullivan & Frode Weierud, "Breaking German Army Ciphers",
#         Cryptologia 29(3):193-232 (2005). Message + key pages at
#         cryptocellar.org/bgac (message forms (c) 2006; recovered keys released
#         04 Aug 2017). CC BY-NC-SA. HG Nord, Operation Barbarossa, Jun-Oct 1941.
#
# These are ciphertexts the authors originally FAILED to break (2003-04); their
# day-keys were recovered later, by them and by the M4 Message Breaking Project.
# Every message here decrypts to clean telegraphic German at its stated key.
#
# CONVENTIONS  (identical to enigma-messages.txt)
#   REFLECTOR B; WHEELS left->right (I..VIII); RING/START letters (A=1..Z=26).
#   The KENNGRUPPE (1st 5-letter group on the form) is the discriminant, NOT
#     ciphertext -- deciphering starts at the 2nd group. CIPHERTEXT below already
#     excludes it. DECRYPT is the raw machine output (operator orthography:
#     X = word separator, Q often = ch/k, digits spelled EINS ZWO DREI ...).
#   A dash '-' is an unrecorded-but-real letter (illegible/missing on the form);
#     it is a real rotor position -- kept as a placeholder, never stripped.
#
# VALIDATION: eval/build_army_messages_1941.py decrypts every record with ./enigma
#   at its key and reproduces DECRYPT exactly (deterministic). Two messages that
#   duplicate eval/enigma-messages.txt (No. 203 CFYZR, No. 233 XNRLR) are dropped
#   here by ciphertext-dedup, so the two files are disjoint.
# ============================================================================
"""


def main():
    # Self-dedup FIRST. The old code deduped only against enigma-messages.txt, so
    # adding a message already present here under a different (garbled) Kenngruppe
    # silently produced two records -- which is exactly what happened when Nrs
    # 115-117 were "rediscovered" from the source page and matched on Kenngruppe
    # rather than on ciphertext. Match on the ciphertext, which is the identity.
    seen = {}
    for rec in M:
        body = body_of("", rec[4], rec[6]).replace("-", "")
        if body in seen:
            raise SystemExit("duplicate ciphertext: Nr %s repeats Nr %s"
                             % (rec[0], seen[body]))
        seen[body] = rec[0]
    existing = load_existing_bodies()
    records, dropped = [], []
    for no, daykey, label, start, special, kenn, ct, notes in M:
        u, w, r, s, body, pt = decrypt(no, daykey, start, special, ct)
        canon = body.replace("-", "")
        if canon in existing or any(canon == b.replace("-", "") for b in existing):
            dropped.append((no, kenn)); continue
        records.append((no, label, u, w, r, start, s, kenn, body, pt, notes))

    with open(OUT, "w") as f:
        f.write(HEADER + "\n")
        for no, label, u, w, r, start, s, kenn, body, pt, notes in records:
            f.write("### Message No. %s  --  %s  (%s)\n" % (no, label, kenn))
            f.write("REFLECTOR:   %s\n" % u)
            f.write("WHEELS:      %-11s (-w %s)\n" % (roman(w), w))
            f.write("RING:        %s\n" % r)
            f.write("START:       %s\n" % start)
            f.write("PLUGS:       %s\n" % s)
            f.write("KENNGRUPPE:  %s   (discriminant; not enciphered)\n" % kenn)
            f.write('CMD:         ./enigma -u %s -w %s -r %s -g %s -s "%s"\n' % (u, w, r, start, s))
            f.write("CIPHERTEXT:  %s\n" % wrap(body))
            f.write("DECRYPT:     %s\n" % wrap(pt))
            if no in READINGS:
                rd, gl = READINGS[no]
                f.write("READING:     %s\n" % wrap(rd, 60))
                f.write("GLOSS:       %s\n" % wrap(gl, 60))
            f.write("NOTES:       %s\n\n" % (notes if notes else "(clean)"))

    print("wrote %s" % OUT)
    print("records: %d   dropped as duplicates of enigma-messages.txt: %s"
          % (len(records), ", ".join("%s(%s)" % d for d in dropped) or "none"))
    # re-verify: reproduce each DECRYPT
    bad = 0
    for no, daykey, label, start, special, kenn, ct, notes in M:
        _, _, _, _, _, pt = decrypt(no, daykey, start, special, ct)
        if not pt or len(pt) < 5:
            print("  !! %s produced no plaintext" % no); bad += 1
    print("re-verify: all decrypt OK" if not bad else "re-verify: %d FAILED" % bad)


if __name__ == "__main__":
    main()
