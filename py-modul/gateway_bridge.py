#!/usr/bin/env python3
import serial
import json
import requests
import time
from collections import deque

# ========== НАСТРОЙКИ ==========
SERIAL_PORT = '/dev/ttyACM0'      # порт шлюза
BAUDRATE = 115200                 # скорость Meshtastic по умолчанию
API_ENDPOINT = 'http://your-server.com/api/v1/tracking/batch'  # ваш Web API
BATCH_SIZE = 20                   # отправлять пачками по 20 записей
BATCH_TIMEOUT = 5                 # или каждые 5 секунд
# ================================

class GatewayReader:
    def __init__(self):
        self.ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=1)
        self.buffer = deque()
        self.last_send_time = time.time()

    def parse_line(self, line):
        """Извлекает JSON из строки вида '@TRACK|{...}'"""
        if line.startswith('@TRACK|'):
            try:
                json_str = line.split('@TRACK|', 1)[1].strip()
                return json.loads(json_str)
            except Exception as e:
                print(f"[ERROR] Failed to parse JSON: {line} – {e}")
        return None

    def send_batch(self):
        """Отправляет накопленный буфер на Web API"""
        if not self.buffer:
            return

        payload = {
            "gateway_id": "heltec_t114_gateway",
            "received_at": int(time.time() * 1000),
            "packets": list(self.buffer)
        }

        try:
            response = requests.post(API_ENDPOINT, json=payload, timeout=5)
            if response.status_code == 200:
                print(f"[INFO] Sent {len(self.buffer)} records to API")
                self.buffer.clear()
            else:
                print(f"[ERROR] API returned {response.status_code}: {response.text}")
        except Exception as e:
            print(f"[ERROR] Failed to send to API: {e}")

    def run(self):
        print(f"[INFO] Listening on {SERIAL_PORT} at {BAUDRATE} baud")
        while True:
            try:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if not line:
                    # Проверяем, не пора ли отправить буфер по таймауту
                    if self.buffer and (time.time() - self.last_send_time) >= BATCH_TIMEOUT:
                        self.send_batch()
                        self.last_send_time = time.time()
                    continue

                data = self.parse_line(line)
                if data:
                    self.buffer.append(data)
                    print(f"[DEBUG] Received: {data}")

                    # Отправляем, если накопили BATCH_SIZE записей
                    if len(self.buffer) >= BATCH_SIZE:
                        self.send_batch()
                        self.last_send_time = time.time()

            except KeyboardInterrupt:
                print("\n[INFO] Stopping...")
                if self.buffer:
                    self.send_batch()
                break
            except Exception as e:
                print(f"[ERROR] Unexpected error: {e}")

if __name__ == '__main__':
    reader = GatewayReader()
    reader.run()