"""Gera o ícone da abelha a partir de uma única lista de formas, em dois formatos:

  icons/bee.svg                                     ícone do computador + logo no app
  android/res/drawable/ic_launcher_foreground.xml   ícone adaptável do Android
  android/res/drawable/ic_notification.xml          ícone da notificação (silhueta branca)

Os vetores do Android só aceitam caminhos simples (sem círculos, elipses ou
transformações), então cada forma é escrita aqui como caminho e os dois
arquivos saem dela. Edite as formas abaixo e rode:  python icons/generate_icons.py
"""

import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INK = "#2B2118"         # contornos, cabeça, listras
BODY_COLOR = "#FFC83A"  # amarelo mel
WING = "#DCEEFF"        # azul claro
BACKGROUND = "#FFF1C9"  # creme (quadrado arredondado no computador, fundo adaptável no Android)


def circle(cx, cy, r):
    return (f"M{cx - r:g},{cy:g} A{r:g},{r:g} 0 1,0 {cx + r:g},{cy:g} "
            f"A{r:g},{r:g} 0 1,0 {cx - r:g},{cy:g} Z")


def ellipse(cx, cy, rx, ry, rotation=0.0):
    a = math.radians(rotation)
    dx, dy = rx * math.cos(a), rx * math.sin(a)
    x1, y1, x2, y2 = cx - dx, cy - dy, cx + dx, cy + dy
    return (f"M{x1:.2f},{y1:.2f} A{rx:g},{ry:g} {rotation:g} 1,0 {x2:.2f},{y2:.2f} "
            f"A{rx:g},{ry:g} {rotation:g} 1,0 {x1:.2f},{y1:.2f} Z")


BODY = (35, 39, 16, 12)  # elipse do corpo: cx, cy, rx, ry
BODY_PATH = ellipse(*BODY)


def body_stripe(x1, x2):
    """A parte da elipse do corpo entre x1 e x2, como caminho exato
    (o SVG do Qt ignora recortes, então as listras não podem ser só recortadas)."""
    cx, cy, rx, ry = BODY

    def half_height(x):
        return ry * math.sqrt(max(0.0, 1 - ((x - cx) / rx) ** 2))

    t1, t2 = half_height(x1), half_height(x2)
    return (f"M{x1:g},{cy - t1:.2f} A{rx:g},{ry:g} 0 0,1 {x2:g},{cy - t2:.2f} "
            f"L{x2:g},{cy + t2:.2f} A{rx:g},{ry:g} 0 0,1 {x1:g},{cy + t1:.2f} Z")


# (caminho, preenchimento, contorno, espessura) - pintadas nesta ordem, área de 64x64.
SHAPES = [
    (ellipse(28, 24, 8, 12, -25), WING, INK, 2),               # asa de trás
    (ellipse(40, 25, 7, 11, 20), WING, INK, 2),                # asa da frente
    (BODY_PATH, BODY_COLOR, None, 0),                          # corpo
    (body_stripe(28, 33) + " " + body_stripe(38, 43), INK, None, 0),  # listras
    (BODY_PATH, None, INK, 2.5),                               # contorno do corpo
    ("M50,36 L57.5,39 L50,42 Z", INK, None, 0),                # ferrão
    ("M17,30 C15,25 12,22.5 8.5,21.5", None, INK, 2),          # antenas
    ("M20,30.5 C20.5,25.5 19,22 16,19.5", None, INK, 2),
    (circle(17, 37, 8), INK, None, 0),                         # cabeça
    (circle(14.2, 35, 2.2), "#FFFFFF", None, 0),               # olho
]


def svg():
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<!-- Gerado por icons/generate_icons.py - edite as formas lá. -->",
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">',
        '  <path d="M14,0 H50 A14,14 0 0 1 64,14 V50 A14,14 0 0 1 50,64 H14 '
        f'A14,14 0 0 1 0,50 V14 A14,14 0 0 1 14,0 Z" fill="{BACKGROUND}"/>',
    ]
    for path, fill, stroke, width in SHAPES:
        attrs = [f'd="{path}"', f'fill="{fill or "none"}"']
        if stroke:
            attrs += [f'stroke="{stroke}"', f'stroke-width="{width:g}"',
                      'stroke-linecap="round"', 'stroke-linejoin="round"']
        parts.append(f'  <path {" ".join(attrs)}/>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def vector_drawable():
    # Frente do ícone adaptável: área de 108dp, da qual só os 66dp centrais
    # aparecem sempre, então o desenho de 64 unidades é encaixado nessa zona.
    scale = 66 / 64
    parts = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Gerado por icons/generate_icons.py - edite as formas lá. -->",
        '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
        '    android:width="108dp" android:height="108dp"',
        '    android:viewportWidth="108" android:viewportHeight="108">',
        f'    <group android:translateX="21" android:translateY="21"'
        f' android:scaleX="{scale:.4f}" android:scaleY="{scale:.4f}">',
    ]
    for path, fill, stroke, width in SHAPES:
        attrs = [f'android:pathData="{path}"']
        if fill:
            attrs.append(f'android:fillColor="{fill}"')
        if stroke:
            attrs += [f'android:strokeColor="{stroke}"', f'android:strokeWidth="{width:g}"',
                      'android:strokeLineCap="round"', 'android:strokeLineJoin="round"']
        parts.append(f'        <path {" ".join(attrs)}/>')
    parts += ["    </group>", "</vector>"]
    return "\n".join(parts) + "\n"


# Notificação: o Android pinta só a silhueta (tudo que não é transparente
# fica branco), então listras e olho viram furos (evenOdd) no corpo e na
# cabeça, e as asas e antenas ficam sem contorno.
NOTIFICATION_SHAPES = [
    (ellipse(28, 24, 8, 12, -25), False, None),
    (ellipse(40, 25, 7, 11, 20), False, None),
    (BODY_PATH + " " + body_stripe(28, 33) + " " + body_stripe(38, 43), True, None),
    ("M50,36 L57.5,39 L50,42 Z", False, None),
    ("M17,30 C15,25 12,22.5 8.5,21.5", None, 2.5),
    ("M20,30.5 C20.5,25.5 19,22 16,19.5", None, 2.5),
    (circle(17, 37, 8) + " " + circle(14.2, 35, 2.2), True, None),
]


def notification_drawable():
    parts = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Gerado por icons/generate_icons.py - edite as formas lá. -->",
        '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
        '    android:width="24dp" android:height="24dp"',
        '    android:viewportWidth="64" android:viewportHeight="64">',
    ]
    for path, even_odd, stroke_width in NOTIFICATION_SHAPES:
        attrs = [f'android:pathData="{path}"']
        if stroke_width:
            attrs += ['android:strokeColor="#FFFFFF"', f'android:strokeWidth="{stroke_width:g}"',
                      'android:strokeLineCap="round"']
        else:
            attrs.append('android:fillColor="#FFFFFF"')
            if even_odd:
                attrs.append('android:fillType="evenOdd"')
        parts.append(f'    <path {" ".join(attrs)}/>')
    parts.append("</vector>")
    return "\n".join(parts) + "\n"


def main():
    (ROOT / "icons" / "bee.svg").write_text(svg(), encoding="utf-8")
    drawables = ROOT / "android" / "res" / "drawable"
    drawables.mkdir(parents=True, exist_ok=True)
    (drawables / "ic_launcher_foreground.xml").write_text(vector_drawable(), encoding="utf-8")
    (drawables / "ic_notification.xml").write_text(notification_drawable(), encoding="utf-8")
    print("gerados icons/bee.svg,", drawables.relative_to(ROOT) / "ic_launcher_foreground.xml",
          "e", drawables.relative_to(ROOT) / "ic_notification.xml")


if __name__ == "__main__":
    main()
