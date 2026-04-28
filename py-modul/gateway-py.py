#!/usr/bin/env python3
"""
Мониторинг треков через официальную библиотеку Meshtastic.
"""

import struct
import sys
import time
from datetime import datetime

import meshtastic
import meshtastic.serial_interface
from pubsub import pub

# Порт, который использует наш TrackerModule
TRACKER_PORT_NUM = 256  # meshtastic_PortNum_PRIVATE_APP


def onReceive(packet, interface):
    """Callback-функция, вызывается при получении любого пакета."""
    try:
        # Проверяем, что пакет имеет нужный нам порт
        if packet.get("decoded", {}).get("portnum") != TRACKER_PORT_NUM:
            return

        payload = packet.get("decoded", {}).get("payload")
        if not payload:
            return

        # Разбираем бинарные данные (батч из нескольких TrackPoint)
        point_size = 16  # sizeof(TrackPoint)
        num_points = len(payload) // point_size

        for i in range(num_points):
            offset = i * point_size
            data = payload[offset : offset + point_size]

            # Распаковываем согласно структуре TrackPoint
            ts, lat_i, lon_i, alt, bat, status = struct.unpack("<I i i h B B", data)

            # Преобразуем координаты обратно в градусы
            lat = lat_i / 1e7
            lon = lon_i / 1e7

            dt = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")

            print(
                f"[{dt}] From=0x{packet.get('from', 0):08x} "
                f"Lat={lat:.7f} Lon={lon:.7f} Alt={alt}m Bat={bat}%"
            )

    except Exception as e:
        print(f"Ошибка обработки пакета: {e}")


def onConnection(interface, topic=pub.AUTO_TOPIC):
    """Callback при успешном подключении к устройству."""
    print("Подключено к устройству. Ожидание данных трекинга...")


if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else None

    # Подписываемся на события библиотеки
    pub.subscribe(onReceive, "meshtastic.receive")
    pub.subscribe(onConnection, "meshtastic.connection.established")

    # Создаём интерфейс (автоматически найдёт устройство, если port не указан)
    try:
        if port:
            interface = meshtastic.serial_interface.SerialInterface(devPath=port)
        else:
            interface = meshtastic.serial_interface.SerialInterface()
    except Exception as e:
        print(f"Ошибка подключения: {e}")
        sys.exit(1)

    # Держим скрипт запущенным
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nОстановлено.")
        interface.close()
