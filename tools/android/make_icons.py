"""Generate Android launcher icons from favicon.ico (adaptive starfield background + art foreground, legacy square/round PNGs)."""
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / "android" / "app" / "src" / "main" / "res"
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
# Starfield drawn once at 108dp @ 4x and scaled down, so every density shows the same stars.
FIELD_PX = 432


def load_art() -> Image.Image:
    ico = Image.open(ROOT / "favicon.ico")
    ico.size = max(ico.ico.sizes())
    ico.load()
    return ico.convert("RGBA")


def fit(art: Image.Image, box: int) -> Image.Image:
    art = art.copy()
    art.thumbnail((box, box), Image.Resampling.LANCZOS)
    return art


def starfield() -> Image.Image:
    """Deep-space backdrop: a navy center fading to black, small stars, and a few brighter ones with a soft glow."""
    n = FIELD_PX
    field = Image.new("RGB", (n, n), (0, 0, 0))
    grad = Image.radial_gradient("L").resize((n, n))
    navy = Image.new("RGB", (n, n), (14, 20, 48))
    field = Image.composite(field, navy, grad)
    rng = random.Random(64)
    stars = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    draw = ImageDraw.Draw(stars)
    for _ in range(160):
        x, y = rng.uniform(0, n), rng.uniform(0, n)
        r = rng.choice([0.8, 1.0, 1.2, 1.6])
        tint = rng.choice([(255, 255, 255), (200, 220, 255), (255, 240, 210)])
        draw.ellipse((x - r, y - r, x + r, y + r), fill=tint + (rng.randint(90, 230),))
    glow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    for _ in range(9):
        x, y = rng.uniform(0, n), rng.uniform(0, n)
        gdraw.ellipse((x - 7, y - 7, x + 7, y + 7), fill=(170, 200, 255, 110))
        draw.ellipse((x - 2.2, y - 2.2, x + 2.2, y + 2.2), fill=(255, 255, 255, 255))
    out = field.convert("RGBA")
    out.alpha_composite(glow.filter(ImageFilter.GaussianBlur(4)))
    out.alpha_composite(stars)
    return out


def main() -> None:
    art = load_art()
    field = starfield()
    for name, scale in DENSITIES.items():
        out = RES / f"mipmap-{name}"
        out.mkdir(parents=True, exist_ok=True)
        # Adaptive icon: 108dp layers, art inside the 66dp safe zone so launcher masks never clip it.
        size = round(108 * scale)
        field.resize((size, size), Image.Resampling.LANCZOS).save(out / "ic_launcher_background.png")
        fg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        a = fit(art, round(66 * scale))
        fg.alpha_composite(a, ((size - a.width) // 2, (size - a.height) // 2))
        fg.save(out / "ic_launcher_foreground.png")
        # Legacy 48dp icons for launchers without adaptive icon support: the same starfield, square and round.
        legacy = round(48 * scale)
        sq = field.resize((legacy, legacy), Image.Resampling.LANCZOS)
        a = fit(art, round(legacy * 0.86))
        sq.alpha_composite(a, ((legacy - a.width) // 2, (legacy - a.height) // 2))
        sq.save(out / "ic_launcher.png")
        mask = Image.new("L", (legacy, legacy), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, legacy - 1, legacy - 1), fill=255)
        rnd = Image.new("RGBA", (legacy, legacy), (0, 0, 0, 0))
        rnd.paste(sq, (0, 0), mask)
        rnd.save(out / "ic_launcher_round.png")
    print("icons written to", RES)


if __name__ == "__main__":
    main()
