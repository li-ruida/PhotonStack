#!/usr/bin/env python3
"""Build the offline annotation data from an explicitly pinned OpenNGC checkout.
Usage: python import-openngc.py upstream-directory destination revision
Raw catalog CSVs are unchanged. Outlines retain every vertex at standard DSS
brightness (level 2) and all unlevelled shapes; no contour fitting/simplification.
"""
import csv,hashlib,json,re,shutil,sys
from pathlib import Path
src,dst,revision=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
if not re.fullmatch('[0-9a-f]{40}',revision):raise ValueError('Specify the upstream commit')
dst.mkdir(parents=True,exist_ok=True)
files={'database_files/NGC.csv':'NGC.csv','database_files/addendum.csv':'addendum.csv','LICENSES/CC-BY-SA-4.0.txt':'LICENSE.txt','README.md':'UPSTREAM-README.md','AUTHORS':'AUTHORS','NGC_guide.txt':'NGC_guide.txt','outlines/metodology.txt':'outlines-methodology.txt'}
manifest={'repository':'https://github.com/mattiaverga/OpenNGC','revision':revision,'license':'CC-BY-SA-4.0','upstreamFiles':{}}
def record(p):manifest['upstreamFiles'][str(p.relative_to(src))]=hashlib.sha256(p.read_bytes()).hexdigest()
for name,target in files.items():shutil.copyfile(src/name,dst/target);record(src/name)
objects=[]
for file in sorted((src/'outlines/objects').glob('*.txt')):
    if '_lv' in file.stem and not file.stem.endswith('_lv2'):continue
    record(file);name=file.stem.split('_lv')[0];shape=[]
    for row in csv.DictReader(file.open(),delimiter='\t'):
        # Keep the half-open domain even when a tiny negative RA rounds to 360.
        point=((float(row['RAJ2000'])%360)%360,float(row['DEJ2000']))
        if not shape or point!=shape[-1]:shape.append(point)
        if row['Cont_Flag']=='*':
            if len(shape)>=3:objects.append((name,shape))
            shape=[]
    if len(shape)>=3:objects.append((name,shape))
with (dst/'outlines.txt').open('w') as f:
    f.write('PhotonStack-Outlines-1\n')
    for name,points in objects:
        # A repeated final vertex carries closure explicitly. Do not close open
        # contours by inventing a segment not present in the source.
        f.write(f'{name} {len(points)}\n')
        for ra,dec in points:f.write(f'{ra:.17g} {dec:.17g}\n')
(dst/'REVISION').write_text(revision+'\n')
manifest['generatedOutlineSHA256']=hashlib.sha256((dst/'outlines.txt').read_bytes()).hexdigest()
(dst/'provenance.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n')
(dst/'NOTICE.txt').write_text('OpenNGC — Mattia Verga and contributors\nhttps://github.com/mattiaverga/OpenNGC\nRevision: '+revision+'\nData license: CC BY-SA 4.0; see LICENSE.txt.\nPhotonStack copies catalog CSVs without changes and converts standard-level nebula outlines to a numeric text representation without simplifying vertices.\nSee provenance.json for original file hashes, AUTHORS, UPSTREAM-README.md and outlines-methodology.txt for credits and source methods.\n')
print('Imported',len(objects),'contours,',sum(len(p) for _,p in objects),'vertices')
