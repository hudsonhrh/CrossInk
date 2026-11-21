"""Run with python -m unittest discover -s test/device/optical_ghosting."""
import tempfile
import unittest
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image

from make_book import pattern
from score import sample, score


class CameraControls(unittest.TestCase):
    def test_known_residuals_through_jpeg_perspective_and_shading(self):
        rng = np.random.default_rng(913)
        # The synthetic camera operator uses Pillow's quadrilateral transform.
        # Scoring independently uses a fitted projective map; this near-affine
        # view also exercises the wide margin used around every tile.
        im = np.asarray(Image.open(BytesIO(pattern(1))).convert('L'))[2:-2,2:-2] / 255.
        height,width=im.shape
        y,x=np.mgrid[:height,:width]
        ink=42 + 154*im
        opposite=42 + 154*(1-im)
        # Fixed patterned wallpaper and lower-contrast source with local shading.
        wall=119+32*np.sin(x/18)*np.cos(y/24) + .08*y
        xy=np.array([[53.,44.],[333.,50.],[323.,409.],[43.,403.]])
        # Inverse affine transform used by Pillow maps camera -> display pixel.
        mat=np.array([[280/width,-10/height,53],[6/width,359/height,44],[0,0,1]])
        inv=np.linalg.inv(mat).ravel()[:6]
        with tempfile.TemporaryDirectory() as tmp:
            def expose(pixels, name):
                values=pixels + rng.normal(0,.9,size=im.shape) + rng.normal(0,.3)
                panel=Image.fromarray(np.uint8(np.clip(values,0,255)))
                cam=panel.transform((398,457),Image.Transform.AFFINE,inv,Image.Resampling.BILINEAR)
                path=Path(tmp)/(name+'.jpg')
                cam.save(path,quality=88)
                return sample(path,xy)
            a,b=expose(ink,'a'),expose(opposite,'b')
            levels=[-.065,0,.015,.06,.14]
            rng.shuffle(levels)
            for i,fraction in enumerate(levels):
                final_a=expose(wall + fraction*(ink-wall),f'after-a{i}')
                final_b=expose(wall + fraction*(opposite-wall),f'after-b{i}')
                result=score(a,final_a,b,final_b)
                self.assertAlmostEqual(result['residual_percent_of_camera_black_white_contrast'],100*fraction,delta=.4)
                self.assertLess(abs(result['signed_placebo_percent_mean']),.4)
            with self.assertRaisesRegex(ValueError,'opposite patterns'):
                score(a,final_a,a,final_b)
            with self.assertRaisesRegex(ValueError,'Targets differ'):
                score(a,expose(wall,'wall'),b,expose(230-wall,'wrong'))
            with self.assertRaisesRegex(ValueError,'Bad grid corners'):
                sample(path=Path(tmp)/'a.jpg',points=xy[::-1])

    def test_ordinary_text_night_and_day(self):
        from score import score_text
        from PIL import ImageDraw, ImageFont
        import textwrap
        passages = [
            "A heron landed quietly by the water. The oak trees grew tall above the bank. ",
            "Before sunset the meadow fell into shade. Across the river, a single bird sang. ",
            "On the next morning the leaves were still. Beyond the meadow sunlight touched the hills. ",
        ]
        font = ImageFont.load_default(size=15)
        pages = []
        for passage in passages:
            im = Image.new("L", (432,720), 200)
            ImageDraw.Draw(im).multiline_text((0,0), '\n'.join(textwrap.wrap(passage*70,width=53)), fill=40, font=font, spacing=5)
            pages.append(np.array(im,dtype=float))
        a,t,b=pages
        for g in [-.04,0,.025,.075]:
            ta,tb=t+g*(a-t),t+g*(b-t)
            args=(a,ta,b,tb)
            for night in [False,True]:
                imgs=[255-v for v in args] if night else args
                result=score_text(*imgs,dark=night)
                self.assertAlmostEqual(result["text_residual_percent_of_camera_black_white_contrast"],g*100,places=2)
        with self.assertRaisesRegex(ValueError,"Final text pages|Too few background regions"):
            score_text(a,t,b,a)
