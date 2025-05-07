import sys
import tkinter as tk
from tkinter.scrolledtext import ScrolledText
from PIL import Image, ImageTk
import threading
import serial
import time
import cv2
import numpy as np

# Konfiguration (anpassen!)
COM_PORT = "COM8"          # Serieller Port (USB) deines ESPCams
BAUDRATE = 921600          # Muss mit der ESPCam-Firmware übereinstimmen
# Bei ESPCam mit QQVGA: 160 x 120, RGB565 (2 Byte pro Pixel)
CAM_WIDTH = 160
CAM_HEIGHT = 120
FRAME_SIZE = CAM_WIDTH * CAM_HEIGHT * 2  # 38400 Bytes

# --- GUI-Aufbau via Tkinter ---
root = tk.Tk()
root.title("RC Controller - SerialCam (ESPCam)")
root.geometry("800x600")

# Label für Kamerastream (oben)
camera_label = tk.Label(root)
camera_label.pack(side=tk.TOP, padx=5, pady=5)

# ScrolledText für Konsole (unten)
console = ScrolledText(root, height=10)
console.pack(side=tk.BOTTOM, fill=tk.BOTH, expand=True)

# Umleitung von print-Ausgaben in das Konsolen-Widget
class ConsoleRedirector:
    def __init__(self, widget):
        self.widget = widget
    def write(self, msg):
        self.widget.insert(tk.END, msg)
        self.widget.see(tk.END)
    def flush(self):
        pass

sys.stdout = ConsoleRedirector(console)

# --- Serielle Schnittstelle öffnen ---
ser = None  # globale Variable initialisieren
try:
    ser = serial.Serial(COM_PORT, BAUDRATE, timeout=2)
    print(f"Serielle Verbindung auf {COM_PORT} geöffnet.")
except Exception as e:
    print("Fehler beim Öffnen der seriellen Schnittstelle:", e)
    # GUI wird trotzdem gestartet, auch wenn die serielle Verbindung fehlt.

def read_serial_stream():
    global ser
    if ser is None:
        print("Serielle Schnittstelle ist nicht verfügbar.")
        return
    while True:
        try:
            # Debug: Zeige, wie viele Bytes aktuell im Puffer liegen
            available = ser.in_waiting
            print(f"{available} Bytes im Puffer verfügbar")
            
            # Lese den Header, den der ESPCam sendet:
            # Erwartetes Format: "FRAME_START 38400\n"
            header_line = ser.readline().decode('utf-8', errors='ignore').strip()
            print(f"Header empfangen: '{header_line}'")
            if not header_line.startswith("FRAME_START"):
                if header_line:
                    print("Unerwarteter Header:", header_line)
                continue

            # Extrahiere – nach einem Leerzeichen – die übermittelte Framegröße
            parts = header_line.split()
            if len(parts) < 2:
                print("Header unvollständig:", header_line)
                continue
            try:
                expected_length = int(parts[1])
            except Exception as e:
                print("Fehler beim Parsen des Headers:", header_line, e)
                continue

            if expected_length != FRAME_SIZE:
                print(f"Warnung: Erwartete Framegröße {FRAME_SIZE} Bytes, Header liefert {expected_length} Bytes.")

            # Lese exakt die Bilddaten (RGB565)
            frame_data = ser.read(expected_length)
            if len(frame_data) != expected_length:
                print("Unvollständiges Frame empfangen.")
                continue

            # Lese das eventuell nachfolgende Newline der Bilddaten (kann auch leer sein)
            ser.readline()
            
            # Lese den Trailer, der "FRAME_END" lauten sollte
            trailer_line = ser.readline().decode('utf-8', errors='ignore').strip()
            print(f"Trailer empfangen: '{trailer_line}'")
            if trailer_line != "FRAME_END":
                print("Unerwarteter Trailer:", trailer_line)
                continue

            print("Frame empfangen.")

            # Konvertiere das empfangene RGB565-Bild in ein Tkinter-fähiges Bild:
            # Zuerst in ein numpy-Array (16 Bit) mit der richtigen Form
            rgb565 = np.frombuffer(frame_data, dtype=np.uint16).reshape((CAM_HEIGHT, CAM_WIDTH))
            # Extrahiere R, G, B-Komponenten:
            r = ((rgb565 >> 11) & 0x1F) << 3
            g = ((rgb565 >> 5) & 0x3F) << 2
            b = (rgb565 & 0x1F) << 3
            # Erzeuge ein 3-Kanal-Bild im 8-Bit-Format:
            rgb888 = np.dstack((r, g, b)).astype(np.uint8)
            # Bei PIL wird ein RGB-Bild erwartet – hier ist eventuell keine Umrechnung nötig,
            # da rgb888 bereits die richtigen Farbwerte enthält.
            pil_img = Image.fromarray(rgb888, 'RGB')
            imgtk = ImageTk.PhotoImage(image=pil_img)

            # Aktualisiere den Inhalt vom Kamera-Label im Tkinter-Hauptthread
            def update_image():
                camera_label.config(image=imgtk)
                camera_label.image = imgtk  # Referenz sichern, damit das Bild angezeigt wird
            root.after(0, update_image)

        except Exception as e:
            print("Fehler beim Lesen des Kamerastreams:", e)
            time.sleep(1)

# Starte einen separaten Thread zum Auslesen des seriellen Datenstroms
serial_thread = threading.Thread(target=read_serial_stream, daemon=True)
serial_thread.start()

# Starte den Tkinter-Hauptloop
root.mainloop()