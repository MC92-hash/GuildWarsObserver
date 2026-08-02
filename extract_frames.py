import imageio.v3 as iio
import os
import numpy as np
from PIL import Image

jobs = [
    ("2026-07-24 18-05-35.mkv", "frames_obs"),
    ("Timeline 1.mov", "frames_game"),
]

N = 10  # frames per video

for path, outdir in jobs:
    os.makedirs(outdir, exist_ok=True)
    try:
        meta = iio.immeta(path, plugin="pyav")
    except Exception:
        meta = {}
    # Read all frames lazily to count, but that can be heavy; instead grab via index props
    try:
        # get total via reading with a reader
        frames = iio.imread(path, plugin="pyav", index=None)
        total = frames.shape[0]
        print(f"{path}: total frames={total}, shape={frames.shape}")
        idxs = np.linspace(0, total - 1, N).astype(int)
        for k, fi in enumerate(idxs):
            img = frames[fi]
            im = Image.fromarray(img)
            # downscale to max width 900 to keep small
            w, h = im.size
            if w > 900:
                im = im.resize((900, int(h * 900 / w)))
            im.save(os.path.join(outdir, f"f{k:02d}_frame{fi}.png"))
        print(f"  wrote {len(idxs)} frames to {outdir}")
    except Exception as e:
        print(f"ERROR {path}: {e}")
