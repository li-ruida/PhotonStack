"""Read-only independent Astropy geometry and SVG/recipe audit.
Usage: python sky_annotation_validation.py annotation-directory
Requires Astropy and NumPy. Uses WCSLIB and SkyCoord angular offsets, not engine
projection, ellipse or clipping implementations. Catalog precision is separate
from numerical agreement with its published coordinates and illustrative shapes.
"""
import base64,csv,hashlib,json,math,shlex,sys
from pathlib import Path
from xml.etree import ElementTree as ET
import numpy as np
from astropy.io import fits
from astropy.coordinates import SkyCoord
from astropy import units as u
from astropy.wcs import WCS

def clip(a,b,w,h):
    # Cohen-Sutherland, independent of engine Liang-Barsky implementation.
    xmin=ymin=-.5;xmax=w-.5;ymax=h-.5
    def code(x,y):return (1 if x<xmin else 2 if x>xmax else 0)|(4 if y<ymin else 8 if y>ymax else 0)
    x1,y1=a;x2,y2=b
    for _ in range(12):
        c1,c2=code(x1,y1),code(x2,y2)
        if not (c1|c2):return [[x1,y1],[x2,y2]]
        if c1&c2:return None
        c=c1 or c2
        if c&8:x=x1+(x2-x1)*(ymax-y1)/(y2-y1);y=ymax
        elif c&4:x=x1+(x2-x1)*(ymin-y1)/(y2-y1);y=ymin
        elif c&2:y=y1+(y2-y1)*(xmax-x1)/(x2-x1);x=xmax
        else:y=y1+(y2-y1)*(xmin-x1)/(x2-x1);x=xmin
        if c==c1:x1,y1=x,y
        else:x2,y2=x,y
    raise AssertionError('clipping failed')

def main(folder):
    r=json.loads((folder/'report.json').read_text());source=Path(r['source'])
    assert hashlib.sha256(source.read_bytes()).hexdigest()==r['sourceSHA']
    for line in (folder/'recipe.txt').read_text().splitlines()[3:]:
        kind,name,digest=shlex.split(line);p=folder/('catalog' if kind=='catalog' else '')/name
        assert hashlib.sha256(p.read_bytes()).hexdigest()==digest,name
    rows={}
    for filename in ['NGC.csv','addendum.csv']:
        rows.update({o['Name']:o for o in csv.DictReader((folder/'catalog'/filename).open(),delimiter=';')})
    outlines={}
    with (folder/'catalog/outlines.txt').open() as f:
        assert next(f).strip()=='PhotonStack-Outlines-1'
        for line in f:
            name,n=line.split();points=[list(map(float,next(f).split())) for _ in range(int(n))]
            outlines.setdefault(name,[]).append(np.array(points))
    w=WCS(fits.getheader(source),naxis=2);maximum=0;segments=0
    for o in r['objects']:
        row=rows[o['id']];coord=SkyCoord(row['RA'],row['Dec'],unit=(u.hourangle,u.deg))
        np.testing.assert_allclose([o['ra'],o['dec']],[coord.ra.deg,coord.dec.deg],rtol=0,atol=1e-10)
        center=w.all_world2pix([[o['ra'],o['dec']]],0,tolerance=1e-10,maxiter=100)[0]
        maximum=max(maximum,float(np.max(np.abs(center-[o['x'],o['y']]))))
        if o['outlineKind']=='published-contour':shapes=outlines[o['id']]
        elif o['outlineKind']=='catalog-ellipse':
            t=np.linspace(0,2*np.pi,129);pa=np.deg2rad(o['positionAngle'] or 0)
            major=float(row['MajAx'])/120*np.cos(t);minor=float(row['MinAx'])/120*np.sin(t)
            east=major*np.sin(pa)+minor*np.cos(pa);north=major*np.cos(pa)-minor*np.sin(pa)
            curve=coord.directional_offset_by(np.arctan2(east,north)*u.rad,np.hypot(east,north)*u.deg)
            shapes=[np.column_stack([curve.ra.deg,curve.dec.deg])]
        else:shapes=[]
        expected=[]
        for shape in shapes:
            points=w.all_world2pix(shape,0,tolerance=1e-10,maxiter=100)
            for a,b in zip(points[:-1],points[1:]):
                part=clip(a,b,r['width'],r['height'])
                if part is not None:expected.append(part)
        actual=[pair for path in o['paths'] for pair in zip(path[:-1],path[1:])]
        assert len(expected)==len(actual),(o['id'],len(expected),len(actual))
        if actual:
            error=float(np.max(np.abs(np.asarray(expected)-np.asarray(actual))));maximum=max(maximum,error);segments+=len(actual)
        assert maximum<1e-4,(o['id'],maximum)
    ns={'s':'http://www.w3.org/2000/svg'};svg=ET.parse(folder/'annotated.svg');image=svg.find('s:image',ns)
    assert base64.b64decode(image.attrib['href'].split(',')[1])==(folder/'preview.png').read_bytes()
    text=' '.join(n.text or '' for n in svg.findall('.//s:text',ns))
    for o in r['objects']:
        if o['labelVisible'] and r['showLabels']:assert o['label'] in text
    assert 'CC BY-SA 4.0' in text
    print(json.dumps({'objects':len(r['objects']),'segments':segments,'maximumProjectionDifferencePixels':maximum,'sourceSHA':r['sourceSHA']},indent=2))
if __name__=='__main__':main(Path(sys.argv[1]))
