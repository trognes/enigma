#!/usr/bin/env python3
#
# Generates the plain A-Z corpus files in this directory from human-readable
# source passages (kept here for provenance). Run:  python3 build_corpora.py
#
# Why authored passages and not fetched literature: the session's egress proxy
# denies general web hosts (e.g. www.gutenberg.org -> 403), so external corpora
# could not be downloaded. These are original, idiomatic passages on varied
# topics, written to diversify the excerpt vocabulary/letter-distribution beyond
# the single built-in passage. To add REAL corpora, just drop more
# `<language>_<name>.txt` files (pure text is fine -- the loader keeps only A-Z,
# uppercased) into this directory; eval.py picks them up automatically.
#
# German umlauts/eszett are transliterated ae/oe/ue/ss to match the project's
# existing convention (the built-in german passage spells SCHLUESSEL etc.).

import os
import re

# The two built-in passages are emitted as *_builtin.txt so every corpus lives
# as a file and existing results rows (all drawn from these) remain substring-
# verifiable. Copied verbatim from the CORPORA dicts.
BUILTIN = {
    "english_builtin": "THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES",
    "german_builtin": "DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEILDIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBENFEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHLUESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENKRIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN",
    # danish/french "builtin" = the passages eval.py used as the fallback corpus,
    # so the corpus name stays "builtin" and matches the earlier rows' content.
    "danish_builtin": "DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENESVERDENOGSEDENSTORESKIBEOGBYERNEOGSKOVENEHVERTAARBLEVHUNAELDREOGFIKLOVTILATSTIGEOPGENNEMDETKLAREVANDFORATSIDDEPAAKLIPPERNEISKINNETFRAMAANENOGSEUDOVERDENSTOREVIDEVERDENOGNAARSOLENGIKNEDDYKKEDEHUNNEDIGENMENHUNGLEMTEALDRIGDENDEJLIGEVERDENOVENOVERVANDETOGENDAGDAHUNREDDEDEENUNGPRINSFRADRUKNINGFORELSKEDEHUNSIGHAABLOEST",
    "french_builtin": "LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAISAUVENTMAUVAISQUIMEMPORTEDECADELABCOMMELAFEUILLEMORTEPENDANTLONGTEMPSJEMESUISCOUCHEDEBONNEHEUREETJAIREVEDESPAYSLOINTAINSOULESHOMMESSONTLIBRESETOULAVIEESTDOUCEETBELLECHAQUEMATINJEMEPROMENAISLELONGDELARIVIEREENECOUTANTLECHANTDESOISEAUXETLEMURMUREDELEAUQUICOULAITDOUCEMENTVERSLAMER",
}

# Genuine historical Enigma message plaintexts (already A-Z after cleaning;
# umlauts/eszett transliterated ae/oe/ue/ss, X/Y/K/J procedure separators kept as
# they are part of the operational telegraphic style). Provenance in comments.
# These test whether real telegraphic German (unlike prose) scores under the
# prose-trained n-gram tables.
GENUINE = {
    # Grossadmiral Doenitz message P1030681, May 1945 (Hitler naming Doenitz his
    # successor) -- a genuine WW2 Kriegsmarine M4 message. Plaintext as commonly
    # published (cryptomuseum.com/crypto/enigma/msg/p1030681.htm; the M4/U-534
    # material at enigma.hoerenberg.com). Retrieved via web search; treat as
    # genuine operational German, not a byte-verified transcription.
    "german_doenitz1945": "KRKRALLEXXFOLGENDESISTSOFORTBEKANNTZUGEBENXXICHHABEFOLGENDENBEFEHLERHALTENXXJANSTELLEDESBISHERIGENREICHSMARSCHALLSJGOERINGJSETZTDERFUEHRERSIEYHERRGROSSADMIRALYALSSEINENNACHFOLGEREINXSCHRIFTLSCHEVOLLMACHTUNTERWEGSXABSOFORTSOLLENSIESAEMTLICHEMASSNAHMENVERFUEGENYDIESICHAUSDERGEGENWAERTIGENLAGEERGEBENXGEZXREICHSLEITERKKTULPEKKJBORMANNJXXOBXDXMMMDURCHFKSTXKOMXADMXUUUBOOTEXKP",
    # The 1930 Enigma instruction-manual test message (Reichswehr; pre-war but the
    # canonical genuine Enigma message). Plaintext as enciphered (Q for CH, X word
    # separators). Documented at cryptocellar.org, Wikipedia ("grill" method),
    # wiki.franklinheath.co.uk. Short (~90 chars) -- usable only at short lengths.
    "german_manual1930": "FEINDLIQEINFANTERIEKOLONNEBEOBAQTETXANFANGSUEDAUSGANGBAERWALDEXENDEDREIKMOSTWAERTSNEUSTADT",
}

# Human-readable source passages (umlauts allowed; cleaned on write).
READABLE = {
    "english_ocean": """
The ocean covers more than seventy percent of the surface of our planet and
remains one of the least explored places on earth. Beneath the waves lies a
world of strange and beautiful creatures, from tiny glowing plankton to enormous
whales that sing to one another across great distances. Coral reefs shelter
countless fish in warm shallow water, while in the cold darkness of the deep sea
animals have learned to make their own light. Powerful currents carry warm water
from the equator toward the poles and shape the weather of every continent.
Sailors have crossed these waters for thousands of years, guided by the stars,
and even today much of what lies below the surface remains a mystery waiting to
be discovered.
""",
    "english_mountains": """
High in the mountains the air grows thin and cold, and only the hardiest plants
and animals can survive. Great peaks of rock and ice rise toward the sky, carved
over millions of years by wind, water, and slow moving rivers of ice called
glaciers. In the valleys below, rivers begin as small streams fed by melting
snow and gather strength as they rush toward the distant sea. Climbers travel
from far away to test themselves against the steep slopes, carrying ropes and
tents and enough food for many days. When the sun sets behind the ridges, the
snow turns pink and gold, and the first stars appear in a sky so clear that it
seems close enough to touch.
""",
    "english_city": """
A great city never truly sleeps, for even in the darkest hours of the night
there are people at work keeping it alive. Tall buildings of glass and steel
stand where fields and farms once lay, and beneath the crowded streets run
tunnels carrying trains full of tired travelers. Markets sell food from every
corner of the world, and on busy corners musicians play for coins while artists
paint the passing crowd. Over the years the city has grown outward and upward,
swallowing the villages around it and drawing new people who come in search of
work and a better life. Yet hidden among the modern towers stand old churches
and narrow lanes that remember a quieter time long before the noise of engines
filled the air.
""",
    "german_wald": """
Der deutsche Wald ist seit vielen Jahrhunderten ein wichtiger Teil der
Landschaft und der Kultur. In den dichten Wäldern wachsen hohe Buchen und
Eichen, unter denen sich Rehe, Wildschweine und viele Vögel verstecken. Im
Frühling bedecken grüne Blätter die Bäume, und im Herbst färben sich die Wälder
rot und golden. Viele Menschen gehen am Wochenende in den Wald, um zu wandern,
frische Luft zu atmen und die Ruhe der Natur zu genießen. Förster kümmern sich
um die Bäume und sorgen dafür, dass der Wald gesund bleibt und nachwächst. Auch
für das Klima ist der Wald sehr wichtig, weil die Bäume die Luft reinigen und
den Boden vor starkem Regen schützen.
""",
    "german_reise": """
Eine lange Reise durch ein fremdes Land ist immer ein Abenteuer, das man nie
vergisst. Am frühen Morgen fährt der Zug aus dem Bahnhof, und bald ziehen
Felder, Flüsse und kleine Dörfer am Fenster vorbei. In den großen Städten
steigen viele Menschen ein und aus, und überall hört man Sprachen, die man nicht
versteht. Wer mit offenen Augen reist, entdeckt an jedem Ort etwas Neues: alte
Gebäude, bunte Märkte und freundliche Menschen, die gern von ihrer Heimat
erzählen. Am Abend sucht der müde Reisende ein einfaches Zimmer, schreibt seine
Erlebnisse in ein Heft und freut sich schon auf den nächsten Tag. So wird aus
vielen kleinen Eindrücken die Erinnerung an eine unvergessliche Reise.
""",
    "german_wissenschaft": """
Die Wissenschaft versucht, die Welt um uns herum zu verstehen und ihre
Geheimnisse zu erklären. Forscher stellen Fragen, führen sorgfältige Versuche
durch und schreiben ihre Ergebnisse auf, damit andere sie prüfen können. Vor
vielen hundert Jahren glaubten die Menschen, dass die Sonne sich um die Erde
dreht, doch mutige Denker zeigten mit Hilfe von Beobachtung und Rechnung, dass
es genau umgekehrt ist. Seitdem hat das Wissen der Menschheit ständig
zugenommen, von den Gesetzen der Bewegung bis zu den winzigen Bausteinen der
Materie. Jede neue Antwort führt zu weiteren Fragen, und gerade darin liegt der
Reiz der Forschung, denn das Streben nach Wahrheit findet niemals ein Ende.
""",
    "danish_danmark": """
Danmark er et lille land i Nordeuropa som består af en stor halvø og mange små
øer. Landet er kendt for sine flade marker, sine lange strande og sit milde
klima. Om sommeren tager mange mennesker til stranden for at bade i det kølige
vand og nyde solen. Langs kysten ligger små fiskerbyer hvor både sejler ud hver
morgen for at fange fisk. I skovene lever rådyr og harer, og om foråret synger
fuglene i de grønne træer. Danskerne cykler meget, både i byerne og på landet,
fordi landet er fladt og let at komme rundt i. Selv om vintrene kan være kolde
og mørke, holder folk humøret oppe med hygge, levende lys og varm kaffe sammen
med gode venner og nær familie.
""",
    "danish_hav": """
Havet dækker størstedelen af vores planet og er stadig et af de mindst
udforskede steder på jorden. Under bølgerne lever mærkelige og smukke skabninger,
fra det lille lysende plankton til de store hvaler, der synger til hinanden over
lange afstande. Koralrev giver ly til utallige fisk i det varme lave vand, mens
dyrene i det kolde mørke dyb har lært at lave deres eget lys. Stærke havstrømme
fører varmt vand fra ækvator mod polerne og former vejret på hvert kontinent.
Søfolk har krydset disse vande i tusinder af år, styret efter stjernerne, og
meget af det, der ligger under overfladen, er stadig en gåde, som venter på at
blive opdaget.
""",
    "danish_by": """
En stor by sover aldrig helt, for selv i nattens mørkeste timer er der mennesker
på arbejde for at holde den i live. Høje huse af glas og stål rejser sig, hvor
der før lå marker, og under de travle gader kører tog fulde af trætte rejsende.
På torvene sælges mad fra alle verdens hjørner, og på hjørnerne spiller musikere
for mønter, mens kunstnere maler den forbipasserende folkemængde. Gennem årene er
byen vokset udad og opad, slugt de nærliggende landsbyer og tiltrukket nye
mennesker, der søger arbejde og et bedre liv. Alligevel står der gemt mellem de
moderne tårne gamle kirker og smalle stræder, som husker en mere stille tid.
""",
    "danish_skov": """
Skoven har i mange hundrede år været en vigtig del af det danske landskab. I de
tætte skove vokser høje bøge og ege, hvorunder rådyr, harer og mange fugle gemmer
sig. Om foråret dækker grønne blade træerne, og om efteråret farves skovene røde
og gyldne. Mange mennesker går i skoven i weekenden for at vandre, trække frisk
luft og nyde naturens ro. Skovfogeder passer træerne og sørger for, at skoven
forbliver sund og vokser op igen. Skoven er også vigtig for klimaet, fordi
træerne renser luften og beskytter jorden mod kraftig regn.
""",
    "french_mer": """
La mer recouvre la plus grande partie de notre planète et reste un monde
mystérieux et magnifique. Sous les vagues vivent des créatures étranges, depuis
le minuscule plancton qui brille dans la nuit jusqu'aux baleines immenses qui
chantent sur de longues distances. Les récifs de corail abritent d'innombrables
poissons dans les eaux chaudes et peu profondes, tandis que dans l'obscurité des
grands fonds certains animaux produisent leur propre lumière. De puissants
courants transportent l'eau chaude de l'équateur vers les pôles et façonnent le
climat de chaque continent. Les marins traversent ces eaux depuis des milliers
d'années, guidés par les étoiles, et une grande partie de ce qui se cache sous la
surface demeure encore inconnue.
""",
    "french_ville": """
Une grande ville ne dort jamais vraiment, car même aux heures les plus sombres de
la nuit, des gens travaillent pour la maintenir en vie. De hautes tours de verre
et d'acier s'élèvent là où s'étendaient autrefois des champs, et sous les rues
animées circulent des trains remplis de voyageurs fatigués. Les marchés vendent
des aliments venus de tous les coins du monde, et à chaque carrefour des
musiciens jouent tandis que des artistes peignent la foule qui passe. Au fil des
années, la ville a grandi vers le ciel, avalant les villages voisins et attirant
de nouveaux habitants venus chercher du travail et une vie meilleure. Pourtant,
cachées parmi les immeubles modernes, de vieilles églises et d'étroites ruelles
se souviennent d'une époque plus tranquille.
""",
    "french_montagne": """
Haut dans les montagnes, l'air devient froid et rare, et seules les plantes et
les bêtes les plus robustes peuvent survivre. De grands sommets de roche et de
glace s'élèvent vers le ciel, sculptés pendant des millions d'années par le vent,
l'eau et les lents fleuves de glace que l'on appelle glaciers. Dans les vallées
en contrebas, les rivières naissent de petits ruisseaux nourris par la neige
fondante et prennent de la force en descendant vers la mer lointaine. Des
grimpeurs viennent de loin pour se mesurer aux pentes escarpées, portant des
cordes, des tentes et assez de vivres pour de nombreux jours. Quand le soleil se
couche derrière les crêtes, la neige devient rose et dorée, et les premières
étoiles apparaissent dans un ciel si clair qu'il semble tout proche.
""",
}

# Two transliterations of the German umlauts/eszett:
#  * MULTI  (historical Enigma convention): ae/oe/ue/ss  -> *.txt
#  * SINGLE (matches the tool's accent folding: ae-umlaut -> A): a/o/u/s -> *_sl.txt
# The tool now folds accented n-grams to a single base letter, so the SINGLE
# corpora are the convention-matched ones; keeping both lets the eval compare
# which transliteration the folded tables actually prefer (EVAL_CORPORA=...).
_FRENCH = {   # single-char base folds; no ü (German's ue/u mapping wins that)
    "à": "a", "â": "a", "ç": "c", "è": "e", "é": "e", "ê": "e", "ë": "e",
    "î": "i", "ï": "i", "ô": "o", "ù": "u", "û": "u", "ÿ": "y",
    "À": "a", "Â": "a", "Ç": "c", "È": "e", "É": "e", "Ê": "e", "Ë": "e",
    "Î": "i", "Ï": "i", "Ô": "o", "Ù": "u", "Û": "u", "Ÿ": "y",
}
TRANS = str.maketrans({
    "ä": "ae", "ö": "oe", "ü": "ue", "Ä": "ae", "Ö": "oe", "Ü": "ue", "ß": "ss",
    "å": "aa", "ø": "oe", "æ": "ae", "Å": "aa", "Ø": "oe", "Æ": "ae",
    "œ": "oe", "Œ": "oe", **_FRENCH,
})
TRANS_SINGLE = str.maketrans({
    "ä": "a", "ö": "o", "ü": "u", "Ä": "a", "Ö": "o", "Ü": "u", "ß": "s",
    "å": "a", "ø": "o", "æ": "a", "Å": "a", "Ø": "o", "Æ": "a",
    "œ": "o", "Œ": "o", **_FRENCH,
})


def clean(text, trans=TRANS):
    return re.sub(r"[^A-Z]", "", text.translate(trans).upper())


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, text in (list(BUILTIN.items()) + list(GENUINE.items())
                       + list(READABLE.items())):
        multi = clean(text)
        with open(os.path.join(here, name + ".txt"), "w") as fh:
            fh.write(multi + "\n")
        # Emit a single-letter variant only when it actually differs (accented
        # source), i.e. the German prose passages, as <name>_sl.txt.
        single = clean(text, TRANS_SINGLE)
        if single != multi:
            with open(os.path.join(here, name + "_sl.txt"), "w") as fh:
                fh.write(single + "\n")
            print("%-26s %5d chars  (+ _sl variant)" % (name + ".txt", len(multi)))
            continue
        print("%-24s %5d chars" % (name + ".txt", len(multi)))


if __name__ == "__main__":
    main()
