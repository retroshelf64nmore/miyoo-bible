# Builds one reader data folder per Bible version from eBible.org USFM, keeping the words of Jesus (\wj).
# Output: <out>/<id>/meta.txt   key<TAB>value (language, title, label, license, sections ...)
#         <out>/<id>/index.txt  CODE<TAB>book name<TAB>chapters<TAB>section (0 OT, 1 NT, 2 deuterocanon)
#         <out>/<id>/NN.txt     T title | C chapter | H heading | V verse   (red letters: \x01 ... \x02)
#         <out>/<id>/LICENSE.txt
# Usage: python make_data.py <selection.json> <usfm root with <id>/ folders> <out> <font.ttf> <fallback font.ttf> <cjk font dir>
#   usfm root: each translation's eBible USFM zip (https://ebible.org/Scriptures/<id>_usfm.zip) unpacked into <id>/
#   cjk font dir: NotoSerifJP/SC/KR.ttf (+ _OFL.txt) from github.com/google/fonts (ofl/notoserifjp ...)
#   needs: pip install pymupdf fonttools
import glob, json, os, re, shutil, sys, unicodedata
if os.environ.get('PYLIBS'): sys.path.insert(0, os.environ['PYLIBS'])     # optional local package folder
import pymupdf

sel_path, usfm_root, out, font_path, fallback_path, cjk_dir = sys.argv[1:7]
OT = ('GEN EXO LEV NUM DEU JOS JDG RUT 1SA 2SA 1KI 2KI 1CH 2CH EZR NEH EST JOB PSA PRO ECC SNG ISA JER LAM EZK DAN HOS JOL AMO '
      'OBA JON MIC NAM HAB ZEP HAG ZEC MAL').split()
NT = 'MAT MRK LUK JHN ACT ROM 1CO 2CO GAL EPH PHP COL 1TH 2TH 1TI 2TI TIT PHM HEB JAS 1PE 2PE 1JN 2JN 3JN JUD REV'.split()
DC = 'TOB JDT ESG WIS SIR BAR LJE S3Y SUS BEL 1MA 2MA 3MA 4MA 1ES 2ES MAN PS2 DAG'.split()
WITH_DC = {'latVUC'}                       # Catholic editions keep their extra books
TITLES = {'eng-kjv': 'King James Version'}       # eng-kjv's apocrypha files aren't used
SHORT_NAMES = {('eng', 'SNG'): 'Solomon'}       # book-list names chosen by hand (language code, book code)
LANG_FIX = {'kor': '한국어'}                    # eBible's native name for Korean reads "Korean person"
LABELS = {'eng-kjv': 'KJV', 'engwebp': 'WEB', 'kor': '한국어', 'spaRV1909': 'RV1909', 'latVUC': 'Vulgata', 'fraLSG': 'LSG 1910', 'deu1912': 'Luther 1912',
          'russyn': 'Синодальный', 'epo': 'Londona Biblio'}
SECTIONS = {   # section headings in the book list, where I'm sure of the wording; other languages show a plain divider
    'eng': ('OLD TESTAMENT', 'NEW TESTAMENT', 'APOCRYPHA'), 'spa': ('ANTIGUO TESTAMENTO', 'NUEVO TESTAMENTO', 'DEUTEROCANÓNICOS'),
    'fra': ('ANCIEN TESTAMENT', 'NOUVEAU TESTAMENT', 'DEUTÉROCANONIQUES'), 'deu': ('ALTES TESTAMENT', 'NEUES TESTAMENT', 'APOKRYPHEN'),
    'ita': ('ANTICO TESTAMENTO', 'NUOVO TESTAMENTO', 'DEUTEROCANONICI'), 'por': ('ANTIGO TESTAMENTO', 'NOVO TESTAMENTO', 'DEUTEROCANÓNICOS'),
    'nld': ('OUDE TESTAMENT', 'NIEUWE TESTAMENT', 'APOCRIEFEN'), 'lat': ('VETUS TESTAMENTUM', 'NOVUM TESTAMENTUM', 'LIBRI DEUTEROCANONICI'),
    'epo': ('MALNOVA TESTAMENTO', 'NOVA TESTAMENTO', 'DEŬTEROKANONAJ'), 'rus': ('ВЕТХИЙ ЗАВЕТ', 'НОВЫЙ ЗАВЕТ', 'НЕКАНОНИЧЕСКИЕ'),
    'ukr': ('СТАРИЙ ЗАВІТ', 'НОВИЙ ЗАВІТ', 'НЕКАНОНІЧНІ'), 'ces': ('STARÝ ZÁKON', 'NOVÝ ZÁKON', 'DEUTEROKANONICKÉ'),
    'pol': ('STARY TESTAMENT', 'NOWY TESTAMENT', 'DEUTEROKANONICZNE'), 'ron': ('VECHIUL TESTAMENT', 'NOUL TESTAMENT', 'DEUTEROCANONICE'),
    'vie': ('CỰU ƯỚC', 'TÂN ƯỚC', 'THỨ KINH'), 'hat': ('ANSYEN TESTAMAN', 'NOUVO TESTAMAN', ''),
    'swh': ('AGANO LA KALE', 'AGANO JIPYA', ''), 'swa': ('AGANO LA KALE', 'AGANO JIPYA', ''), 'ind': ('PERJANJIAN LAMA', 'PERJANJIAN BARU', ''),
    'tgl': ('LUMANG TIPAN', 'BAGONG TIPAN', ''), 'dan': ('DET GAMLE TESTAMENTE', 'DET NYE TESTAMENTE', ''),
    'swe': ('GAMLA TESTAMENTET', 'NYA TESTAMENTET', ''), 'nob': ('DET GAMLE TESTAMENTE', 'DET NYE TESTAMENTE', ''),
    'fin': ('VANHA TESTAMENTTI', 'UUSI TESTAMENTTI', ''), 'hun': ('ÓSZÖVETSÉG', 'ÚJSZÖVETSÉG', ''),
}
HEADINGS = re.compile(r'(d|s\d?|ms\d?|qa|sp)$')
SKIP = re.compile(r'(id|ide|h|toc\d|toca\d|rem|cl|cp|mr|r|sr|imt\d?|is\d?|ip|ipr|ili\d?|io\d?|iot|ie|iex|imte\d?|ib)$')
CHAR_STYLES = {'w', 'add', 'nd', 'it', 'bd', 'bdit', 'sc', 'qs', 'tl', 'k', 'bk', 'pn', 'png', 'em', 'sls', 'qt', 'ord', 'no', 'qac', 'jmp', 'wg', 'wh', 'wa', 'addpn', 'dc', 'sig', 'lik', 'liv', 'litl'}

FIX = {0x2011: '-', 0x201F: '"', 0x00AD: '', 0x231C: '‘', 0x231D: '’'}   # nb hyphen, reversed quote, soft hyphen, corner quotes
def strip_char(c):
    cat = unicodedata.category(c)
    if cat in ('Zs', 'Zl', 'Zp'): return ' '
    if cat == 'Cf' or (cat == 'Cc' and c not in '\x01\x02' and not c.isspace()): return ''    # direction marks, BOMs, stray controls
    return c
def clean(s):
    s = ''.join(FIX.get(ord(c)) if ord(c) in FIX else strip_char(c) for c in s)
    s = re.sub(r'[֐-׿]', '', s)                 # Hebrew letters on Psalm 119's stanza headings ("א Aleph" -> "Aleph")
    s = re.sub(r'\s+', ' ', s)
    s = re.sub(r'\x01(\s*)\x02', r'\1', s)
    s = s.replace('\x01 ', ' \x01').replace(' \x02', '\x02 ')
    return re.sub(' +', ' ', s).strip()

def parse(path):
    text = unicodedata.normalize('NFC', open(path, encoding='utf-8-sig').read())
    names = {k: clean(m.group(1)) for k in ('toc2', 'h', 'toc1') for m in [re.search(r'\\%s ([^\n\\]+)' % k, text)] if m}
    text = re.sub(r'\\(f|fe|x|fig|rq|ca|va|vp) .*?\\\1\*', '', text, flags=re.S)    # notes, figures, alternate numbers
    text = re.sub(r'\|[^\\]*', '', text)                                                # word attributes
    text = re.sub(r'\\([cv]) (\d+)[a-z]?(?:-\d+[a-z]?)?\s*', lambda m: '\\%s\x03%s\x03' % (m.group(1), m.group(2)), text)
    title, chapters, verse, head, red, mode = [], [], None, None, False, 'p'
    def end_verse():
        nonlocal verse
        if verse is not None:
            if chapters: chapters[-1][1].append('V %d %s' % (verse[0], clean(''.join(verse[1]) + ('\x02' if red else ''))))
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
        if mk == 'c':
            end_verse(); end_head(); n = int(num)
            if not chapters or chapters[-1][0] != n: chapters.append((n, []))
            mode = 'p'
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
        else: end_head(); mode = 'p'
    end_verse(); end_head()
    chapters = [(n, ls) for n, ls in chapters if any(l.startswith('V') for l in ls)]
    # short name for lists, the top bar and chapter headings ("Mark", not "The Good News According to Mark";
    # the full title from \mt still opens chapter 1)
    name = names.get('h') or names.get('toc2') or names.get('toc1') or ''
    if name.isupper() and len(name) > 4:                     # 'GENESIS' -> 'Genesis', but keep Roman numerals ('II')
        name = ' '.join(w if re.fullmatch('[IVX]+', w) else w.capitalize() for w in name.split())
    return clean(' '.join(title)), re.sub(r'\s+', ' ', name).strip(), chapters

font, fallback = pymupdf.Font(fontfile=font_path), pymupdf.Font(fontfile=fallback_path)   # the reader falls back the same way
# Chinese / Japanese / Korean ship their own font, cut down to the characters that Bible uses (font.ttf in its folder).
CJK_DIR = cjk_dir
CJK = {'jpnm': 'NotoSerifJP', 'cmnswcb': 'NotoSerifSC', 'kor': 'NotoSerifKR'}
NO_SPACES = {'jpnm', 'cmnswcb'}            # lines may break between any two characters
cjk_fonts = {}

def make_cjk_font(src, chars, dst):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    from fontTools import subset
    f = TTFont(src)
    if 'fvar' in f: f = instancer.instantiateVariableFont(f, {'wght': 400})       # stb_truetype draws only the default master
    opts = subset.Options(); opts.layout_features = ['kern', 'vert', 'locl']; opts.name_IDs = ['*']; opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes={ord(c) for c in chars} | set(range(32, 127)))
    sub.subset(f)
    f.save(dst)
selection = json.load(open(sel_path, encoding='utf-8'))
report = []
for s in selection:
    vid = s['id']
    files = {}
    for f in glob.glob(os.path.join(usfm_root, vid, '*.usfm')):
        first = open(f, encoding='utf-8-sig').readline().split()
        if len(first) > 1: files[first[1].upper()] = f
    codes = [(c, 0) for c in OT] + [(c, 1) for c in NT] + ([(c, 2) for c in DC] if vid in WITH_DC else [])
    books, chars = [], set()
    for code, sec in codes:
        if code not in files: continue
        title, name, chs = parse(files[code])
        if not chs: continue
        books.append((code, sec, title, SHORT_NAMES.get((s['code'], code)) or name or code, chs))
        chars.update(name, title)
        for _, ls in chs:
            for l in ls: chars.update(l[2:])
    extra = None
    if vid in CJK:
        extra = cjk_fonts.setdefault(vid, pymupdf.Font(fontfile=os.path.join(CJK_DIR, CJK[vid] + '.ttf')))
    missing = sorted(c for c in chars if ord(c) >= 32 and c not in '\x01\x02\x03' and not font.has_glyph(ord(c))
                     and not fallback.has_glyph(ord(c)) and not (extra and extra.has_glyph(ord(c))))
    nb = sum(1 for b in books if b[1] < 2)
    nv = sum(1 for b in books for _, ls in b[4] for l in ls if l.startswith('V'))
    status = 'books' if nb != 66 else 'font' if missing else 'partial' if nv < 29000 else 'ok'     # skip unfinished translations
    report.append((vid, s['langEn'], nb, len(missing), ''.join(missing[:12]), status))
    if status != 'ok': continue
    d = os.path.join(out, vid)
    os.makedirs(d, exist_ok=True)
    sec_names = SECTIONS.get(s['code'], ('', '', ''))
    s['lang'] = LANG_FIX.get(vid, s['lang'])
    label = LABELS.get(vid) or s['lang']                       # top-bar name: the language, in that language
    meta = {'id': vid, 'language': s['lang'], 'language_en': s['langEn'], 'code': s['code'], 'title': TITLES.get(vid, s['title']), 'label': label,
            'license': s['license'], 'copyright': s['copyright'], 'source': 'https://ebible.org/find/details.php?id=' + vid,
            'sections': '|'.join(sec_names)}
    if vid in CJK:
        make_cjk_font(os.path.join(CJK_DIR, CJK[vid] + '.ttf'), {c for c in chars if ord(c) >= 32}, os.path.join(d, 'font.ttf'))
        shutil.copy(os.path.join(CJK_DIR, CJK[vid] + '_OFL.txt'), os.path.join(d, 'FONT_LICENSE.txt'))
        meta['font'] = 'font.ttf'
    if vid in NO_SPACES: meta['breaks'] = 'any'
    with open(os.path.join(d, 'meta.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        for k, v in meta.items(): fh.write(f'{k}\t{v}\n')
    with open(os.path.join(d, 'index.txt'), 'w', encoding='utf-8', newline='\n') as idx:
        for i, (code, sec, title, name, chs) in enumerate(books, 1):
            idx.write(f'{code}\t{name}\t{len(chs)}\t{sec}\n')
            with open(os.path.join(d, f'{i:02d}.txt'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(f'T {title or name}\n')
                for ch, lines in chs:
                    fh.write(f'C {ch}\n')
                    for l in lines: fh.write(l + '\n')
    lic_text = {'PD': 'This text is in the public domain.'}.get(s['license'], f"Licensed under Creative Commons {s['license'].replace('CC-', '')}: https://creativecommons.org/licenses/")
    with open(os.path.join(d, 'LICENSE.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(f"{s['title']} ({s['langEn']})\n{s['copyright']}\n{lic_text}\nSource: {meta['source']}\n"
                 "Converted for the Miyoo Mini Bible app: footnotes and cross references removed, text otherwise unchanged.\n")
# The language list shows every name at once, but a CJK font only loads with its own translation: so the
# characters of the names get two tiny shared fonts (Simplified Chinese face first, then Korean for hangul).
name_chars = {c for s in selection if s['id'] in CJK for c in LANG_FIX.get(s['id'], s['lang'])}
os.makedirs(os.path.join(out, '_fonts'), exist_ok=True)
for i, fam in enumerate(('NotoSerifSC', 'NotoSerifKR'), 1):
    f = pymupdf.Font(fontfile=os.path.join(CJK_DIR, fam + '.ttf'))
    mine = {c for c in name_chars if f.has_glyph(ord(c))}
    name_chars -= mine
    if mine: make_cjk_font(os.path.join(CJK_DIR, fam + '.ttf'), mine, os.path.join(out, '_fonts', f'names_{i}.ttf'))
shutil.copy(os.path.join(CJK_DIR, 'NotoSerifSC_OFL.txt'), os.path.join(out, '_fonts', 'OFL.txt'))
with open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8', newline='\n') as fh:
    for r in report: fh.write('%-12s %-26s books=%-3d missing=%-4d %-14s %s\n' % r)
print('ok:', sum(r[5] == 'ok' for r in report), 'of', len(report))
