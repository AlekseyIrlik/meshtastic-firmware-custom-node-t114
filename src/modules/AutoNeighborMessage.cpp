#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "configuration.h" // для доступа к moduleConfig, если добавим поля
#include "gps/GeoCoord.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>

AutoNeighborMessage *autoNeighborMessage = nullptr;

// Временные настройки
static constexpr float DEFAULT_DIST_THRESHOLD_M = 20.0f;                   // порог движения в метрах
static constexpr uint32_t DEFAULT_TIME_INTERVAL_MS = 30UL * 60UL * 1000UL; // 30 минут

AutoNeighborMessage::AutoNeighborMessage()
    : ProtobufModule("AutoNeighborMessage", meshtastic_PortNum_POSITION_APP, &meshtastic_Position_msg),
      concurrency::OSThread("AutoNeighborMessage")
{
    // Мы хотим видеть все пакеты, чтобы, возможно, реагировать на запросы (опционально)
    isPromiscuous = true;

    // Устанавливаем интервал OSThread: проверяем состояние каждые 10 секунд
    setIntervalFromNow(10000);

    LOG_INFO("AutoNeighborMessage constructed (sending Position packets with hop_limit=0)");
}

void AutoNeighborMessage::sendPosition(float lat, float lon)
{
    // Создаём Position-пакет
    meshtastic_Position pos = meshtastic_Position_init_default;
    pos.latitude_i = lat * 1e7;
    pos.longitude_i = lon * 1e7;
    pos.time = getValidTime(RTCQualityNTP); // используем актуальное время, если есть

    meshtastic_MeshPacket *p = allocDataProtobuf(pos);
    if (!p) {
        LOG_ERROR("allocDataProtobuf failed");
        return;
    }

    // Ограничиваем распространение только прямыми соседями (hop_limit = 0)
    p->hop_limit = 0;
    // Устанавливаем флаг, чтобы не требовать подтверждения (экономия эфира)
    p->want_ack = false;

    service->sendToMesh(p);

    lastSendTime = millis();
    lastLat = lat * 1e7;
    lastLon = lon * 1e7;
    hasLastPos = true;

    LOG_INFO("Sent Position to neighbors: lat=%.6f lon=%.6f", lat, lon);
}

float AutoNeighborMessage::calculateDistance(float lat1, float lon1, float lat2, float lon2)
{
    return GeoCoord::latLongToMeter(lat1, lon1, lat2, lon2);
}

int32_t AutoNeighborMessage::runOnce()
{
    // Получаем актуальную позицию из локального узла
    auto myNode = service->refreshLocalMeshNode();
    if (!myNode || !myNode->has_position) {
        // Если позиции нет, пробуем позже
        return 10000;
    }

    float lat = myNode->position.latitude_i / 1e7f;
    float lon = myNode->position.longitude_i / 1e7f;

    bool shouldSend = false;

    if (!hasLastPos) {
        LOG_INFO("First position, sending immediately");
        shouldSend = true;
    } else {
        float dist = calculateDistance(lat, lon, lastLat / 1e7f, lastLon / 1e7f);
        if (dist > DEFAULT_DIST_THRESHOLD_M) {
            LOG_INFO("Moved %.1f meters, sending position", dist);
            shouldSend = true;
        }
    }

    uint32_t now = millis();
    if (!shouldSend && (now - lastSendTime) >= DEFAULT_TIME_INTERVAL_MS) {
        LOG_INFO("Time interval expired, sending position");
        shouldSend = true;
    }

    if (shouldSend) {
        sendPosition(lat, lon);
    }

    // Возвращаем интервал проверки (10 секунд)
    return 10000;
}

bool AutoNeighborMessage::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Position *p)
{
    // Мы не обязаны обрабатывать входящие Position-пакеты, но если нужно,
    // можно что-то сделать (например, обновить свою базу соседей).
    // Пока просто игнорируем.
    return false; // разрешаем другим модулям обрабатывать
}