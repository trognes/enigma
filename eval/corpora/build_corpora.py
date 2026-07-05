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
}

TRANS = str.maketrans({
    "ä": "ae", "ö": "oe", "ü": "ue", "Ä": "ae", "Ö": "oe", "Ü": "ue", "ß": "ss",
})


def clean(text):
    return re.sub(r"[^A-Z]", "", text.translate(TRANS).upper())


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, text in list(BUILTIN.items()) + list(READABLE.items()):
        cleaned = clean(text)
        with open(os.path.join(here, name + ".txt"), "w") as fh:
            fh.write(cleaned + "\n")
        print("%-24s %5d chars" % (name + ".txt", len(cleaned)))


if __name__ == "__main__":
    main()
