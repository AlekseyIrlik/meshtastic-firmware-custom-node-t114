#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h" // для RTCQualityNTR
#include "configuration.h"
#include "gps/GeoCoord.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>
#include <algorithm>

AutoNeighborMessage *autoNeighborMessage = nullptr;

// Конструктор модуля
AutoNeighborMessage::AutoNeighborMessage()
    : ProtobufModule("AutoNeighborMessage", meshtastic_PortNum_POSITION_APP, &meshtastic_Position_msg),
      concurrency::OSThread("AutoNeighborMessage")
{
    // Принимаем все пакеты POSITION_APP, даже не адресованные нам
    isPromiscuous = true;
    // Первый вызов runOnce через 10 секунд, затем периодически каждые 10 секунд
    setIntervalFromNow(10000);
    // Инициализация переменных для отслеживания времени отправки
    lastSendTime = 0;
    lastSendByNeighborMs = 0;
    LOG_INFO("AutoNeighborMessage constructed (sending Position packets with hop_limit=0, "
             "dist_threshold=%.1fm, interval=%us, cleanup=%us, neighbor_send_interval=%us)",
             AUTO_NEIGHBOR_DIST_THRESHOLD_M, AUTO_NEIGHBOR_TIME_INTERVAL_MS / 1000, AUTO_NEIGHBOR_CLEANUP_MS / 1000,
             AUTO_NEIGHBOR_SEND_INTERVAL_MS / 1000);
}

// Отправка пакета позиции в эфир
void AutoNeighborMessage::sendPosition(float lat, float lon, const char *reason)
{
    // Не отправляем нулевую позицию (нет GPS-фикса)
    if (lat == 0.0f && lon == 0.0f) {
        LOG_WARN("Attempted to send zero position, ignoring");
        return;
    }

    meshtastic_Position pos = meshtastic_Position_init_default;
    pos.latitude_i = lat * 1e7; // градусы * 1e7 (так вычисляется в документации)
    pos.longitude_i = lon * 1e7;
    pos.time = getValidTime(RTCQualityNTP); // время отправки, если доступно

    meshtastic_MeshPacket *p = allocDataProtobuf(pos);
    if (!p) {
        LOG_ERROR("allocDataProtobuf failed");
        return;
    }

    // hop_limit = 0 пакет не ретранслируется, достигает только прямых соседей
    p->hop_limit = 0;
    // Широковещательный пакет (to = BROADCAST), ACK не используется
    p->want_ack = false;
    service->sendToMesh(p);

    // Запоминаем последнюю отправленную позицию и время
    lastSendTime = millis();
    lastLat = lat * 1e7;
    lastLon = lon * 1e7;
    hasLastPos = true;

    LOG_INFO("Sent Position to neighbors (reason: %s): lat=%.6f lon=%.6f", reason, lat, lon);
}

// Расчёт расстояния между двумя точками в метрах (из модуля)
float AutoNeighborMessage::calculateDistance(float lat1, float lon1, float lat2, float lon2)
{
    return GeoCoord::latLongToMeter(lat1, lon1, lat2, lon2);
}

// Удаление устаревших записей о соседях
void AutoNeighborMessage::cleanupNeighbors()
{
    uint32_t nowMs = millis();
    size_t oldSize = neighbors.size();
    neighbors.erase(std::remove_if(neighbors.begin(), neighbors.end(), // I like JS style :)
                                   [nowMs](const NeighborPos &n) { return (nowMs - n.lastSeenMs) > AUTO_NEIGHBOR_CLEANUP_MS; }),
                    neighbors.end());
    if (oldSize != neighbors.size()) {
        LOG_DEBUG("Cleaned up %zu stale neighbors", oldSize - neighbors.size());
    }
}

int32_t AutoNeighborMessage::runOnce()
{
    auto myNode = service->refreshLocalMeshNode();
    if (!myNode || !myNode->has_position) {
        return 10000; // нет позиции – ждём
    }

    float lat = myNode->position.latitude_i / 1e7f;
    float lon = myNode->position.longitude_i / 1e7f;
    float alt = myNode->position.altitude;
    bool shouldSend = false;
    uint32_t nowMs = millis();
    const char *sendReason = nullptr;

    // Собственное перемещение
    if (!hasLastPos) {
        LOG_INFO("First position, sending immediately");
        shouldSend = true;
        sendReason = "first_position";
    } else {
        float dist = calculateDistance(lat, lon, lastLat / 1e7f, lastLon / 1e7f);
        if (dist > AUTO_NEIGHBOR_DIST_THRESHOLD_M) {
            LOG_INFO("Moved %.1f meters, sending position", dist);
            shouldSend = true;
            sendReason = "movement";
        }
    }

    // Нахождение рядом с любым из сохранённых соседей (с троттлингом)
    cleanupNeighbors(); // сначала удаляем старых
    if (!shouldSend && !neighbors.empty()) {
        for (const auto &nbr : neighbors) {
            float distToNeighbor = calculateDistance(lat, lon, nbr.lat / 1e7f, nbr.lon / 1e7f);
            if (distToNeighbor <= AUTO_NEIGHBOR_DIST_THRESHOLD_M) {
                // Отправляем только если не прошло слишком мало времени с последней такой отправки
                if (nowMs - lastSendByNeighborMs >= AUTO_NEIGHBOR_SEND_INTERVAL_MS) {
                    shouldSend = true;
                    lastSendByNeighborMs = nowMs;
                    sendReason = "neighbor";
                    LOG_INFO("Neighbor 0x%x within %.1f meters (dist=%.1f), sending position", nbr.nodeId,
                             AUTO_NEIGHBOR_DIST_THRESHOLD_M, distToNeighbor);
                } else {
                    LOG_DEBUG("Neighbor nearby but send throttled (last send %lu ms ago)", nowMs - lastSendByNeighborMs);
                }
                break;
            }
        }
    }

    // Таймер (не ограничивается троттлингом)
    if (!shouldSend && (nowMs - lastSendTime) >= AUTO_NEIGHBOR_TIME_INTERVAL_MS) {
        LOG_INFO("Time interval expired, sending position");
        shouldSend = true;
        sendReason = "timer";
    }

    if (shouldSend) {
        sendPosition(lat, lon, sendReason);
    }

    return 10000; // следующий вызов через 10 секунд
}

// Обработка входящих пакетов позиции
bool AutoNeighborMessage::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Position *p)
{
    // Игнорируем свои пакеты
    if (isFromUs(&mp)) {
        return false;
    }

    // Сохраняем только пакеты от прямых соседей (hop_limit == 0)
    if (mp.hop_limit != 0) {
        return false;
    }

    // Сохраняем позицию полученного узла в локальный список соседей, только если она ненулевая
    if (p && (p->latitude_i != 0 || p->longitude_i != 0)) {
        uint32_t nowMs = millis();
        bool found = false;
        for (auto &nbr : neighbors) {
            if (nbr.nodeId == mp.from) {
                nbr.lat = p->latitude_i;
                nbr.lon = p->longitude_i;
                nbr.lastSeenMs = nowMs;
                found = true;
                break;
            }
        }
        if (!found) {
            neighbors.push_back({mp.from, p->latitude_i, p->longitude_i, nowMs});
            LOG_DEBUG("Added neighbor 0x%x pos: lat=%.6f lon=%.6f", mp.from, p->latitude_i / 1e7f, p->longitude_i / 1e7f);
        } else {
            LOG_DEBUG("Updated neighbor 0x%x pos: lat=%.6f lon=%.6f", mp.from, p->latitude_i / 1e7f, p->longitude_i / 1e7f);
        }
    }
    // Возвращаем false, чтобы другие модули тоже могли обработать пакет
    return false;
}