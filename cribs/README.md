# cribs — known-word lists for the `--crib-file` finisher

A **crib** file is a list of known words (one per line, optional weight after it; `#`
comments) used by the opt-in `--crib-file` finisher. After each restart plugboard climb
converges, its board is ranked by `n-gram score + --crib-weight × crib_score`, where
`crib_score` sums the weights of listed words that appear as substrings of the decrypt
(telegraphic traffic concatenates words within a clause, so substring — not token — matching
is used). The intent is Ostwald & Weierud's "assessment stage": lift the true board above a
spurious one that scores higher on n-grams but contains no real words.

```sh
./enigma -c -a -l german -d ngrams-telegraphic --crib-file cribs/german-hgnord.txt \
         -S m4a10 -J --gainfix-best3 -R 200 -T4 -u B -w 421 -r YHO -g WAS < cipher.txt
```

## `german-hgnord.txt`

Generic HG Nord / Wehrmacht telegraphic vocabulary — spelled-out numbers, the phonetic
alphabet, and standard military nouns/verbs. Chosen from general telegraphy conventions and
the words the article names (Berta, Eins, Frage, Roem); **not** fitted to any particular
message. Longer/rarer words carry more weight, so genuine multi-word signal on the true
board outweighs coincidental short-word matches on garbage.

## Status: NOT RECOMMENDED (measured-down)

On the 69-message held-out set, over the telegraphic tables, the crib finisher is net
**−0.1 pp** at weight 0.5 (−1.7 at 1.0) — see `eval/eval_crib.py` and
`eval/MODERN_BREAKING_NOTES.md` §7. It scores the odd genuine scoring-failure win
(No. 203: 79→100 %) but false-positive re-ranking offsets it. Once the telegraphic tables
surface the true board, the residual is dominated by **wrong-basin** failures (the truth
isn't among the converged restarts), so a re-ranker has nothing true to promote. Kept as an
off-by-default diagnostic — the negative result is the artifact.
