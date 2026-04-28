#!/usr/bin/env python3
"""
Читает CSV с треками и генерирует статическую HTML-карту с маршрутами.
"""

import csv
import argparse
import webbrowser
from collections import defaultdict
from datetime import datetime
import os

HTML_TEMPLATE = """<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Meshtastic Tracks</title>
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    <style>
        body { margin: 0; padding: 0; }
        #map { position: absolute; top: 0; bottom: 0; width: 100%; }
        .info {
            position: absolute;
            top: 10px;
            right: 10px;
            background: white;
            padding: 8px 12px;
            border-radius: 4px;
            box-shadow: 0 0 10px rgba(0,0,0,0.2);
            z-index: 1000;
            font-family: sans-serif;
        }
    </style>
</head>
<body>
    <div id="map"></div>
    <div class="info" id="info">Загрузка...</div>
    <script>
        // Данные треков, вставленные Python-скриптом
        var tracksData = __TRACKS_DATA__;

        var map = L.map('map');
        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '© OpenStreetMap'
        }).addTo(map);

        var allPoints = [];
        var deviceTracks = {};

        // Группируем точки по device_id
        tracksData.forEach(function(pt) {
            var dev = pt.device_id.toString(16);
            if (!deviceTracks[dev]) {
                deviceTracks[dev] = [];
            }
            deviceTracks[dev].push([pt.lat, pt.lon]);
            allPoints.push([pt.lat, pt.lon]);
        });

        // Рисуем треки для каждого устройства
        Object.keys(deviceTracks).forEach(function(dev) {
            var points = deviceTracks[dev];
            var hue = (parseInt(dev, 16) * 137) % 360;
            var color = 'hsl(' + hue + ', 80%, 50%)';

            // Полилиния маршрута
            var line = L.polyline(points, {color: color, weight: 4, opacity: 0.7}).addTo(map);

            // Маркер начала (зелёный)
            if (points.length > 0) {
                L.circleMarker(points[0], {
                    radius: 6,
                    color: 'green',
                    fillColor: '#fff',
                    fillOpacity: 1,
                    weight: 3
                }).bindTooltip('Start 0x' + dev, {permanent: false}).addTo(map);
            }

            // Маркер конца (красный)
            if (points.length > 1) {
                L.circleMarker(points[points.length-1], {
                    radius: 6,
                    color: 'red',
                    fillColor: '#fff',
                    fillOpacity: 1,
                    weight: 3
                }).bindTooltip('End 0x' + dev, {permanent: false}).addTo(map);
            }

            // Промежуточные точки (маленькие кружки)
            points.forEach(function(coord) {
                L.circleMarker(coord, {
                    radius: 3,
                    color: color,
                    fillColor: '#fff',
                    fillOpacity: 1,
                    weight: 2
                }).addTo(map);
            });
        });

        // Подгоняем карту под все точки
        if (allPoints.length > 0) {
            var bounds = L.latLngBounds(allPoints);
            map.fitBounds(bounds, {padding: [50, 50]});
            document.getElementById('info').innerHTML =
                'Устройств: ' + Object.keys(deviceTracks).length +
                ', точек: ' + allPoints.length;
        } else {
            document.getElementById('info').innerHTML = 'Нет данных';
        }
    </script>
</body>
</html>
"""

def main():
    parser = argparse.ArgumentParser(description="Генерация карты по CSV с треками")
    parser.add_argument("-i", "--input", default="tracks.csv", help="Входной CSV-файл")
    parser.add_argument("-o", "--output", default="tracks_map.html", help="Выходной HTML-файл")
    parser.add_argument("--open", action="store_true", help="Открыть карту в браузере")
    args = parser.parse_args()

    tracks = []
    try:
        with open(args.input, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                # Преобразуем типы
                pt = {
                    'timestamp': int(row['timestamp']),
                    'device_id': int(row['device_id']),
                    'lat': float(row['lat']),
                    'lon': float(row['lon']),
                    'alt': int(row['alt']),
                    'battery': int(row['battery']),
                    'rssi': int(row['rssi']),
                    'snr': float(row['snr'])
                }
                tracks.append(pt)
    except FileNotFoundError:
        print(f"Файл {args.input} не найден.")
        return
    except Exception as e:
        print(f"Ошибка чтения CSV: {e}")
        return

    if not tracks:
        print("Нет данных для отображения.")
        return

    # Преобразуем данные в JSON-строку для вставки в HTML
    import json
    tracks_json = json.dumps(tracks)

    html_content = HTML_TEMPLATE.replace('__TRACKS_DATA__', tracks_json)

    with open(args.output, 'w', encoding='utf-8') as f:
        f.write(html_content)

    print(f"Карта сохранена в {args.output}")
    if args.open:
        webbrowser.open('file://' + os.path.abspath(args.output))

if __name__ == "__main__":
    main()