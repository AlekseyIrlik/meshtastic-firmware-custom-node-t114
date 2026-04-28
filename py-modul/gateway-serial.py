#!/usr/bin/env python3
"""
Мониторинг треков со шлюза Meshtastic TrackerModule.
Читает строки с префиксом @TRACK| из Serial или лог-файла.
"""

import serial
import json
import sys
import argparse
import time
from datetime import datetime

def parse_track_line(line):
    """Извлекает JSON из строки @TRACK|... и возвращает словарь."""
    if line.startswith("@TRACK|"):
        json_str = line[len("@TRACK|"):].strip()
        try:
            return json.loads(json_str)
        except json.JSONDecodeError as e:
            print(f"Ошибка парсинга JSON: {e}\nСтрока: {line}", file=sys.stderr)
            return None
    return None

def format_track(data):
    """Форматирует данные трека для вывода в консоль."""
    ts = data.get('ts', 0)
    dt = datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S') if ts else 'N/A'
    lat = data.get('lat', 0)
    lon = data.get('lon', 0)
    alt = data.get('alt', 0)
    bat = data.get('bat', '?')
    dev = data.get('dev', 0)
    rssi = data.get('rssi', 0)
    snr = data.get('snr', 0)

    return (f"[{dt}] Dev=0x{dev:08x} "
            f"Lat={lat:.7f} Lon={lon:.7f} Alt={alt}m "
            f"Bat={bat}% RSSI={rssi} SNR={snr:.1f}")

def monitor_serial(port, baudrate=115200):
    """Читает Serial порт и выводит треки."""
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
        print(f"Мониторинг {port} на скорости {baudrate}...")
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                data = parse_track_line(line)
                if data:
                    print(format_track(data))
    except serial.SerialException as e:
        print(f"Ошибка Serial: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nОстановлено пользователем.")
    finally:
        if 'ser' in locals() and ser.is_open:
            ser.close()

def process_logfile(filename):
    """Обрабатывает существующий лог-файл."""
    with open(filename, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            data = parse_track_line(line)
            if data:
                print(format_track(data))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Парсер треков Meshtastic TrackerModule")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("-p", "--port", help="Serial порт (например, /dev/ttyACM0)")
    group.add_argument("-f", "--file", help="Лог-файл с записями @TRACK|...")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Скорость Serial (по умолчанию 115200)")

    args = parser.parse_args()

    if args.port:
        monitor_serial(args.port, args.baud)
    else:
        process_logfile(args.file)