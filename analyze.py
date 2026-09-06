from PIL import Image
import numpy as np
from scipy import ndimage
from scipy.ndimage import binary_dilation

arr = np.array(Image.open("bin/biome_mega.bmp"))
OCEAN = [20,60,140]
LAND  = [60,160,60]
LAKE  = [40,100,180]

# Land = land + lakes (lakes are on land, not coast)
land = np.all(arr == LAND, axis=2) | np.all(arr == LAKE, axis=2)
total = arr.shape[0] * arr.shape[1]
pct = np.sum(land) / total * 100
print(f"Land+lakes coverage: {pct:.1f}%")

lbl, n = ndimage.label(land)
sizes = np.bincount(lbl.ravel())[1:]
n_micro = np.sum(sizes < 50)
n_medium = np.sum((sizes >= 50) & (sizes < 500))
n_large = np.sum(sizes >= 500)
print(f"Components: {n} total  |  micro(<50px): {n_micro}  |  medium(50-500): {n_medium}  |  large(500+): {n_large}")

if n_large > 0:
    large_indices = np.argsort(sizes)[-min(5, len(sizes)):] + 1
    print(f"\nTop {len(large_indices)} continents by size:")
    for rank, label_id in enumerate(reversed(large_indices)):
        mask = lbl == label_id
        area = np.sum(mask)
        # Fill interior holes so perimeter only counts outer coast
        filled = ndimage.binary_fill_holes(mask)
        eroded = ndimage.binary_erosion(filled, structure=np.ones((3,3)))
        perimeter = np.sum(filled & ~eroded)
        if perimeter > 0:
            iq = 4 * np.pi * area / (perimeter * perimeter)
            print(f"  #{rank+1}: area={area}px, outer_perim={perimeter}px, IQ={iq:.4f}")
        else:
            print(f"  #{rank+1}: degenerate")

# Coast: outer boundary of filled land (so lake/river edges don't pollute)
filled_land = ndimage.binary_fill_holes(land)
coast = filled_land & ~ndimage.binary_erosion(filled_land, structure=np.ones((3,3)))
ys, xs = np.where(coast)
sample_n = min(5000, len(xs))
if sample_n > 0:
    rng = np.random.RandomState(42)
    idx = rng.choice(len(xs), sample_n, replace=False)
    nbrs = []
    for i in idx:
        y0, x0 = ys[i], xs[i]
        patch = arr[max(0,y0-1):y0+2, max(0,x0-1):x0+2]
        ocean_nbrs = np.sum(np.all(patch == OCEAN, axis=2))
        nbrs.append(ocean_nbrs)
    print(f"\nCoast exposure: {np.mean(nbrs):.2f} ocean neighbours (sampled {sample_n})")

ocean = np.all(arr == OCEAN, axis=2)
ocean_lbl, ocean_n = ndimage.label(ocean)
ocean_sizes = np.bincount(ocean_lbl.ravel())[1:]
if ocean_n > 1:
    bg_label = np.argmax(ocean_sizes) + 1
    hole_mask = (ocean_lbl != bg_label) & (ocean_lbl > 0)
    hole_px = np.sum(hole_mask)
    border_mask = np.zeros_like(ocean, dtype=bool)
    border_mask[0,:] = border_mask[-1,:] = True
    border_mask[:,0] = border_mask[:,-1] = True
    interior_holes = hole_px - np.sum(hole_mask & border_mask)
    interior_hole_pct = interior_holes / total * 100
    print(f"Interior ocean holes: {interior_holes}px ({interior_hole_pct:.2f}% of total)")
else:
    print("Interior ocean holes: N/A")

# Fractal dimension on outer coast only
bs_list = [2, 4, 8, 16, 32]
cnt = []
for b in bs_list:
    h, w = coast.shape
    nh, nw = h // b, w // b
    cnt.append(coast[:nh*b, :nw*b].reshape(nh, b, nw, b).any(axis=(1,3)).sum())
if len(cnt) > 1:
    coeffs = np.polyfit(np.log(bs_list), np.log(cnt), 1)
    fd = -coeffs[0]
    print(f"Coast fractal dimension: {fd:.3f}")

ocean_near_land = binary_dilation(land, structure=np.ones((3,3))) & ocean
shelf_pct = np.sum(ocean_near_land) / total * 100
print(f"Shelf presence (ocean within 2px of land): {shelf_pct:.1f}%")
