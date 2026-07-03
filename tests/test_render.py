"""Rendering tests (Pillow only): text grids, GIF re-encoding, HA previews."""
import importlib.util
import io
import os
import sys
import types

from PIL import Image

from xyaoled import text
from xyaoled.core import H, W
from xyaoled.gif import make_gif


def _load_ha_render():
    """Load the vendored HA render module without importing homeassistant."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pkg = types.ModuleType("_vend")
    pkg.__path__ = [os.path.join(root, "custom_components", "xyaoled")]
    sys.modules.setdefault("_vend", pkg)
    for name in ("protocol", "render"):
        if f"_vend.{name}" not in sys.modules:
            spec = importlib.util.spec_from_file_location(
                f"_vend.{name}",
                os.path.join(root, "custom_components", "xyaoled", f"{name}.py"),
            )
            mod = importlib.util.module_from_spec(spec)
            sys.modules[f"_vend.{name}"] = mod
            spec.loader.exec_module(mod)
    return sys.modules["_vend.render"]


ha_render = _load_ha_render()


def lit(grid):
    return sum(sum(row) for row in grid)


def test_render_centered_lights_pixels():
    g = text.render_centered("HI", 14, 0, 0, None)
    assert len(g) == H and len(g[0]) == W
    assert lit(g) > 10


def test_render_pages_splits_long_text():
    pages = text.render_pages("eins zwei drei vier fuenf sechs sieben acht", 14, None)
    assert len(pages) > 1
    assert all(lit(p) > 0 for p in pages)


def test_render_strip_is_multiple_of_panel_width():
    frames = text.render_strip("Lauftext etwas laenger", 14, None)
    assert len(frames) > 1
    assert all(len(f) == H and len(f[0]) == W for f in frames)
    assert lit(frames[0]) > 0


def test_font_cache_returns_same_object():
    assert text._load_font(14, None) is text._load_font(14, None)


def test_make_gif_static(tmp_path):
    p = tmp_path / "img.png"
    Image.new("RGB", (200, 100), (255, 0, 0)).save(p)
    data, n = make_gif(str(p), "contain")
    assert n == 1
    assert data.startswith(b"GIF89a")
    out = Image.open(io.BytesIO(data))
    assert out.size == (W, H)


def test_make_gif_animation_keeps_durations(tmp_path):
    p = tmp_path / "anim.gif"
    frames = [Image.new("RGB", (W, H), c) for c in ((255, 0, 0), (0, 255, 0), (0, 0, 255))]
    frames[0].save(p, save_all=True, append_images=frames[1:],
                   duration=[100, 200, 300], loop=0)
    data, n = make_gif(str(p), "contain")
    assert n == 3
    out = Image.open(io.BytesIO(data))
    durs = []
    for i in range(out.n_frames):
        out.seek(i)
        durs.append(out.info.get("duration"))
    assert durs == [100, 200, 300]


# ------------------------------------------------------------ HA preview images

def test_preview_png_scales_and_colours():
    grid = [[0] * W for _ in range(H)]
    grid[0][0] = 1
    png = ha_render.preview_png(grid, (0, 255, 0))
    img = Image.open(io.BytesIO(png))
    s = ha_render.PREVIEW_SCALE
    assert img.size == (W * s, H * s)
    assert img.getpixel((0, 0)) == (0, 255, 0)          # lit pixel, upscaled
    assert img.getpixel((s, 0)) == (0, 0, 0)            # neighbour stays dark


def test_preview_png_from_gif():
    buf = io.BytesIO()
    Image.new("RGB", (W, H), (10, 20, 30)).save(buf, format="GIF")
    png = ha_render.preview_png_from_gif(buf.getvalue())
    img = Image.open(io.BytesIO(png))
    assert img.size == (W * ha_render.PREVIEW_SCALE, H * ha_render.PREVIEW_SCALE)


def test_blank_preview_is_black():
    img = Image.open(io.BytesIO(ha_render.blank_preview_png()))
    assert img.convert("RGB").getpixel((5, 5)) == (0, 0, 0)
