"""Measure camera-space carryover in a four-photograph, opposite-history test."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

from make_book import COLS, ROWS, tile_signs

SAMPLES = 16  # Equal-area samples well inside each tile.


def corners(value):
    try:
        points = np.array([list(map(float, p.split(","))) for p in value.split()])
        if points.shape != (4, 2) or not np.isfinite(points).all():
            raise ValueError
    except ValueError:
        raise argparse.ArgumentTypeError("Enter four x,y pairs in TL TR BR BL order") from None
    return points


def project(points, coords):
    """Map unit-square sample points to pixel centers on a planar photograph."""
    square = ((0, 0), (1, 0), (1, 1), (0, 1))
    matrix, vector = [], []
    for (u, v), (x, y) in zip(square, points):
        matrix.extend(([u, v, 1, 0, 0, 0, -x*u, -x*v], [0, 0, 0, u, v, 1, -y*u, -y*v]))
        vector.extend((x, y))
    h = np.append(np.linalg.solve(matrix, vector), 1).reshape(3, 3)
    xy = coords @ h.T
    return xy[..., :2] / xy[..., 2:]


def sample(path, points, text_mode=False):
    with Image.open(path) as im:
        data = np.asarray(ImageOps.exif_transpose(im).convert("L"))
    # Reject a mirrored, twisted, out-of-frame or undersampled crop.
    edge = np.roll(points, -1, axis=0) - points
    following = np.roll(edge, -1, axis=0)
    if (np.any(edge[:,0]*following[:,1] - edge[:,1]*following[:,0] <= 0)
            or (points < 0).any() or (points > (data.shape[1]-1, data.shape[0]-1)).any()
            or np.min(np.linalg.norm(edge, axis=1) / np.array([COLS, ROWS, COLS, ROWS])) < 10):
        raise ValueError(f"Bad grid corners, orientation or resolution in {path}")
    inside = np.linspace(.25, .75, SAMPLES)
    u = ((np.arange(COLS)[:, None] + inside) / COLS).ravel()
    v = ((np.arange(ROWS)[:, None] + inside) / ROWS).ravel()
    if text_mode:
        # Sample at the X4 Pro logical pixel scale, excluding status bars and bezel.
        u = (np.arange(432) + 24.5) / 480
        v = (np.arange(720) + 40.5) / 800
    xx, yy = np.meshgrid(u, v)
    coords = project(points, np.stack((xx, yy, np.ones_like(xx)), axis=-1))
    x, y = coords[..., 0], coords[..., 1]
    x0, y0 = x.astype(int), y.astype(int)
    dx, dy = x-x0, y-y0
    patch = ((1-dx)*(1-dy)*data[y0, x0] + dx*(1-dy)*data[y0, x0+1]
             + dy*(1-dx)*data[y0+1, x0] + dx*dy*data[y0+1, x0+1])
    if text_mode:
        return patch
    return patch.reshape(ROWS, SAMPLES, COLS, SAMPLES).transpose(0,2,1,3)


def blocks(values):
    """Per-block diagonal differences cancel constant and linear shading."""
    return .25*(values[::2,::2] + values[1::2,1::2] - values[::2,1::2] - values[1::2,::2]).ravel()


def score(a, after_a, b, after_b):
    signs = np.array(tile_signs())
    before = a - b
    after = after_a - after_b
    pre = signs*blocks(before.mean(axis=(2,3)))
    post = signs*blocks(after.mean(axis=(2,3)))
    scale = np.median(pre)
    if abs(scale) < 35 or (pre / scale < .4).any():
        raise ValueError("Both predecessor photos must contain opposite patterns aligned to the selected grid")
    # The same page must follow both sources. Reject residual differences too
    # large to attribute to the measured pattern (motion, changing wallpaper).
    ratio = np.median(post/pre)
    unexplained = after - ratio*before
    offset = np.median(unexplained)
    mismatch = np.quantile(np.abs(unexplained-offset), .90) / abs(scale)
    if mismatch > .12 or abs(offset) > .12*abs(scale):
        raise ValueError("Targets differ in artwork, position, lighting or exposure; repeat under fixed conditions")
    # Placebo orientations are balanced over blocks, to avoid projecting any
    # constant measured carryover into the null score.
    null_signs = np.array([1, -1]*(len(pre)//2))
    np.random.default_rng(9364).shuffle(null_signs)
    ratios = post/pre
    low, high = np.quantile(ratios, [.25,.75])
    return {
        "residual_percent_of_camera_black_white_contrast": round(float(100*ratio),3),
        "signed_placebo_percent_mean": round(float(100*np.mean(ratios*null_signs)),3),
        "block_interquartile_percent": [round(float(100*x),3) for x in [low,high]],
        "unexplained_target_difference_p90_percent": round(float(100*mismatch),3),
        "source_contrast_jpeg_luma_levels": round(float(abs(scale)),2),
        "blocks": len(pre),
        "per_block_percent": [round(float(100*x),3) for x in ratios],
    }


def score_text(a, after_a, b, after_b, dark=False):
    """Compare old text strokes in areas where the destination is background."""
    # Work in positive-paper polarity in both display modes.
    if dark:
        a, after_a, b, after_b = (255. - im for im in (a, after_a, b, after_b))
    pre, post = a-b, after_a-after_b
    contrast = np.quantile(abs(pre), .985)
    if contrast < 40:
        raise ValueError("Insufficient distinct text in the two source pages or a low-contrast photo")
    ratios = []
    for y in range(0, pre.shape[0], 48):
        for x in range(0, pre.shape[1], 48):
            region = np.s_[y:y+48,x:x+48]
            # Both final photos should show the local paper tone. Reject current
            # glyphs so a slight camera shift cannot turn their edges into ghosts.
            paper = np.ones(pre[region].shape, dtype=bool)
            for frame in (after_a,after_b):
                tile = frame[region]
                paper &= tile >= np.quantile(tile,.85)-.18*contrast
            src, dst = pre[region], post[region]
            pos = paper & (src > .6*contrast)
            neg = paper & (src < -.6*contrast)
            if min(pos.sum(),neg.sum()) < 12:
                continue
            ratios.append((dst[pos].mean()-dst[neg].mean())/(src[pos].mean()-src[neg].mean()))
    if len(ratios) < 12:
        raise ValueError("Too few background regions with distinct old text strokes; use denser pages or check registration and display mode")
    frac=np.median(ratios)
    noise=post-frac*pre
    shift=np.median(noise)
    # A changed or moved text page gives large changes concentrated on strokes.
    mismatch=np.quantile(abs(noise-shift),.965)/contrast
    if mismatch > .24 or abs(shift) > .12*contrast:
        raise ValueError("Final text pages or exposures differ; use the same page and keep camera fixed")
    return {
        "text_residual_percent_of_camera_black_white_contrast": round(float(100*frac),3),
        "block_interquartile_percent": [round(float(100*x),3) for x in np.quantile(ratios,[.25,.75])],
        "usable_text_regions": len(ratios),
        "unexplained_target_difference_p96_5_percent": round(float(100*mismatch),3),
        "source_contrast_jpeg_luma_levels": round(float(contrast),2),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for label in ["pattern_a","target_after_a","pattern_b","target_after_b"]:
        p.add_argument(label, type=Path)
    p.add_argument("--text", action="store_true", help="Measure ordinary text pages using the four corners of the whole panel")
    p.add_argument("--dark", action="store_true", help="Use light text on dark paper in --text mode")
    p.add_argument("--corners", type=corners, required=True, help="Four outer checker corners in pattern A, TL TR BR BL, x,y pairs in portrait-corrected pixels. Use quoted space-separated pairs.")
    p.add_argument("--corners-b", type=corners, help="Use if camera moved between the A and B runs. Each predecessor/target pair must remain fixed.")
    args=p.parse_args()
    try:
        groups = [sample(path, points, args.text) for path,points in [
            (args.pattern_a,args.corners), (args.target_after_a,args.corners),
            (args.pattern_b,args.corners if args.corners_b is None else args.corners_b),
            (args.target_after_b,args.corners if args.corners_b is None else args.corners_b)]]
        result = score_text(*groups, dark=args.dark) if args.text else score(*groups)
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, np.linalg.LinAlgError) as exc:
        p.exit(2, f"Cannot score this set. {exc}\n")


if __name__ == "__main__":
    main()
