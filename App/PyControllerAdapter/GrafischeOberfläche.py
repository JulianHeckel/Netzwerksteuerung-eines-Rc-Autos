import sys
import tkinter as tk
from tkinter.scrolledtext import ScrolledText
from PIL import Image, ImageTk
import asyncio
import threading
import cv2
import numpy as np

# Konfiguration (anpassen!)
esp_ip = "192.168.1.43"  # IP-Adresse des ESP
CAM_WIDTH = 160
CAM_HEIGHT = 120
FRAME_SIZE = CAM_WIDTH * CAM_HEIGHT * 2  # RGB565: 2 Byte pro Pixel

# --- GUI-Aufbau via Tkinter ---
root = tk.Tk()
root.title("RC Controller - GUI")
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

# --- Asynchroner Task: Kamerastream empfangen und im GUI aktualisieren ---
async def receive_cam_stream():
    while True:
        try:
            print("Verbinde zum Kamerastream-Server...")
            reader, writer = await asyncio.open_connection(esp_ip, 5001)
            print("Kamerastream TCP-Verbindung aufgebaut.")
            # Lese (und verwerfe) Header "FRAME_START\n"
            header = await reader.readline()
            if b"FRAME_START" not in header:
                print("Unerwarteter Header:", header)
                writer.close()
                await writer.wait_closed()
                continue

            # Lese exakt FRAME_SIZE Bytes (das Kameraframe)
            frame_data = await reader.readexactly(FRAME_SIZE)
            # Lies den Trailer (z. B. "FRAME_END\n")
            trailer = await reader.readline()
            print("Frame empfangen, Trailer:", trailer)
            writer.close()
            await writer.wait_closed()

            # Konvertiere aus RGB565 zu einem BGR-Bild (für OpenCV)
            rgb565 = np.frombuffer(frame_data, dtype=np.uint16).reshape((CAM_HEIGHT, CAM_WIDTH))
            r = ((rgb565 >> 11) & 0x1F) << 3
            g = ((rgb565 >> 5)  & 0x3F) << 2
            b = (rgb565 & 0x1F) << 3
            rgb888 = np.dstack((r, g, b)).astype(np.uint8)
            bgr = cv2.cvtColor(rgb888, cv2.COLOR_RGB2BGR)
            
            # Konvertiere das Bild für Tkinter (BGR -> RGB -> PIL Image -> ImageTk.PhotoImage)
            img = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            pil_img = Image.fromarray(img)
            imgtk = ImageTk.PhotoImage(image=pil_img)
            
            # Aktualisiere das Label im Hauptthread
            camera_label.config(image=imgtk)
            camera_label.image = imgtk  # Referenz speichern, damit das Bild nicht gelöscht wird

        except Exception as e:
            print("Fehler beim Empfangen des Kamerastreams:", e)
            await asyncio.sleep(1)

# --- Integration von asyncio und Tkinter ---
# Wir starten einen separaten Thread für den asyncio-Eventloop.
def start_asyncio_loop(loop):
    asyncio.set_event_loop(loop)
    loop.run_forever()

new_loop = asyncio.new_event_loop()
t = threading.Thread(target=start_asyncio_loop, args=(new_loop,), daemon=True)
t.start()

# Starte den asynchronen Task im asyncio-Loop
asyncio.run_coroutine_threadsafe(receive_cam_stream(), new_loop)

# Starte den Tkinter-Hauptloop
root.mainloop()