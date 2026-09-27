r"""Depth for recorded frames, estimated from each picture alone by Depth Anything V2 Small (tools\fetch_depth_model.ps1), on the GPU
through ONNX Runtime's DirectML provider. For the frame generation experiments: nr_fgeval's depthdir= reads what this writes.

    tools\.venv\Scripts\python tools\depth_maps.py <frames folder> <output folder> [--height 518] [--model <file.onnx>]

<frames folder> holds frame_NNNNN.bmp, as `nr_lsrec export <recording> <folder>` writes them. For each one it writes depth_NNNNN.bin:
the frame's width x height as 32-bit floats, 1 = near and 0 = far (the model's relative inverse depth, scaled by the frame's own 2nd and
98th percentiles), plus depth_NNNNN.png, a picture of it to look at.

Setting up the Python it needs, once:  py -m venv tools\.venv  then  tools\.venv\Scripts\pip install onnxruntime-directml numpy pillow
"""
import argparse
import glob
import os
import re
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MODEL = os.path.join(ROOT, 'addons', 'DLSS5NR01', 'external', 'depth', 'depth_anything_v2_small.onnx')
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames')
    ap.add_argument('out')
    ap.add_argument('--height', type=int, default=518, help="the model's input height (a multiple of 14; the width follows the frame's shape)")
    ap.add_argument('--model', default=DEFAULT_MODEL)
    a = ap.parse_args()
    import onnxruntime as ort
    if not os.path.exists(a.model):
        print('no model at %s: run tools\\fetch_depth_model.ps1' % a.model); return 2
    opts = ort.SessionOptions(); opts.log_severity_level = 3
    session = ort.InferenceSession(a.model, opts, providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
    name = session.get_inputs()[0].name
    files = sorted(glob.glob(os.path.join(a.frames, 'frame_*.bmp')))
    if not files:
        print('no frame_*.bmp in', a.frames); return 2
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    for f in files:
        index = int(re.search(r'frame_(\d+)', f).group(1))
        im = Image.open(f).convert('RGB')
        W, H = im.size
        h = max(14, a.height // 14 * 14); w = max(14, round(W * h / H / 14) * 14)
        x = (np.asarray(im.resize((w, h), Image.BICUBIC), np.float32) / 255.0 - MEAN) / STD
        y = session.run(None, {name: x.transpose(2, 0, 1)[None]})[0][0]
        lo, hi = np.percentile(y, 2), np.percentile(y, 98)
        d = np.clip((y - lo) / max(hi - lo, 1e-6), 0.0, 1.0).astype(np.float32)
        full = np.asarray(Image.fromarray(d, 'F').resize((W, H), Image.BILINEAR), np.float32)
        full.tofile(os.path.join(a.out, 'depth_%05d.bin' % index))
        Image.fromarray((full * 255).astype(np.uint8), 'L').save(os.path.join(a.out, 'depth_%05d.png' % index))
    print('%d depth maps in %.0f s, in %s' % (len(files), time.time() - t0, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
