"""score_dump_regions.py MASKS_T.npz DUMP.raw...: nr_fgeval's frames (dumpraw=FILE) on the made-up scene (tools/flowlab/make_synth_clip.py), scored by region.

Each dump entry is a frame number (the truth frame's number in the clip) and four 1280 x 720 RGBA pictures: the frame made, the truth, the real frame before, the one after. The truth
frame 2r - 1 sits at masks_t[r - 1]. Output: per region, the PSNR gain (dB) of the frame made over the plain blend of the two real frames."""
import sys
import numpy as np

W, H = 1280, 720
ENTRY = 4 + 4 * W * H * 4


def main():
    masks = dict(np.load(sys.argv[1]))
    for path in sys.argv[2:]:
        raw = np.memmap(path, np.uint8, 'r')
        n = len(raw) // ENTRY
        res = {m: [] for m in masks}; tot = []
        for e in range(n):
            rel = int(np.frombuffer(raw[e * ENTRY:e * ENTRY + 4], np.int32)[0])
            o = e * ENTRY + 4
            pic, truth, a, c = [raw[o + k * W * H * 4:o + (k + 1) * W * H * 4].reshape(H, W, 4)[:, :, :3].astype(np.float32) for k in range(4)]
            r = (rel + 1) // 2
            if r - 1 >= len(masks['wall']): continue
            eg = ((pic - truth) ** 2).mean(-1); eb = ((0.5 * (a + c) - truth) ** 2).mean(-1)
            inner = np.zeros((H, W), bool); inner[24:H - 24, 24:W - 24] = True
            for m, mk in masks.items():
                mk = mk[r - 1] & inner
                if mk.any(): res[m].append(10 * np.log10(eb[mk].mean() / max(eg[mk].mean(), 1e-6)))
            tot.append(10 * np.log10(eb[inner].mean() / max(eg[inner].mean(), 1e-6)))
        print('%-34s %2d frames: whole %+.2f dB | ' % (path.replace(chr(92), '/').split('/')[-1], len(tot), np.mean(tot)) + '   '.join('%s %+.2f' % (m, np.mean(v)) for m, v in res.items()))


main()
