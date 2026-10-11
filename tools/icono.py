# Dibuja el ícono de AmpBox: "AM" amarillo arriba y "PIA" blanco abajo (AMPIA),
# sobre fondo oscuro con borde amarillo para que se vea en la barra de tareas oscura.
#   resources/icon.png        grande, con la barra azul del loop abajo
#   resources/icon-small.png  16-32 px (barra de tareas, esquina de la ventana): sin barra
# Uso: python tools/icono.py
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

S = 1024 * 4                      # se dibuja grande y se achica (bordes suaves)
font_path = "C:/Windows/Fonts/seguibl.ttf"    # Segoe UI Black

bg, white, yellow, blue = (30, 32, 37, 255), (245, 245, 245, 255), (255, 196, 0, 255), (47, 129, 247, 255)


def base(margin, border):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    box = (margin * S, margin * S, (1 - margin) * S, (1 - margin) * S)
    d.rounded_rectangle(box, radius=0.2 * S, fill=yellow)                      # borde
    inner = tuple(v + (border * S if i < 2 else -border * S) for i, v in enumerate(box))
    d.rounded_rectangle(inner, radius=(0.2 - border) * S, fill=bg)
    return img, d


def fit(d, text, max_w, max_h):
    size = S
    while True:
        f = ImageFont.truetype(font_path, size)
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        if r - l <= max_w and b - t <= max_h:
            return f, (l, t, r, b)
        size -= S // 200


def draw_lines(d, lines, width):
    # Las dos líneas con el mismo tamaño de letra (el de la más ancha).
    f, _ = min((fit(d, text, width * S, h * S) for text, _, _, h in lines), key=lambda x: x[0].size)
    for text, color, cy, _ in lines:
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        d.text(((S - (r - l)) / 2 - l, cy * S - (t + b) / 2), text, font=f, fill=color)


out = Path(__file__).resolve().parent.parent / "resources" / "icon.png"
out.parent.mkdir(exist_ok=True)

# Grande: AM / PIA y la barra del loop.
img, d = base(0.02, 0.035)
draw_lines(d, (("AM", yellow, 0.26, 0.25), ("PIA", white, 0.56, 0.25)), 0.74)
bar_y = 0.78 * S
d.rounded_rectangle((0.16 * S, bar_y, 0.84 * S, bar_y + 0.06 * S), radius=0.03 * S, fill=(45, 48, 55, 255))
d.rounded_rectangle((0.16 * S, bar_y, 0.6 * S, bar_y + 0.06 * S), radius=0.03 * S, fill=blue)
d.rectangle((0.585 * S, bar_y - 0.015 * S, 0.61 * S, bar_y + 0.075 * S), fill=white)
img.resize((1024, 1024), Image.LANCZOS).save(out)
print(out)

# Chica: sin margen ni barra, letras lo más grandes posible.
img, d = base(0.0, 0.06)
draw_lines(d, (("AM", yellow, 0.30, 0.38), ("PIA", white, 0.71, 0.38)), 0.84)
small = out.with_name("icon-small.png")
img.resize((256, 256), Image.LANCZOS).save(small)
print(small)
