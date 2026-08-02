from PIL import Image, ImageEnhance
import os

# Crop region around the vine bridge (in the 760-wide OBS frames), then brighten + upscale.
box = (430, 55, 620, 230)  # left, top, right, bottom
os.makedirs("crop_obs", exist_ok=True)
for name in ["obs_01","obs_03","obs_05","obs_07","obs_08","obs_09","obs_10","obs_11","obs_12"]:
    p = f"frames_obs/{name}.png"
    if not os.path.exists(p):
        continue
    im = Image.open(p).convert("RGB").crop(box)
    im = ImageEnhance.Brightness(im).enhance(2.2)
    im = ImageEnhance.Contrast(im).enhance(1.15)
    w,h = im.size
    im = im.resize((w*4, h*4), Image.LANCZOS)
    im.save(f"crop_obs/{name}_crop.png")
    print("wrote", name)
print("done")
