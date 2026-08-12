#!/usr/bin/env python3
"""Convert a Minecraft resource pack into Farlands' texture pack format.

Farlands keys its texture array by ENGINE texture names (see
data/block_definitions.json), so a Minecraft pack must have its files
translated to those names before the game can load it.

Usage:
  python tools/pack_converter.py <pack_folder_or_zip> [--out <dir>]
                                 [--name <pack_name>] [--author <name>]

Examples:
  # Convert the unzipped folder into user://packs/pacp/  (run from repo root,
  # then point the game at the Windows user:// path, see README).
  python tools/pack_converter.py "C:\\path\\to\\PACP" --name pacp --out user_packs

Output layout (written into <out>/<name>/):
  pack.json                # name, schema, author, base_resolution
  textures/<engine_name>.png
  icon.png                 # copied from the source pack.png when present

Behaviour:
  * Only the engine's texture names (translation table below) are converted.
  * A name the pack does not provide is skipped with a note - the engine
    falls back to its built-in res://textures/blocks/<name>.png automatically.
  * base_resolution is detected from the converted PNGs (most common size,
    default 16). The engine resizes every layer to it with nearest-neighbour.
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import zipfile

# ---------------------------------------------------------------------------
# Engine texture name -> Minecraft file candidates (in priority order).
# Legacy (<1.13) names are listed after the modern ones for old packs.
# "cactus" covers the side face - the engine uses one texture per face name.
# ---------------------------------------------------------------------------
ENGINE_TO_MINECRAFT = {
    "stone": ["stone"],
    "dirt": ["dirt"],
    "grass_side": ["grass_block_side", "grass_side"],
    "grass_top": ["grass_block_top", "grass_top"],
    "water": ["water_still", "water_flow"],
    "wood_side": ["oak_log", "log_oak"],
    "wood_top": ["oak_log_top", "log_oak_top"],
    "leaves": ["oak_leaves", "leaves_oak"],
    "bedrock": ["bedrock"],
    "mud": ["mud"],
    "wet_sand": ["sand"],
    "snow": ["snow"],
    "gravel": ["gravel"],
    "cactus": ["cactus_side", "cactus_top"],
    "light_block": ["light_15", "light"],
}

# Emissive texture names have no Minecraft equivalent (they are Farlands-only).
# A pack that ships them (e.g. a Farlands-native pack) keeps the engine name.
EMISSIVE_NAMES = ["light_block_emit", "light_red_emit", "light_green_emit", "light_blue_emit"]


def png_size(path):
    """Width/height of a PNG via its IHDR chunk (no PIL dependency)."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(26)
        if len(hdr) < 26 or hdr[:8] != b"\x89PNG\r\n\x1a\n":
            return None
        return struct.unpack(">II", hdr[16:24])
    except OSError:
        return None


def sanitize_name(raw):
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", raw.strip())
    return name.strip("_").lower() or "pack"


def guess_author(description):
    """Pull an author out of a mcmeta description like '2.5.2_02 / By mzovjn'."""
    if not description:
        return None
    m = re.search(r"[Bb]y\s+([A-Za-z0-9_.-]+)", description)
    return m.group(1) if m else None


def find_block_texture_dir(root):
    candidate = os.path.join(root, "assets", "minecraft", "textures", "block")
    return candidate if os.path.isdir(candidate) else None


def extract_if_zip(path, tmp):
    if not zipfile.is_zipfile(path):
        return path
    out = os.path.join(tmp, "src")
    with zipfile.ZipFile(path) as z:
        z.extractall(out)
    return out


def convert(src_root, out_dir, pack_name, author_override):
    block_dir = find_block_texture_dir(src_root)
    if block_dir is None:
        print("ERROR: no assets/minecraft/textures/block found in the pack")
        sys.exit(1)

    pack_out = os.path.join(out_dir, pack_name)
    textures_out = os.path.join(pack_out, "textures")
    os.makedirs(textures_out, exist_ok=True)

    block_files = set(os.listdir(block_dir))
    converted = []  # (engine_name, minecraft_file)
    skipped = []

    for engine_name, candidates in ENGINE_TO_MINECRAFT.items():
        match = next((c for c in candidates if c + ".png" in block_files), None)
        if match is None:
            skipped.append(engine_name)
            continue
        src = os.path.join(block_dir, match + ".png")
        size = png_size(src)
        if size is None or size[0] == 0 or size[1] == 0:
            print(f"  skip {engine_name}: {match}.png is not a readable PNG")
            skipped.append(engine_name)
            continue
        # Copy and convert to RGBA8 format to avoid format mismatch errors
        try:
            from PIL import Image
            img = Image.open(src)
            if img.mode != 'RGBA':
                img = img.convert('RGBA')
            img.save(os.path.join(textures_out, engine_name + ".png"), 'PNG')
        except ImportError:
            # PIL not available, just copy
            shutil.copyfile(src, os.path.join(textures_out, engine_name + ".png"))
        converted.append((engine_name, match, size))

    for emissive in EMISSIVE_NAMES:
        if emissive + ".png" in block_files:
            shutil.copyfile(os.path.join(block_dir, emissive + ".png"),
                            os.path.join(textures_out, emissive + ".png"))
            converted.append((emissive, emissive, png_size(os.path.join(block_dir, emissive + ".png"))))

    # base_resolution: most common width among converted textures, else 16.
    sizes = [s[0] for _, _, s in converted if s is not None]
    base_resolution = max(set(sizes), key=sizes.count) if sizes else 16

    # pack.json + icon
    author = author_override
    if not author:
        try:
            meta = json.load(open(os.path.join(src_root, "pack.mcmeta"), encoding="utf-8"))
            author = guess_author(meta.get("pack", {}).get("description"))
        except (OSError, ValueError):
            pass
    with open(os.path.join(pack_out, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({
            "name": pack_name,
            "schema": 1,
            "min_supported": 1,
            "max_supported": 1,
            "base_resolution": base_resolution,
            "author": author or "",
        }, f, indent=2)

    pack_icon = os.path.join(src_root, "pack.png")
    if os.path.isfile(pack_icon):
        shutil.copyfile(pack_icon, os.path.join(pack_out, "icon.png"))

    print(f"Converted {len(converted)} textures -> {pack_out}")
    for engine_name, mc, size in converted:
        print(f"  {engine_name:14s} <- {mc}.png ({size[0]}x{size[1]})")
    if skipped:
        print(f"Skipped (pack has none; engine uses built-in): {', '.join(sorted(skipped))}")
    print(f"base_resolution: {base_resolution}  author: {author or '(none)'}")


def main():
    ap = argparse.ArgumentParser(description="Convert a Minecraft resource pack to Farlands format.")
    ap.add_argument("pack", help="path to the pack folder or .zip")
    ap.add_argument("--out", default="user_packs", help="output directory (default: user_packs)")
    ap.add_argument("--name", default=None, help="pack name (default: sanitized folder basename)")
    ap.add_argument("--author", default=None, help="pack author (default: parsed from pack.mcmeta)")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="packconv_")
    try:
        src_root = extract_if_zip(args.pack, tmp)
        basename = os.path.splitext(os.path.basename(args.pack))[0]
        pack_name = sanitize_name(args.name) if args.name else sanitize_name(basename)
        convert(src_root, args.out, pack_name, args.author)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
