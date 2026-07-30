#!/usr/bin/env python3
"""Check or reflow Markdown to the repo's 80-column rule (CLAUDE.md, "Line
width: 80 columns"), without disturbing document structure.

    python3 tests/reflow_md.py --check FILE...   # report, exit 1 if any violate
    python3 tests/reflow_md.py --write FILE...   # rewrap in place

One of --check/--write is required, so the tool can never edit a file by
accident.

WHAT IT LEAVES ALONE
    Fenced code blocks, headings, table rows and blank lines pass through
    untouched. Their content often cannot wrap, and the rule exempts it.

WHY NOT A ONE-LINE `textwrap` CALL
    Reflowing prose looks trivial and is not. Both of these were hit while
    reflowing this repo's own documentation, and both corrupt a file in ways
    that read as plausible:

    * A paragraph-oriented reflow MERGES CONSECUTIVE LIST ITEMS. A run of
      bullets looks like one paragraph unless each item is treated as its own
      block, and the result is a run-on blob that still renders.
    * Rewrapping can move a token to the START of a line where Markdown reads
      it as a block marker rather than as prose. CLAUDE.md says
      "failing if any benchmark is >`THRESHOLD`% ..." mid-sentence; wrap that
      `>` to line-start and the rest of the paragraph silently becomes a
      blockquote. `-`, `#`, `|` and `1.` have the same hazard.

    So: list items are blocks, and a line is never started with a token that
    Markdown would read as a marker (it breaks one word earlier instead).

SAFETY CHECKS (run on every --write)
    1. Token identity — the word sequence must be unchanged; only line breaks
       may move. Blockquote markers are excluded from the comparison because
       rewrapping a quoted paragraph legitimately changes how many there are.
    2. Blockquote blocks — the number of quoted regions must be unchanged, so
       quoting cannot be silently dropped by check 1's exclusion.
    Either failing aborts without writing.

Width is DISPLAY COLUMNS, not bytes: the docs use `—`, `×`, `≈` and `§`
constantly, three bytes and one column each, so a byte-based check over-reports.
"""
import re
import sys
import unicodedata

WIDTH = 80
QUOTE = re.compile(r'^(\s*(?:>\s?)+)')
ITEM = re.compile(r'^(\s*)([-*]\s+|\d+\.\s+)')
DANGER = re.compile(r'^(?:>|#|\||```|~~~)|^[-*+]$|^\d+[.)]$')


def cols(text):
    """Display width: East-Asian wide/fullwidth count 2, everything else 1."""
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1
               for c in text)


def split_quote(line):
    m = QUOTE.match(line)
    return (m.group(1), line[m.end():]) if m else ('', line)


def passthrough(line):
    body = split_quote(line)[1]
    return (not line.strip()) or body.startswith('#') \
        or body.lstrip().startswith('|')


def reflow(lines, width=WIDTH):
    out, i, fence = [], 0, False
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith('```'):
            fence = not fence
            out.append(line)
            i += 1
            continue
        if fence or passthrough(line):
            out.append(line)
            i += 1
            continue

        quote, body = split_quote(line)
        item = ITEM.match(body)
        if item:                       # list item: marker, then hanging indent
            first = quote + item.group(1) + item.group(2)
            cont = quote + item.group(1) + ' ' * len(item.group(2))
            text = [body[item.end():]]
        else:                          # plain paragraph
            first = cont = quote + re.match(r'^(\s*)', body).group(1)
            text = [body.strip()]

        j = i + 1                      # gather this block's continuation lines
        while j < len(lines):
            nxt = lines[j]
            if nxt.lstrip().startswith('```') or passthrough(nxt):
                break
            nquote, nbody = split_quote(nxt)
            if nquote.strip() != quote.strip():     # quote depth changed
                break
            if ITEM.match(nbody):                   # a new item ends the block
                break
            text.append(nbody.strip())
            j += 1

        words = ' '.join(text).split()
        if not words:
            out.append(line)
            i = j
            continue

        res, cur, pre = [], first, first
        for word in words:
            cand = cur + ('' if cur == pre else ' ') + word
            if cols(cand) > width and cur.strip() != pre.strip():
                carry = []
                if DANGER.match(word):
                    # never start a line with a Markdown block marker
                    parts = cur.split()
                    if len(parts) > 1:
                        carry = [parts[-1]]
                        cur = cur[:cur.rstrip().rfind(parts[-1])]
                res.append(cur.rstrip())
                pre = cont
                cur = cont + ' '.join(carry + [word])
            elif cur == pre:
                cur = cur + word
            else:
                cur = cand
        res.append(cur.rstrip())
        out.extend(res)
        i = j
    return out


def content(lines):
    """Word sequence, ignoring blockquote markers and all whitespace."""
    return ' '.join(' '.join(split_quote(l)[1] for l in lines).split())


def quote_blocks(lines):
    """Number of quoted regions (a quoted line following an unquoted one)."""
    n, prev = 0, False
    for line in lines:
        quoted = bool(split_quote(line)[0].strip())
        n += quoted and not prev
        prev = quoted
    return n


def unavoidable(line):
    """True when no rewrap could fit this line: some single token is too long
    on its own. The rule exempts unbreakable text -- long URLs, file paths --
    and splitting such a token would corrupt it, so this is not a violation."""
    words = split_quote(line)[1].split()
    if not words:
        return False
    indent = cols(line) - cols(line.lstrip())
    return indent + max(cols(w) for w in words) > WIDTH


def violations(lines):
    """(line number, width) for every line over the limit. Fenced blocks and
    lines held over by an unbreakable token are exempt."""
    bad, fence = [], False
    for n, line in enumerate(lines, 1):
        if line.lstrip().startswith('```'):
            fence = not fence
            continue
        if not fence and cols(line) > WIDTH and not unavoidable(line):
            bad.append((n, cols(line)))
    return bad


def main(argv):
    mode = argv[1] if len(argv) > 1 else ''
    paths = argv[2:]
    if mode not in ('--check', '--write') or not paths:
        print(__doc__.strip().split('\n\n')[1])
        return 2

    failed = False
    for path in paths:
        src = open(path, encoding='utf-8').read().split('\n')
        if mode == '--check':
            bad = violations(src)
            if bad:
                failed = True
                print('%s: %d line(s) over %d columns' %
                      (path, len(bad), WIDTH))
                for n, w in bad[:20]:
                    print('    line %-5d %3d columns' % (n, w))
            else:
                print('%s: ok' % path)
            continue

        dst = reflow(src)
        if content(src) != content(dst):
            a, b = content(src).split(), content(dst).split()
            for x, y in zip(a, b):
                if x != y:
                    print('%s: TOKEN MISMATCH %r vs %r' % (path, x, y))
                    break
            print('%s: ABORTED, content changed' % path)
            failed = True
            continue
        if quote_blocks(src) != quote_blocks(dst):
            print('%s: ABORTED, blockquote blocks %d -> %d' %
                  (path, quote_blocks(src), quote_blocks(dst)))
            failed = True
            continue
        open(path, 'w', encoding='utf-8').write('\n'.join(dst))
        left = len(violations(dst))
        print('%s: %d -> %d lines, %d over %d columns (fenced blocks exempt)' %
              (path, len(src), len(dst), left, WIDTH))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
