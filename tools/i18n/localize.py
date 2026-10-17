#!/usr/bin/env python3
"""Re-apply the Chinese localization layer after an upstream sync.

The only hand-maintained artifact is translations.tsv (key <TAB> en <TAB> zh,
# lines are comments; one key may carry several English variants on separate
rows - the strings.xml generation dedupes by key). Everything else is derived:

  - app/src/main/res/values/strings.xml      regenerated (default English)
  - app/src/main/res/values-zh/strings.xml   regenerated (Chinese)
  - layout/menu XML: android:text/title/contentDescription values found in
    the map are swapped to @string/<key>
  - Java UI files (JAVA_FILES): exact quoted literals from the map are
    swapped to getString(R.string.<key>); plain views get
    getContext().getString(...). The setSeg2(..., verified) grey-out trigger
    gets its 4th boolean argument re-added (the old form keyed off the
    literal "Verified", which a swap would break).

Strings the report lists under "untranslated" are upstream strings the map
does not know yet: add a row to translations.tsv, re-run, rebuild.
Lines skipped as "manual" hold a mapped literal in a String field
initializer - moving those to a resource reference is a code change, not a
swap, and stays with the fork's own history.

Usage:
  python3 tools/i18n/localize.py            apply and print the report
  python3 tools/i18n/localize.py --check    verify the tree is up to date
                                            (exit 1 if anything is stale)
"""

import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent.parent
TSV = TOOLS / "translations.tsv"

XML_DIRS = ["app/src/main/res/layout", "app/src/main/res/menu"]
XML_ATTR_RE = re.compile(r'(android:(?:text|title|contentDescription)=")([^"]*)(")')

# Java files holding UI strings -> the getString() receiver each one needs.
JAVA_FILES = {
    "app/src/main/java/df/root/MainActivity.java": "getString",
    "app/src/main/java/df/root/TwoStepProgressView.java": "getContext().getString",
}

# A String field initializer cannot take getString() (static final has no
# context; plain views need it after construction) - skipped and reported.
DECL_RE = re.compile(r'^\s*(?:private\s+|public\s+|protected\s+)?'
                     r'(?:static\s+)?(?:final\s+)?String\s+\w+\s*=')

# Proper nouns / HTTP headers that look like UI strings but are not.
SKIP_LITERALS = {"DirtyFrag", "DFRoot", "Accept", "User-Agent"}


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace("'", "\\'").replace('"', "&quot;"))


def load_map():
    en_to_key, en_of_key, zh_of_key, order = {}, {}, {}, []
    for line in TSV.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, en, zh = line.split("\t")
        if key not in en_of_key:
            order.append(key)
            en_of_key[key] = en
            zh_of_key[key] = zh
        en_to_key[en] = key
    return en_to_key, en_of_key, zh_of_key, order


def gen_strings(order, values):
    out = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>"]
    for key in order:
        out.append('    <string name="%s">%s</string>' % (key, esc(values[key])))
    out.append("</resources>")
    return "\n".join(out) + "\n"


def transform_xml(text, en_to_key):
    swaps, candidates = 0, []

    def sub(m):
        nonlocal swaps
        value = m.group(2)
        if value in en_to_key:
            swaps += 1
            return m.group(1) + "@string/" + en_to_key[value] + m.group(3)
        if value and not value.startswith("@") and value not in candidates:
            candidates.append(value)
        return m.group(0)

    return XML_ATTR_RE.sub(sub, text), swaps, candidates


def looks_like_ui(literal):
    if not literal or literal.startswith("@") or literal in SKIP_LITERALS:
        return False
    if ":" in literal or "=" in literal:
        return False
    if re.fullmatch(r"[A-Z0-9_ ()/.&,%-]*", literal):
        return False
    return bool(re.search(r"[A-Z]", literal) or " " in literal)


def transform_java(text, en_to_key, prefix):
    swaps, manual, candidates = 0, [], []
    out_lines = []
    for line in text.splitlines(keepends=True):
        if line.strip().startswith(("//", "*", "/*")):
            out_lines.append(line)
            continue
        if DECL_RE.search(line) and any('"%s"' % en in line for en in en_to_key):
            manual.append(line.strip())
            out_lines.append(line)
            continue
        code = line
        for en, key in sorted(en_to_key.items(), key=lambda kv: -len(kv[0])):
            lit = '"%s"' % en
            if lit in code:
                code = code.replace(lit, "%s(R.string.%s)" % (prefix, key))
                swaps += 1
        # setSeg2 keyed its grey-out off the literal "Verified"; the swap
        # above rewrites that literal, so the 4th boolean has to come back.
        pat = (r'(setSeg2\([^()]*?,\s*)' + re.escape(prefix)
               + r'\(R\.string\.verified\)(,\s*0x[0-9A-Fa-f]+\s*\))')
        code, n = re.subn(
            pat,
            lambda m: m.group(1) + prefix + "(R.string.verified)" + m.group(2) + ", true",
            code)
        swaps += n
        for lit in re.findall(r'"([^"\n]*)"', code):
            if lit in en_to_key or not looks_like_ui(lit):
                continue
            # matchers (startsWith/contains/regex calls) and logcat lines are
            # internal strings, never UI chrome.
            if re.search(r'\.(?:startsWith|endsWith|contains|matches|replaceFirst'
                         r'|replaceAll|appendReplacement|compile|quoteReplacement)\('
                         r'|\bLog\.[iewdv]\(', code):
                continue
            if lit not in candidates:
                candidates.append(lit)
        out_lines.append(code)
    return "".join(out_lines), swaps, candidates, manual


def main():
    check = "--check" in sys.argv[1:]
    en_to_key, en_of_key, zh_of_key, order = load_map()
    stale, report = [], []

    jobs = [(rel, ROOT / rel, "xml") for rel in
            [str(p.relative_to(ROOT)) for d in XML_DIRS for p in sorted(Path(ROOT / d).glob("*.xml"))]]
    jobs += [(rel, ROOT / rel, "java") for rel in JAVA_FILES]

    results = {}
    used_keys = set()
    for rel, path, kind in jobs:
        old = path.read_text(encoding="utf-8")
        if kind == "xml":
            new, swaps, cand = transform_xml(old, en_to_key)
            manual = []
        else:
            new, swaps, cand, manual = transform_java(old, en_to_key, JAVA_FILES[rel])
        results[rel] = new
        used_keys |= set(re.findall(r'R\.string\.(\w+)|@string/(\w+)', new)
                         and {a or b for a, b in re.findall(r'R\.string\.(\w+)|@string/(\w+)', new)})
        if swaps:
            report.append("%-28s %2d swaps" % (rel, swaps))
        for c in cand:
            report.append("UNTRANSLATED  %s  %r" % (rel, c))
        for m in manual:
            report.append("MANUAL        %s  %s" % (rel, m))
        if new != old:
            stale.append((path, new))

    generated = {
        ROOT / "app/src/main/res/values/strings.xml": gen_strings(order, en_of_key),
        ROOT / "app/src/main/res/values-zh/strings.xml": gen_strings(order, zh_of_key),
    }
    for path, new in generated.items():
        if path.read_text(encoding="utf-8") != new:
            stale.append((path, new))

    unused = [k for k in order if k not in used_keys]
    for k in unused:
        report.append("UNUSED        no UI reference to R.string.%s / @string/%s" % (k, k))

    if check:
        if stale or report:
            for line in report:
                print(line)
            print("localize: tree is NOT up to date (%d files would change)" % len(stale))
            return 1
        print("localize: tree is up to date (%d mapped strings)" % len(order))
        return 0

    for path, new in stale:
        path.write_text(new, encoding="utf-8")
        print("wrote %s" % path.relative_to(ROOT))
    for line in report:
        print(line)
    print("localize: %d files changed, %d mapped strings" % (len(stale), len(order)))
    if report:
        print("localize: review UNTRANSLATED / MANUAL / UNUSED lines above")
    return 0


if __name__ == "__main__":
    sys.exit(main())
