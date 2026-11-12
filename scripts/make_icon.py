"""Собирает res/Tryflake.ico из PNG-значка (нужен Pillow). Запуск: python scripts/make_icon.py <icon.png>"""
import sys
from pathlib import Path
from PIL import Image

source = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/assets/icon.png")
target = Path(__file__).resolve().parent.parent / "res" / "Tryflake.ico"
image = Image.open(source).convert("RGBA")
image.save(target, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
print(f"Written {target}")
