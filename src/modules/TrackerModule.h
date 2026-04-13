#pragma once

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include <vector>


#ifndef TRACKER_SAVE_INTERVAL_SEC
#define TRACKER_SAVE_INTERVAL_SEC 60      // интервал сохранения позиции, секунд
#endif

#ifndef TRACKER_MAX_RECORDS
#define TRACKER_MAX_RECORDS 500           // максимальное количество записей в буфере
#endif

#ifndef TRACKER_BATCH_SIZE
#define TRACKER_BATCH_SIZE 20             // максимальное количество записей в одном mesh-пакете
#endif

#ifndef TRACKER_HOP_LIMIT
#define TRACKER_HOP_LIMIT 3               // hop_limit для пакетов трекера
#endif

#ifndef TRACKER_SEND_RETRY_INTERVAL_SEC
#define TRACKER_SEND_RETRY_INTERVAL_SEC 60 // интервал повторных попыток отправки, если шлюз недоступен
#endif


// Бинарный формат одной записи трека (24 байта)

struct TrackerPacket {
    uint32_t device_id;   // уникальный ID спортсмена (номер узла)
    uint32_t timestamp;   // UNIX time (секунды)
    int32_t  latitude_i;  // широта * 1e7
    int32_t  longitude_i; // долгота * 1e7
    int32_t  altitude;    // высота в метрах
    uint8_t  battery;     // заряд батареи (0-100)
    uint8_t  _padding[3]; // выравнивание (не используется)
};

// Структура для локального хранения записи в буфере
struct TrackerRecord {
    TrackerPacket packet;
    bool acked;           // подтверждено шлюзом?
    uint32_t sentPacketId;// ID mesh-пакета, которым отправлено (для сопоставления ACK)
};

// Информация о пакете, ожидающем подтверждения
struct PendingSend {
    uint32_t requestId;        // ID mesh-пакета
    uint16_t firstRecordIndex; // индекс первой записи в кольцевом буфере
    uint16_t recordCount;      // количество записей
};


// Основной класс модуля

class TrackerModule : public SinglePortModule, private concurrency::OSThread
{
public:
    TrackerModule();

protected:
    virtual int32_t runOnce() override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual meshtastic_MeshPacket *allocReply() override { return nullptr; }

private:
    bool isGatewayMode() const;
    bool isGatewayAvailable() const;
    float calculateDistance(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);

    void savePosition(int32_t lat, int32_t lon, int32_t alt);
    void sendPendingRecords();
    void markRecordsAcked(uint32_t requestId);
    void cleanupAckedRecords();
    void outputToSerial(const std::vector<TrackerPacket> &packets);
    void initBuffer();

    uint32_t deviceId;
    uint32_t lastSaveMs;
    uint32_t lastSendAttemptMs;
    bool gatewayMode;

    TrackerRecord *records;
    uint16_t maxRecords;
    uint16_t writeIndex;
    uint16_t recordCount;

    std::vector<PendingSend> pendingSends;

    // Для отслеживания движения
    bool hasLastPosition;
    int32_t lastLat;
    int32_t lastLon;
};

extern TrackerModule *trackerModule;