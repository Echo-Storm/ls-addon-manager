r"""Two screenshots of the same moment, the original and the enhanced one, as two labelled full frames for the README: 1920 wide JPEGs, the
label at the top right (Lossless Scaling's FPS counter at the top left stays, on purpose). Made the Silent Hill f pair in docs/images.

    python handoff\scripts\make_readme_pair.py <original.png> <enhanced.png> <out-original.jpg> <out-enhanced.jpg> [--label "DLSS 5 Neural Rendering"]
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('original'); ap.add_argument('enhanced'); ap.add_argument('out_original'); ap.add_argument('out_enhanced')
ap.add_argument('--label', default='DLSS 5 Neural Rendering')
ap.add_argument('--width', type=int, default=1920)
a = ap.parse_args()
try:
    font = ImageFont.truetype('segoeuib.ttf', 30)
except OSError:
    font = ImageFont.load_default()
for src, dst, text in ((a.original, a.out_original, 'Original'), (a.enhanced, a.out_enhanced, a.label)):
    im = Image.open(src).convert('RGB')
    im = im.resize((a.width, round(im.height * a.width / im.width)), Image.LANCZOS)
    draw = ImageDraw.Draw(im)
    tw = draw.textlength(text, font=font)
    draw.rectangle((im.width - tw - 26, 0, im.width, 48), fill=(0, 0, 0))
    draw.text((im.width - tw - 13, 7), text, fill=(255, 255, 255), font=font)
    im.save(dst, quality=90, optimize=True)
    print('wrote', dst)
