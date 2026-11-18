"""Catalog projections, standalone vector export and immutable replay contracts."""
import base64,hashlib,json,math,shutil,struct,tempfile,unittest
from pathlib import Path
from xml.etree import ElementTree as ET
from test_channels import CLI,write,run

CATALOG=Path(__file__).resolve().parents[2]/'resources/OpenNGC'
def source(p,wcs=True):
    header={'CTYPE1':"'RA---TAN'",'CTYPE2':"'DEC--TAN'",'RADESYS':"'ICRS'",'CRPIX1':128.5,'CRPIX2':96.5,'CRVAL1':10.684791666666667,'CRVAL2':41.26905555555555,'CD1_1':-.02,'CD1_2':.001,'CD2_1':.002,'CD2_2':.02}
    write(p/'source.fits',[100+2*math.sin(i*.17) for i in range(256*192)],256,192,header if wcs else {})
    return p/'source.fits'
def annotate(p,folder='out',options=(),ok=True):return run('sky-annotate','--input',p/'source.fits','--catalog-dir',CATALOG,'--output-dir',p/folder,*options,ok=ok)

class Annotation(unittest.TestCase):
    def test_geometry_svg_and_portable_replay(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);f=source(p);original=f.read_bytes();r=annotate(p)
            m31=next(o for o in r['objects'] if o['id']=='NGC0224')
            self.assertAlmostEqual(m31['x'],127.5,places=7);self.assertAlmostEqual(m31['y'],95.5,places=7)
            self.assertEqual(m31['outlineKind'],'catalog-ellipse')
            svg=ET.parse(p/'out/annotated.svg');ns={'s':'http://www.w3.org/2000/svg'}
            image=svg.find('s:image',ns);self.assertIsNotNone(image)
            png=base64.b64decode(image.attrib['href'].split(',',1)[1]);self.assertEqual(png,(p/'out/preview.png').read_bytes())
            self.assertEqual(struct.unpack('>II',png[16:24]),(r['previewWidth'],r['previewHeight']))
            self.assertIn('CC BY-SA 4.0',(p/'out/annotated.svg').read_text())
            self.assertEqual(run('sky-annotation-inspect','--recipe',p/'out/recipe.txt'),r)
            for command,folder in [('sky-annotation-replay','replay'),('sky-annotation-export','export')]:
                self.assertEqual(run(command,'--recipe',p/'out/recipe.txt','--output-dir',p/folder),r)
                for product in ['preview.png','annotated.svg','overlay.svg','report.json','recipe.txt']:
                    self.assertEqual((p/'out'/product).read_bytes(),(p/folder/product).read_bytes())
            self.assertEqual(f.read_bytes(),original)
            annotate(p,ok=False)
            self.assertEqual((p/'out/preview.png').read_bytes(),png)
            for o in r['objects']:
                for path in o['paths']:
                    self.assertGreaterEqual(len(path),2)
                    for x,y in path:self.assertTrue(-.500001<=x<=255.500001 and -.500001<=y<=191.500001)
    def test_filters_unknowns_empty_and_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source(p)
            r=annotate(p,'search',('--query','M31','--outlines','0','--labels','0'))
            self.assertEqual([o['id'] for o in r['objects']],['NGC0224'])
            self.assertEqual(r['showLabels'],False);self.assertEqual(r['showOutlines'],False)
            self.assertEqual(annotate(p,'empty',('--query','<script>&"'))['matched'],0)
            self.assertEqual(annotate(p,'category',('--category','nebulae','--query','M31'))['matched'],0)
            self.assertEqual(annotate(p,'faint',('--limiting-magnitude','-10','--include-unknown','0'))['matched'],0)
            limited=annotate(p,'limit',('--max-objects','1'));self.assertEqual(len(limited['objects']),1);self.assertGreater(limited['matched'],1)
            for args in [('--max-objects','0'),('--max-objects','2.5'),('--query', 'x'*257),('--labels','2'),('--limiting-magnitude','nan'),('--preview-width','50'),('--category','invented'),('--labels','1','--labels','1')]:
                annotate(p,'bad',args,False);self.assertFalse((p/'bad').exists())
            source(p,False);annotate(p,'no-wcs',ok=False);self.assertFalse((p/'no-wcs').exists())
    def test_tamper_and_display_domain(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source(p);annotate(p)
            for file in ['source.fits','out/preview.png','out/report.json','out/catalog/NGC.csv','out/catalog/outlines.txt']:
                f=p/file;original=f.read_bytes();f.write_bytes(original+b'changed')
                run('sky-annotation-inspect','--recipe',p/'out/recipe.txt',ok=False)
                run('sky-annotation-export','--recipe',p/'out/recipe.txt','--output-dir',p/'bad',ok=False)
                self.assertFalse((p/'bad').exists());f.write_bytes(original)
            run('ghs-stretch','--input',p/'source.fits','--output-dir',p/'ghs','--black','90','--white','110')
            r=run('sky-annotate','--input',p/'ghs/display.fits','--catalog-dir',CATALOG,'--output-dir',p/'display')
            self.assertFalse(r['linearDisplayPreview'])

if __name__=='__main__':unittest.main()
