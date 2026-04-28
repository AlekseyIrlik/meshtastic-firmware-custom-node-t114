#!/usr/bin/env python3
"""
Мониторинг треков со шлюза Meshtastic с визуализацией на карте.
Читает строки @TRACK|{...} из Serial и отображает их в реальном времени.
"""

import serial
import json
import sys
import argparse
from datetime import datetime
from threading import Thread, Lock

from flask import Flask, render_template_string
from flask_socketio import SocketIO, emit

# Глобальный буфер последних точек (для новых подключений)
latest_points = []
points_lock = Lock()

app = Flask(__name__)
socketio = SocketIO(app, async_mode='threading')

HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>Meshtastic Tracker</title>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    <script src="https://cdn.socket.io/4.5.4/socket.io.min.js"></script>
    <style>
        body { margin: 0; padding: 0; }
        #map { position: absolute; top: 0; bottom: 0; width: 100%; }
        .info {
            position: absolute;
            bottom: 20px;
            left: 20px;
            background: white;
            padding: 8px 12px;
            border-radius: 4px;
            box-shadow: 0 0 10px rgba(0,0,0,0.2);
            z-index: 1000;
            font-family: monospace;
        }
    </style>
</head>
<body>
    <div id="map"></div>
    <div class="info" id="status">Ожидание данных...</div>

    <script>
        // Инициализация карты
        var map = L.map('map').setView([52.51, 85.15], 13);
        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '© OpenStreetMap'
        }).addTo(map);

        // Слои для маркеров и путей треков
        var markers = {};       // device_id -> маркер текущей позиции
        var tracks = {};        // device_id -> массив координат
        var polylines = {};     // device_id -> полилиния
        var pointMarkers = {};  // device_id -> массив кружков на линии

        var socket = io();

        socket.on('connect', function() {
            document.getElementById('status').innerHTML = 'Подключено к серверу';
        });

        socket.on('disconnect', function() {
            document.getElementById('status').innerHTML = 'Соединение потеряно';
        });

        // Получение новой точки
        socket.on('track_point', function(data) {
            var dev = data.dev.toString(16);
            var lat = data.lat;
            var lon = data.lon;
            var ts = new Date(data.ts * 1000).toLocaleTimeString();
            var bat = data.bat;
            var rssi = data.rssi;
            var snr = data.snr;

            // Обновляем статус
            document.getElementById('status').innerHTML =
                `Dev: 0x${dev} | ${ts} | Bat: ${bat}% | RSSI: ${rssi} | SNR: ${snr}`;

            // Инициализация при первом появлении устройства
            if (!markers[dev]) {
                // Случайный цвет для трека на основе device_id
                var hue = (parseInt(dev, 16) * 137) % 360;
                var color = 'hsl(' + hue + ', 80%, 50%)';

                // Маркер текущего положения
                markers[dev] = L.marker([lat, lon], {
                    title: 'Device 0x' + dev
                }).bindPopup('Device 0x' + dev).addTo(map);

                // Полилиния (маршрут)
                polylines[dev] = L.polyline([], {color: color, weight: 4, opacity: 0.7}).addTo(map);

                // Массив для координат трека
                tracks[dev] = [];

                // Массив для точечных маркеров вдоль линии
                pointMarkers[dev] = [];
            }

            // Добавляем точку в трек
            tracks[dev].push([lat, lon]);

            // Обновляем линию маршрута
            polylines[dev].setLatLngs(tracks[dev]);

            // Добавляем маленький кружок на линию
            var point = L.circleMarker([lat, lon], {
                radius: 4,
                color: polylines[dev].options.color,
                fillColor: '#fff',
                fillOpacity: 1,
                weight: 2
            }).bindPopup(ts + '<br>Bat: ' + bat + '%').addTo(map);
            pointMarkers[dev].push(point);

            // Перемещаем основной маркер на новую позицию
            markers[dev].setLatLng([lat, lon]);

            // Подгоняем границы карты под все точки
            var allPoints = [];
            for (var d in tracks) {
                allPoints = allPoints.concat(tracks[d]);
            }
            if (allPoints.length > 0) {
                map.fitBounds(L.latLngBounds(allPoints), {padding: [50, 50]});
            }
        });

        // При загрузке страницы запрашиваем последние точки (если есть)
        window.onload = function() {
            socket.emit('request_initial');
        };
        socket.on('initial_points', function(points) {
            points.forEach(function(data) {
                // Используем ту же логику обработки точки
                var dev = data.dev.toString(16);
                if (!markers[dev]) {
                    var hue = (parseInt(dev, 16) * 137) % 360;
                    var color = 'hsl(' + hue + ', 80%, 50%)';
                    markers[dev] = L.marker([data.lat, data.lon]).addTo(map);
                    polylines[dev] = L.polyline([], {color: color}).addTo(map);
                    tracks[dev] = [];
                    pointMarkers[dev] = [];
                }
                tracks[dev].push([data.lat, data.lon]);
                polylines[dev].setLatLngs(tracks[dev]);
                markers[dev].setLatLng([data.lat, data.lon]);
                L.circleMarker([data.lat, data.lon], {
                    radius: 4,
                    color: polylines[dev].options.color,
                    fillColor: '#fff',
                    fillOpacity: 1,
                    weight: 2
                }).addTo(map);
            });
        });
    </script>
</body>
</html>
"""

@app.route('/')
def index():
    return render_template_string(HTML_TEMPLATE)

@socketio.on('connect')
def handle_connect():
    # Отправляем последние известные точки при подключении
    with points_lock:
        for pt in latest_points[-50:]:  # Последние 50 точек
            emit('track_point', pt)

def parse_track_line(line):
    """Извлекает JSON из строки @TRACK|..."""
    if line.startswith("@TRACK|"):
        json_str = line[len("@TRACK|"):].strip()
        try:
            return json.loads(json_str)
        except json.JSONDecodeError:
            return None
    return None

def serial_reader(port, baudrate):
    """Читает Serial и рассылает точки через WebSocket."""
    global latest_points
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
        print(f"Serial порт {port} открыт. Ожидание данных...")
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                data = parse_track_line(line)
                if data:
                    # Добавляем в буфер
                    with points_lock:
                        latest_points.append(data)
                        if len(latest_points) > 200:
                            latest_points.pop(0)
                    # Отправляем всем клиентам
                    socketio.emit('track_point', data)
                    # Логируем в консоль
                    ts = datetime.fromtimestamp(data['ts']).strftime('%Y-%m-%d %H:%M:%S')
                    print(f"[{ts}] Dev=0x{data['dev']:08x} "
                          f"Lat={data['lat']:.7f} Lon={data['lon']:.7f} "
                          f"Alt={data['alt']}m Bat={data['bat']}% "
                          f"RSSI={data['rssi']} SNR={data['snr']:.1f}")
    except serial.SerialException as e:
        print(f"Ошибка Serial: {e}")
    except KeyboardInterrupt:
        pass
    finally:
        if 'ser' in locals() and ser.is_open:
            ser.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Визуализация треков Meshtastic")
    parser.add_argument("-p", "--port", required=True, help="Serial порт (например, /dev/ttyACM1)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Скорость порта")
    parser.add_argument("--host", default="0.0.0.0", help="Хост для веб-сервера")
    parser.add_argument("--web-port", type=int, default=5000, help="Порт веб-сервера")
    args = parser.parse_args()

    # Запускаем Serial-поток в фоне
    serial_thread = Thread(target=serial_reader, args=(args.port, args.baud), daemon=True)
    serial_thread.start()

    # Запускаем Flask-SocketIO сервер
    print(f"\nКарта доступна по адресу: http://localhost:{args.web_port}\n")
    socketio.run(app, host=args.host, port=args.web_port, debug=False)