import os
import json
import time
import asyncio
import pygame
import struct
import cv2
import numpy as np
import tkinter as tk
from PIL import Image, ImageTk

# Konfiguration
esp_ip = "sv02.tobicloud.eu"  # "192.168.1.43"
esp_port = 12346     # Steuerung: Port des TCP-Servers auf dem ESP
com_port = 12345     # Monitoring: Port des TCP-Servers für den ESP 
messages_per_second = 15  # Sendehäufigkeit
skill_level = 10          # 0 bis 100 %, Skalierungsfaktor
# Neue Ports für den Kamerastream
CAM_RECEIVE_PORT = 12346   # Kamerastream-empfang (sollte mit der Arduino-Weiterleitung übereinstimmen)
CAM_MONITOR_PORT = 12345   # Dummy Kamerastream-Monitoring

# Globale Flags zum Aktivieren/Deaktivieren
MONITORING_CONTROL_ENABLED = False     # Monitoring (Control) wie bisher
MONITORING_CAM_ENABLED = False         # Dummy Monitoring für Kamerastream (Port 12348)
CAM_RECEIVE_ENABLED = True             # Kamerastream empfangen (Port 12347)
CONTROL_SEND_ENABLED = True            # Steuerdaten senden aktivieren/deaktivieren

# Kamera-Konfiguration – muss mit den ESP-Einstellungen übereinstimmen:
CAM_WIDTH = 160
CAM_HEIGHT = 120
FRAME_SIZE = CAM_WIDTH * CAM_HEIGHT * 2  # RGB565: 2 Byte pro Pixel

# Datei zur Persistierung des Datenverbrauchs
DATA_USAGE_FILE = "data_usage.json"
data_bytes_sent = 0
data_bytes_received = 0

def load_data_usage():
    global data_bytes_sent, data_bytes_received
    if os.path.exists(DATA_USAGE_FILE):
        try:
            with open(DATA_USAGE_FILE, "r") as f:
                d = json.load(f)
            data_bytes_sent = d.get("bytes_sent", 0)
            data_bytes_received = d.get("bytes_received", 0)
            print(f"Geladene Verbrauchswerte: gesendet={data_bytes_sent}, empfangen={data_bytes_received}")
        except Exception as e:
            print("Fehler beim Laden der Verbrauchsdaten:", e)
    else:
        data_bytes_sent = 0
        data_bytes_received = 0

async def save_usage_loop():
    """Speichert alle 10 Sekunden den aktuellen Datenverbrauch in eine JSON-Datei."""
    global data_bytes_sent, data_bytes_received
    while True:
        try:
            with open(DATA_USAGE_FILE, "w") as f:
                json.dump({
                    "bytes_sent": data_bytes_sent,
                    "bytes_received": data_bytes_received
                }, f)
            # Optional: Gib den aktuellen Verbrauch aus
            print(f"Aktueller Gesamtverbrauch: gesendet={data_bytes_sent}, empfangen={data_bytes_received}")
        except Exception as e:
            print("Fehler beim Speichern der Verbrauchsdaten:", e)
        await asyncio.sleep(10)

# Laden der persistierten Verbrauchsdaten beim Start
load_data_usage()

# Initialisiere Pygame und Joystick
pygame.init()
pygame.joystick.init()
try:
    if pygame.joystick.get_count() == 0:
        raise Exception("Kein Joystick gefunden.")
    joystick = pygame.joystick.Joystick(0)
    joystick.init()
except Exception as e:
    print("Fehler beim Initialisieren des Joysticks:", e)
    exit(1)

def get_controller_inputs():
    pygame.event.pump()
    # Achsenwerte umrechnen
    lt_value = ((-joystick.get_axis(4) - 1) / 2) * 100
    rt_value = ((joystick.get_axis(5) + 1) / 2) * 100
    lsb_value = (joystick.get_axis(0) + 1) * 50
    scaling_factor = skill_level / 100.0
    lt_value *= scaling_factor
    rt_value *= scaling_factor
    lt_value = int(round(lt_value))
    rt_value = int(round(rt_value))
    lsb_value = int(round(lsb_value))
    return lt_value, rt_value, lsb_value

# Tkinter GUI für den Kamerastream
camera_root = tk.Tk()
camera_root.title("Kamerastream")
camera_root.geometry("800x600")  # Fenstergröße auf 800x600 Pixel setzen
camera_label = tk.Label(camera_root)
camera_label.pack()
# Globales Shutdown-Future, das beim Fenster-Schließen erfüllt wird
shutdown_future = None

def on_closing():
    print("Fenster geschlossen. Shutdown angefordert.")
    global shutdown_future
    if shutdown_future is not None and not shutdown_future.done():
        shutdown_future.set_result(None)
    camera_root.quit()

camera_root.protocol("WM_DELETE_WINDOW", on_closing)

async def update_tk():
    """Periodisch Tkinter GUI aktualisieren."""
    while True:
        camera_root.update_idletasks()
        camera_root.update()
        await asyncio.sleep(0.03)

async def tcp_client_loop():
    """Control Client an Port 12346"""
    global data_bytes_sent, data_bytes_received
    while True:
        print(f"Verbinde zum ESP unter {esp_ip}:{esp_port} ...")
        try:
            reader, writer = await asyncio.open_connection(esp_ip, esp_port)
            print("TCP-Verbindung (Control) aufgebaut.")
            interval = 1 / messages_per_second
            while True:
                if CONTROL_SEND_ENABLED:
                    lt_value, rt_value, lsb_value = get_controller_inputs()
                    raw_drive = lt_value + rt_value
                    raw_drive = max(-100, min(100, raw_drive))
                    drive_cmd = int((raw_drive + 100) / 2)
                    steering_cmd = max(0, min(100, lsb_value))
                    packet = struct.pack("cBB", b'\x02', steering_cmd, drive_cmd)
                    print("Sende Packet:", packet)
                    writer.write(packet)
                    data_bytes_sent += len(packet)
                    try:
                        await writer.drain()
                    except Exception as e:
                        print("Fehler beim Senden:", e)
                        writer.close()
                        await writer.wait_closed()
                        break
                    try:
                        data = await asyncio.wait_for(reader.read(64), timeout=0.05)
                        if data:
                            data_bytes_received += len(data)
                            print("ACK erhalten:", data.decode().strip())
                    except asyncio.TimeoutError:
                        pass
                else:
                    return
                await asyncio.sleep(interval)
        except Exception as e:
            print("Verbindungsaufbau (Control) fehlgeschlagen:", e)
        print("Control-Verbindung verloren, versuche in 2 Sekunden neu zu verbinden...")
        await asyncio.sleep(2)

async def receive_cam_stream():
    """Empfängt JPEG-komprimierte Frames vom ESP32Cam und zeigt sie in der Tkinter-GUI an."""
    global data_bytes_received
    while True:
        try:
            print(f"Verbinde zum Kamerastream-Server auf Port {CAM_RECEIVE_PORT} ...")
            reader, writer = await asyncio.open_connection(esp_ip, CAM_RECEIVE_PORT)
            print("Kamerastream TCP-Verbindung aufgebaut.")
            
            while True:
                # Lies die Headerzeile, z.B.: "FRAME_START 12345\n"
                header_line = await reader.readline()
                if not header_line:
                    raise Exception("Verbindung beendet während Header-Lesen")
                header_line = header_line.decode().strip()
                if not header_line.startswith("FRAME_START"):
                    print("Ungültiger Header:", header_line)
                    continue
                try:
                    frame_length = int(header_line.split()[1])
                except Exception as e:
                    print("Header Parsing Fehler:", e)
                    continue
                print(f"Erwarte Frame mit {frame_length} Bytes")
                # Lies exakt die JPEG-Daten
                frame_data = await reader.readexactly(frame_length)
                data_bytes_received += len(frame_data)
                # Lese Trailerzeile und überprüfe
                trailer = await reader.readline()
                if b"FRAME_END" not in trailer:
                    print("Ungültiger Trailer:", trailer)
                    continue
                    
                # Öffne das JPEG-Bild via Pillow
                try:
                    from PIL import Image
                    import io
                    pil_img = Image.open(io.BytesIO(frame_data))
                    # Konvertiere das Bild in ein Tkinter-kompatibles PhotoImage
                    from PIL import ImageTk
                    imgtk = ImageTk.PhotoImage(pil_img)
                    camera_label.config(image=imgtk)
                    camera_label.image = imgtk
                    print("Frame erfolgreich empfangen und dargestellt")
                except Exception as e:
                    print("Fehler beim Decodieren des JPEG-Bildes:", e)
                await asyncio.sleep(0.001)
        except Exception as e:
            print("Fehler beim Empfangen des Kamerastreams:", e)
            await asyncio.sleep(1)

async def tcp_monitor_loop():
    """Monitoring Client an Port 12345"""
    global data_bytes_received
    while True:
        try:
            print(f"Verbinde zum ESP unter {esp_ip}:{com_port} (Monitoring)...")
            reader, writer = await asyncio.open_connection(esp_ip, com_port)
            print("Monitoring-Verbindung aufgebaut.")
            while True:
                data = await reader.read(64)
                if data:
                    data_bytes_received += len(data)
                    print(f"Empfangene Daten auf Port {com_port}: {data}")
                else:
                    print("Keine Daten empfangen. Verbindung wird neu aufgebaut.")
                    writer.close()
                    await writer.wait_closed()
                    break
        except Exception as e:
            print("Fehler bei der Monitoring-Verbindung:", e)
        print("Monitoring-Verbindung verloren, versuche in 2 Sekunden neu zu verbinden...")
        await asyncio.sleep(2)

async def tcp_cam_monitor_loop():
    """Verbindet sich mit dem CAM_RECEIVE_PORT und dem CAM_MONITOR_PORT.
       Auf dem Monitoring-Port wird jede Sekunde ein Zählerwert gesendet.
       Auf dem Receive-Port wird geprüft, ob die gesendeten Dummy-Daten (der Zähler)
       empfangen werden."""
    global data_bytes_sent, data_bytes_received
    while True:
        try:
            print(f"Verbinde zum CAM_RECEIVE_PORT {CAM_RECEIVE_PORT} ...")
            reader_recv, writer_recv = await asyncio.open_connection(esp_ip, CAM_RECEIVE_PORT)
            print("Verbindung auf CAM_RECEIVE_PORT hergestellt.")
            print(f"Verbinde zum CAM_MONITOR_PORT {CAM_MONITOR_PORT} ...")
            reader_mon, writer_mon = await asyncio.open_connection(esp_ip, CAM_MONITOR_PORT)
            print("Verbindung auf CAM_MONITOR_PORT hergestellt.")
            counter = 0
            while True:
                try:
                    data_recv = await asyncio.wait_for(reader_recv.read(64), timeout=0.1)
                    if data_recv:
                        dummy_str = data_recv.decode('utf-8').strip()
                        print(f"Auf CAM_RECEIVE_PORT empfangen: {dummy_str}")
                        data_bytes_received += len(data_recv)
                except asyncio.TimeoutError:
                    pass
                message = f"{counter}\n".encode('utf-8')
                writer_mon.write(message)
                data_bytes_sent += len(message)
                await writer_mon.drain()
                print(f"Auf CAM_MONITOR_PORT gesendet: {counter}")
                counter += 1
                await asyncio.sleep(1)
        except Exception as e:
            print("Fehler in tcp_cam_monitor_loop:", e)
            await asyncio.sleep(2)

async def main():
    global shutdown_future
    loop = asyncio.get_running_loop()
    shutdown_future = loop.create_future()
    tasks = []
    tasks.append(asyncio.create_task(tcp_client_loop()))
    if CAM_RECEIVE_ENABLED:
        tasks.append(asyncio.create_task(receive_cam_stream()))
    if MONITORING_CONTROL_ENABLED:
        tasks.append(asyncio.create_task(tcp_monitor_loop()))
    if MONITORING_CAM_ENABLED:
        tasks.append(asyncio.create_task(tcp_cam_monitor_loop()))
    tasks.append(asyncio.create_task(update_tk()))
    tasks.append(asyncio.create_task(save_usage_loop()))
    await shutdown_future
    print("Trenne alle Clients...")
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    print("Alle Clients wurden getrennt. Programm wird beendet.")

if __name__ == "__main__":
    asyncio.run(main())