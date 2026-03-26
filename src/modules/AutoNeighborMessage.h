#pragma once

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include <vector>

// Конфигурационные параметры (можно переопределить через build_flags)
#ifndef AUTO_NEIGHBOR_DIST_THRESHOLD_M
#define AUTO_NEIGHBOR_DIST_THRESHOLD_M 6.0f
#endif

#ifndef AUTO_NEIGHBOR_TIME_INTERVAL_MS
#define AUTO_NEIGHBOR_TIME_INTERVAL_MS (30UL * 60UL * 1000UL)
#endif

#ifndef AUTO_NEIGHBOR_CLEANUP_MS
#define AUTO_NEIGHBOR_CLEANUP_MS (120UL * 1000UL) // 2 минуты
#endif

#ifndef AUTO_NEIGHBOR_SEND_INTERVAL_MS
#define AUTO_NEIGHBOR_SEND_INTERVAL_MS (30UL * 1000UL) // 30 секунд
#endif

struct NeighborPos {
    uint32_t nodeId;     // идентификатор узла
    int32_t lat;         // широта * 1e7
    int32_t lon;         // долгота * 1e7
    int32_t alt;         // высота
    uint32_t lastSeenMs; // время последнего получения (millis)
};

/**
 * Модуль AutoNeighborMessage предназначен для автоматической отправки
 * широковещательных пакетов позиции (Position) с hop_limit = 0,
 * чтобы они достигали только прямых соседей.
 *
 * Условия отправки:
 *   - При перемещении узла на расстояние > AUTO_NEIGHBOR_DIST_THRESHOLD_M
 *   - Если рядом (в пределах порога) находится хотя бы один сосед,
 *     но не чаще чем AUTO_NEIGHBOR_SEND_INTERVAL_MS
 *   - По таймеру AUTO_NEIGHBOR_TIME_INTERVAL_MS (периодическая отправка)
 *
 * Модуль также собирает информацию о соседях из входящих Position-пакетов
 * и удаляет устаревшие записи через AUTO_NEIGHBOR_CLEANUP_MS.
 */
class AutoNeighborMessage : public ProtobufModule<meshtastic_Position>, public concurrency::OSThread
{
  public:
    AutoNeighborMessage();

  protected:
    virtual int32_t runOnce() override;
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Position *p) override;

  private:
    uint32_t lastSendTime = 0;         // время последней отправки (millis)
    uint32_t lastSendByNeighborMs = 1; // время последней отправки из-за соседа (millis)
    int32_t lastLat = 0;               // предыдущая широта * 1e7
    int32_t lastLon = 0;               // предыдущая долгота * 1e7
    bool hasLastPos = false;           // была ли предыдущая позиция

    std::vector<NeighborPos> neighbors; // список соседей

    void sendPosition(float lat, float lon, const char *reason);
    float calculateDistance(float lat1, float lon1, float lat2, float lon2);
    void cleanupNeighbors(); // удаляет устаревших соседей
};

extern AutoNeighborMessage *autoNeighborMessage;
