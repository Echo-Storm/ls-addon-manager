"""fix_field.py RUN_FOLDER OUT.f32 [method=large2] [pairs=99]: the flow at the later frame of every pair, corrected by one of field_lab's methods, in the layout nr_fgeval's mvfile= reads."""
import sys
import numpy as np
import field_lab as fl


def main():
    folder, out = sys.argv[1], sys.argv[2]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[3:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720)); method = args.get('method', 'large2')
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    Fc = np.memmap(folder + '/ours.f32', np.float32, 'r').reshape(-1, h, w, 2)
    pairs = min(len(Fc), int(args.get('pairs', 999)))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    with open(out, 'wb') as f:
        for p in range(pairs):
            r = p + 1
            A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32)
            G, share = fl.METHODS[method](Fc[p].astype(np.float32), A, C, gx, gy)
            f.write(np.ascontiguousarray(G, np.float32).tobytes())
            print('pair %d: changed %.1f %%' % (p, 100 * share), flush=True)


if __name__ == '__main__':
    main()
