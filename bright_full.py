from PIL import Image, ImageEnhance
import os
os.makedirs("bright_obs", exist_ok=True)
for name in ["obs_01","obs_06","obs_09","obs_12"]:
    p = f"frames_obs/{name}.png"
    if not os.path.exists(p): continue
    im = Image.open(p).convert("RGB")
    im = ImageEnhance.Brightness(im).enhance(2.4)
    im = ImageEnhance.Contrast(im).enhance(1.1)
    im.save(f"bright_obs/{name}_b.png")
    print("wrote", name)
print("done")
