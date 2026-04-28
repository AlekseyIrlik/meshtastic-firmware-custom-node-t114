#pragma once
#include "MeshService.h"
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

// ─── Настройки порта ────────────────────────────────────────────────────────
#define TRACKING_APP_PORT meshtastic_PortNum_PRIVATE_APP

// ─── Лимиты буфера ──────────────────────────────────────────────────────────
#define MAX_TRACK_POINTS 200 // Точек в RAM-буфере (~несколько часов при 1 мин/точка)
#define POINTS_PER_BATCH 5   // Точек в одном LoRa-пакете (5 * 16 = 80 байт, MTU=237)

// ─── Пороги сохранения точки ────────────────────────────────────────────────
#define MIN_MOVE_DISTANCE_M 10.0f // Сохранять если прошёл > 10 м
#define FORCE_SAVE_INTERVAL_MS 60000 // Сохранять минимум раз в 1 минуту (было 5 мин — слишком редко)

// ─── Интервалы отправки ─────────────────────────────────────────────────────
#define SEND_RETRY_INTERVAL_MS 30000 // Как часто пытаться отправить батч при отсутствии связи
#define SEND_THROTTLE_MS 10000 // Минимальный интервал между любыми двумя отправками
#define GATEWAY_ACK_TIMEOUT_MS 60000 // Если ACK не пришёл за это время — точки снова «свободны»

// ─── Валидация времени ───────────────────────────────────────────────────────
// Unix-время до этой метки считается недостоверным (01.01.2024 00:00:00 UTC)
#define MIN_VALID_TIMESTAMP 1704067200UL

// ─── Бинарная структура одной точки трека (16 байт) ─────────────────────────
// uint32(4) + int32(4) + int32(4) + int16(2) + uint8(1) + uint8(1) = 16
struct __attribute__((packed)) TrackPoint {
    uint32_t timestamp; // Unix-время (сек), полученное от GPS
    int32_t lat_i;      // Широта  × 1e7 (градусы → целое)
    int32_t lon_i;      // Долгота × 1e7
    int16_t alt;        // Высота над уровнем моря (м)
    uint8_t battery;    // Заряд батареи (%)
    uint8_t status;     // Битовое поле: бит 0 = SOS, бит 1 = GPS_FIX_LOST
};
static_assert(sizeof(TrackPoint) == 16, "TrackPoint size mismatch");

// Максимальный размер payload одного батча (защита от переполнения MTU)
#define MAX_BATCH_PAYLOAD_BYTES (POINTS_PER_BATCH * sizeof(TrackPoint))
static_assert(MAX_BATCH_PAYLOAD_BYTES <= 200, "Batch payload exceeds safe LoRa MTU");

// ─── Запись в локальном кольцевом буфере ────────────────────────────────────
struct TrackRecord {
    TrackPoint data;
    uint32_t msg_id; // ID пакета, в котором ушла эта точка (0 = ещё не отправлялась)
    uint32_t sent_at_ms; // millis() момента отправки (для детектирования таймаута ACK)
    bool is_pending;     // true = отправлена, ждёт ACK
};

// ─── Модуль ─────────────────────────────────────────────────────────────────
class TrackerModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    // isGateway = true: узел является шлюзом (принимает данные и пишет в Serial)
    // isGateway = false: узел является трекером спортсмена
    explicit TrackerModule(bool isGateway = false);

    virtual int32_t runOnce() override;

    bool isGatewayMode() const { return _isGateway; }

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual meshtastic_MeshPacket *allocReply() override { return nullptr; }

  private:
    // ── Методы трекера ──────────────────────────────────────────────────────
    void saveCurrentPosition();
    void sendBatchToGateway();
    void onAckReceived(uint32_t msgId);
    void expirePendingPoints(); // Сбрасывает is_pending по таймауту, чтобы точки ушли повторно

    // ── Методы шлюза ────────────────────────────────────────────────────────
    void handleIncomingTrackData(const meshtastic_MeshPacket &mp);

    // ── Поиск шлюза ─────────────────────────────────────────────────────────
    uint32_t findGatewayID();

    // ── Буфер данных ─────────────────────────────────────────────────────────
    TrackRecord *buffer = nullptr;
    uint16_t head = 0;  // Индекс следующей записи
    uint16_t tail = 0;  // Индекс старейшей записи
    uint16_t count = 0; // Текущее количество точек в буфере

    // ── Состояние трекера ────────────────────────────────────────────────────
    uint32_t lastSaveMs = 0;
    uint32_t lastSendAttemptMs = 0;
    int32_t lastStoredLat = 0;
    int32_t lastStoredLon = 0;
    bool hasInitialPos = false;

    // ── Режим работы ─────────────────────────────────────────────────────────
    bool _isGateway;
};

extern TrackerModule *trackerModule;