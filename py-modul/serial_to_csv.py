#!/usr/bin/env python3
"""
Читает Serial, парсит строки @TRACK|{...} и дописывает точки в CSV-файл.
Формат CSV: timestamp,device_id,lat,lon,alt,battery,rssi,snr
"""

import serial
import json
import csv
import argparse
import os
from datetime import datetime

def parse_track_line(line):
    if line.startswith("@TRACK|"):
        json_str = line[len("@TRACK|"):].strip()
        try:
            return json.loads(json_str)
        except json.JSONDecodeError:
            return None
    return None

def main():
    parser = argparse.ArgumentParser(description="Запись треков Meshtastic в CSV")
    parser.add_argument("-p", "--port", required=True, help="Serial порт (например, /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Скорость порта")
    parser.add_argument("-o", "--output", default="tracks.csv", help="Имя выходного CSV-файла")
    args = parser.parse_args()

    # Проверяем, существует ли файл, чтобы записать заголовок только при создании
    file_exists = os.path.isfile(args.output)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        print(f"Serial порт {args.port} открыт. Запись в {args.output}...")
    except serial.SerialException as e:
        print(f"Ошибка открытия порта: {e}")
        return

    with open(args.output, 'a', newline='', encoding='utf-8') as csvfile:
        writer = csv.writer(csvfile)
        if not file_exists:
            writer.writerow(['timestamp', 'device_id', 'lat', 'lon', 'alt', 'battery', 'rssi', 'snr'])

        try:
            while True:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    data = parse_track_line(line)
                    if data:
                        ts = data.get('ts', 0)
                        dev = data.get('dev', 0)
                        lat = data.get('lat', 0.0)
                        lon = data.get('lon', 0.0)
                        alt = data.get('alt', 0)
                        bat = data.get('bat', 0)
                        rssi = data.get('rssi', 0)
                        snr = data.get('snr', 0.0)
                        writer.writerow([ts, dev, lat, lon, alt, bat, rssi, snr])
                        csvfile.flush()  # Чтобы данные сразу попадали на диск
                        dt = datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')
                        print(f"[{dt}] Dev=0x{dev:08x} Lat={lat:.7f} Lon={lon:.7f} Alt={alt}m Bat={bat}% RSSI={rssi} SNR={snr:.1f}")
        except KeyboardInterrupt:
            print("\nОстановлено пользователем.")
        finally:
            ser.close()

if __name__ == "__main__":
    main()