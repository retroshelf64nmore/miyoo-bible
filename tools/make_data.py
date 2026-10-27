# Builds the Bible reader's data folder from eBible.org USFM (eng-kjv, engwebp), keeping the words of Jesus (\wj).
# Output per version: <out>/<ver>/index.txt ("Name<TAB>chapters" x66) + <out>/<ver>/NN.txt with tagged lines:
#   T title | C chapter | H heading (psalm titles, section headings) | V verse text
# Red letters: \x01 starts and \x02 ends a run of Jesus' words inside a V line.
# Usage: python -I make_data.py <booklist.txt> <out> kjv=<usfm dir> web=<usfm dir>
import os, re, sys, glob
booklist, out, *vers = sys.argv[1:]
names = [l.strip() for l in open(booklist, encoding='utf-8-sig') if l.strip()]
CODES = ('GEN EXO LEV NUM DEU JOS JDG RUT 1SA 2SA 1KI 2KI 1CH 2CH EZR NEH EST JOB PSA PRO ECC SNG ISA JER LAM EZK DAN HOS JOL AMO '
         'OBA JON MIC NAM HAB ZEP HAG ZEC MAL MAT MRK LUK JHN ACT ROM 1CO 2CO GAL EPH PHP COL 1TH 2TH 1TI 2TI TIT PHM HEB JAS '
         '1PE 2PE 1JN 2JN 3JN JUD REV').split()
HEADINGS = re.compile(r'(d|s\d?|ms\d?|qa|sp)$')                    # psalm titles, section headings, acrostic letters
SKIP = re.compile(r'(id|ide|h|toc\d|rem|cl|mr|r|sr|imt\d?|is\d?|ip|ili\d?|io\d?|ie)$')
CHAR_STYLES = {'w', 'add', 'nd', 'it', 'bd', 'sc', 'qs', 'tl', 'k', 'bk', 'pn', 'em', 'sls', 'qt', 'ord', 'no', 'qac'}

def clean(s):
    s = re.sub(r'\s+', ' ', s)
    s = re.sub(r'\x01(\s*)\x02', r'\1', s)                          # empty red runs
    s = s.replace('\x01 ', ' \x01').replace(' \x02', '\x02 ')       # keep spaces outside the runs
    return re.sub(' +', ' ', s).strip()

def parse(path):
    text = open(path, encoding='utf-8-sig').read()
    text = re.sub(r'\\f .*?\\f\*|\\fe .*?\\fe\*|\\x .*?\\x\*', '', text, flags=re.S)   # footnotes, cross references
    text = re.sub(r'\|[^\\]*', '', text)                                                  # word attributes (strong=...)
    text = re.sub(r'\\([cv]) (\d+)\s*', lambda m: '\\%s\x03%s\x03' % (m.group(1), m.group(2)), text)
    title, chapters, verse, head, red, mode = [], [], None, None, False, 'p'
    def end_verse():
        nonlocal verse
        if verse is not None:
            chapters[-1][1].append('V %d %s' % (verse[0], clean(''.join(verse[1]) + ('\x02' if red else ''))))
            verse = None
    def end_head():
        nonlocal head
        if head is not None:
            s = clean(''.join(head).replace('\x01', '').replace('\x02', ''))
            if s and chapters: chapters[-1][1].append('H ' + s)
            head = None
    # the one space after an opening marker is a separator; after a closing marker (\w*) it is real text
    for m in re.finditer(r'\\(\+?[a-z]+\d*\*)|\\(\+?[a-z]+\d*)(?:\x03(\d+)\x03)?\s?|([^\\]+)', text):
        mk, num, txt = m.group(1) or m.group(2), m.group(3), m.group(4)
        if txt is not None:
            if mode == 'mt': title.append(txt)
            elif mode == 'skip': pass
            elif head is not None: head.append(txt)
            elif verse is not None: verse[1].append(txt)
            continue
        mk = mk.lstrip('+')
        if mk == 'c': end_verse(); end_head(); chapters.append((int(num), [])); mode = 'p'
        elif mk == 'v': end_verse(); end_head(); verse = (int(num), ['\x01'] if red else []); mode = 'p'
        elif mk == 'wj':
            red = True
            if verse is not None: verse[1].append('\x01')
        elif mk == 'wj*':
            red = False
            if verse is not None: verse[1].append('\x02')
        elif mk.endswith('*') or mk in CHAR_STYLES: pass
        elif HEADINGS.match(mk): end_verse(); end_head(); head = []; mode = 'h'
        elif re.match(r'mt\d?$', mk): mode = 'mt'
        elif SKIP.match(mk): mode = 'skip'
        else: end_head(); mode = 'p'                                  # paragraph / poetry markers
    end_verse(); end_head()
    return clean(' '.join(title)), chapters

for spec in vers:
    ver, src = spec.split('=', 1)
    files = {open(f, encoding='utf-8-sig').readline().split()[1]: f for f in glob.glob(os.path.join(src, '*.usfm'))}
    os.makedirs(os.path.join(out, ver), exist_ok=True)
    total = red = 0
    with open(os.path.join(out, ver, 'index.txt'), 'w', encoding='utf-8', newline='\n') as idx:
        for i, (code, name) in enumerate(zip(CODES, names), 1):
            title, chs = parse(files[code])
            idx.write(f'{name}\t{len(chs)}\n')
            with open(os.path.join(out, ver, f'{i:02d}.txt'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(f'T {(title if ver == "kjv" and title else name).upper()}\n')
                for ch, lines in chs:
                    fh.write(f'C {ch}\n')
                    for l in lines: fh.write(l + '\n')
                    total += sum(l.startswith('V') for l in lines)
                    red += sum('\x01' in l for l in lines)
    print(ver, 'verses', total, 'red verses', red)
