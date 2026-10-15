import sys
import time
import shutil
import string
from datetime import datetime

# 10 Balken mit unterschiedlichen Perioden
perioden = [
    ("2 s",    2.0),
    ("1 s",    1.0),
    ("500 ms", 0.5),
    ("100 ms", 0.1),
    ("50 ms",  0.05),
    ("10 ms",  0.01),
    ("1 ms",   0.001),
    ("100 us", 0.0001),
    ("10 us",  0.00001),
    ("1 us",   0.000001),
]

# Leuchtende Farben
farben = [
    (255, 255, 255),  # Weiß
    (255,  40,  40),  # Rot
    (  0, 160, 255),  # Blau
    (  0, 255,  70),  # Neongrün
    (255, 255,   0),  # Gelb
    (  0, 255, 255),  # Cyan
    (255,   0, 255),  # Magenta
    (255, 130,   0),  # Orange
    (220, 220, 220),  # Hellgrau / Weiß
    (100, 255, 100),  # Hellgrün
]

# 5x7 Font für große Buchstaben
FONT = {
    "A": ["01110","10001","10001","11111","10001","10001","10001"],
    "B": ["11110","10001","10001","11110","10001","10001","11110"],
    "C": ["01111","10000","10000","10000","10000","10000","01111"],
    "D": ["11110","10001","10001","10001","10001","10001","11110"],
    "E": ["11111","10000","10000","11110","10000","10000","11111"],
    "F": ["11111","10000","10000","11110","10000","10000","10000"],
    "G": ["01111","10000","10000","10111","10001","10001","01111"],
    "H": ["10001","10001","10001","11111","10001","10001","10001"],
    "I": ["11111","00100","00100","00100","00100","00100","11111"],
    "J": ["00111","00010","00010","00010","10010","10010","01100"],
    "K": ["10001","10010","10100","11000","10100","10010","10001"],
    "L": ["10000","10000","10000","10000","10000","10000","11111"],
    "M": ["10001","11011","10101","10101","10001","10001","10001"],
    "N": ["10001","11001","10101","10011","10001","10001","10001"],
    "O": ["01110","10001","10001","10001","10001","10001","01110"],
    "P": ["11110","10001","10001","11110","10000","10000","10000"],
    "Q": ["01110","10001","10001","10001","10101","10010","01101"],
    "R": ["11110","10001","10001","11110","10100","10010","10001"],
    "S": ["01111","10000","10000","01110","00001","00001","11110"],
    "T": ["11111","00100","00100","00100","00100","00100","00100"],
    "U": ["10001","10001","10001","10001","10001","10001","01110"],
    "V": ["10001","10001","10001","10001","10001","01010","00100"],
    "W": ["10001","10001","10001","10101","10101","10101","01010"],
    "X": ["10001","10001","01010","00100","01010","10001","10001"],
    "Y": ["10001","10001","01010","00100","00100","00100","00100"],
    "Z": ["11111","00001","00010","00100","01000","10000","11111"],
}

RESET = "\033[0m"
GELB_TEXT = "\033[93m"
GRUEN_TEXT = "\033[92m"
WEISS_TEXT = "\033[97m"
HINTERGRUND = "\033[48;2;20;20;20m"

def farbe_bg(rgb):
    r, g, b = rgb
    return f"\033[48;2;{r};{g};{b}m"

def grosser_buchstabe(buchstabe, scale_x=2, scale_y=1):
    """
    Erzeugt einen großen Buchstaben aus dem 5x7 Font.
    scale_x = horizontale Vergrößerung
    scale_y = vertikale Vergrößerung
    """
    muster = FONT.get(buchstabe, FONT["A"])
    zeilen = []

    for row in muster:
        line = ""
        for pixel in row:
            if pixel == "1":
                line += "█" * scale_x
            else:
                line += " " * scale_x
        for _ in range(scale_y):
            zeilen.append(line)

    return zeilen

start = time.perf_counter_ns()
alphabet = string.ascii_uppercase

# Alternate screen + Cursor aus
sys.stdout.write("\033[?1049h\033[?25l")
sys.stdout.flush()

try:
    while True:
        spalten, zeilen = shutil.get_terminal_size()

        label_breite = 9
        breite = max(10, spalten - label_breite - 3)

        jetzt = time.perf_counter_ns()
        t = (jetzt - start) / 1e9
        uhr = datetime.now().strftime("%H:%M:%S.%f")

        # Alphabet: 20 ms pro Buchstabe
        buchstaben_index = (jetzt - start) // 20_000_000
        buchstabe = alphabet[buchstaben_index % 26]

        ausgabe = []
        ausgabe.append("\033[H")
        ausgabe.append(f"{GELB_TEXT}KAMERA SYNC TEST   {uhr}{RESET}\n\n")

        # Balken
        for i, (name, periode) in enumerate(perioden):
            phase = (t / periode) % 1.0

            # Dreieckfunktion: 0 -> 1 -> 0
            position = 1.0 - abs(2.0 * phase - 1.0)

            if i == 0:
                # Oberster Balken: beweglicher Block
                blockbreite = max(3, breite // 18)
                blockbreite = min(blockbreite, breite)
                links = round(position * (breite - blockbreite))

                balken = (
                    HINTERGRUND + " " * links +
                    farbe_bg(farben[i]) + " " * blockbreite +
                    HINTERGRUND + " " * (breite - links - blockbreite) +
                    RESET
                )
            else:
                # Alle anderen: wachsen und schrumpfen
                laenge = round(position * (breite - 1)) + 1
                laenge = max(1, min(laenge, breite))

                balken = (
                    farbe_bg(farben[i]) + " " * laenge +
                    HINTERGRUND + " " * (breite - laenge) +
                    RESET
                )

            ausgabe.append(f"{name:>7}  {balken}\n")
            ausgabe.append("\n")  # genau eine Leerzeile Abstand

        # Alphabet-Bereich
        ausgabe.append(f"{GELB_TEXT}Alphabet: 20 ms Rhythmus{RESET}\n\n")

        # Größe des Buchstabens dynamisch an Terminalhöhe/-breite anpassen
        # Standard: etwas größer
        scale_x = 4
        scale_y = 2

        if spalten < 60:
            scale_x = 2
            scale_y = 1
        elif spalten < 90:
            scale_x = 3
            scale_y = 1

        if zeilen < 32:
            scale_y = 1

        for line in grosser_buchstabe(buchstabe, scale_x=scale_x, scale_y=scale_y):
            ausgabe.append(f"{GRUEN_TEXT}{line}{RESET}\n")

        sys.stdout.write("".join(ausgabe))
        sys.stdout.flush()

        # Anzeige-Update
        time.sleep(0.005)

except KeyboardInterrupt:
    pass

finally:
    # Cursor wieder an + Alternate screen verlassen
    sys.stdout.write("\033[0m\033[?25h\033[?1049l")
    sys.stdout.flush()

