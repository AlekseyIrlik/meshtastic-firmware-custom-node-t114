#include "TrackerModule.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "gps/GeoCoord.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h" // meshtastic_Routing, meshtastic_Routing_fields
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>
#include <pb_decode.h>

TrackerModule *trackerModule;

TrackerModule::TrackerModule(bool isGateway)
    : SinglePortModule("AthleteTracker", TRACKING_APP_PORT), concurrency::OSThread("AthleteTracker"), _isGateway(isGateway)
{
    // Шлюзу буфер не нужен — он только принимает и пишет в Serial
    if (!_isGateway) {
        buffer = (TrackRecord *)malloc(MAX_TRACK_POINTS * sizeof(TrackRecord));
        if (!buffer) {
            LOG_ERROR("Tracker: Failed to allocate track buffer! (%u bytes)", MAX_TRACK_POINTS * sizeof(TrackRecord));
        } else {
            memset(buffer, 0, MAX_TRACK_POINTS * sizeof(TrackRecord));
        }
    }

    LOG_INFO("Tracker: Starting in %s mode", _isGateway ? "GATEWAY" : "ATHLETE");
    setIntervalFromNow(5000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Поиск шлюза (ROUTER) в текущей Mesh-сети
// ─────────────────────────────────────────────────────────────────────────────
uint32_t TrackerModule::findGatewayID()
{
    uint32_t bestNode = 0;
    int32_t bestSnr = INT32_MIN; // Выбираем шлюз с лучшим SNR (ближайший)

    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        auto node = nodeDB->getMeshNodeByIndex(i);
        if (!node || !node->has_user)
            continue;
        if (node->user.role != meshtastic_Config_DeviceConfig_Role_ROUTER)
            continue;

        // Среди нескольких шлюзов выбираем с наилучшим SNR последнего пакета
        if (node->snr > bestSnr || bestNode == 0) {
            bestSnr = node->snr;
            bestNode = node->num;
        }
    }

    if (bestNode == 0) {
        // Шлюз не найден — широковещательная рассылка как крайний резерв.
        // Это создаёт лишний трафик, но гарантирует, что хоть кто-то услышит пакет.
        LOG_WARN("Tracker: No ROUTER found in mesh — falling back to BROADCAST");
        return NODENUM_BROADCAST;
    }

    LOG_DEBUG("Tracker: Gateway selected: 0x%x (SNR=%d)", bestNode, bestSnr);
    return bestNode;
}

// ─────────────────────────────────────────────────────────────────────────────
// Сохранение текущей позиции в кольцевой буфер
// ─────────────────────────────────────────────────────────────────────────────
void TrackerModule::saveCurrentPosition()
{
    if (!buffer)
        return;

    auto me = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!me || !me->has_position || me->position.latitude_i == 0)
        return;

    // FIX: Проверяем достоверность времени перед записью.
    // getTime() может вернуть ~0 или некорректное значение, пока GPS не дал фикс.
    // Точки с невалидным timestamp сломают сортировку на бэкенде.
    uint32_t currentTime = getTime();
    if (currentTime < MIN_VALID_TIMESTAMP) {
        LOG_WARN("Tracker: GPS time not valid yet (%u), skipping point", currentTime);
        return;
    }

    uint32_t now = millis();

    // Проверяем, нужно ли сохранять (по дистанции или по времени)
    if (hasInitialPos) {
        float dist = GeoCoord::latLongToMeter(me->position.latitude_i * 1e-7f, me->position.longitude_i * 1e-7f,
                                              lastStoredLat * 1e-7f, lastStoredLon * 1e-7f);

        bool timeExpired = (now - lastSaveMs >= FORCE_SAVE_INTERVAL_MS);
        bool movedEnough = (dist >= MIN_MOVE_DISTANCE_M);

        if (!timeExpired && !movedEnough)
            return;
    }

    // Кольцевой буфер: если полон — вытесняем самую старую точку.
    // Стратегия: новые данные важнее старых (ситуация «финиш важнее старта» не идеальна,
    // но при S&F в RAM это единственный разумный вариант без Flash).
    if (count >= MAX_TRACK_POINTS) {
        tail = (tail + 1) % MAX_TRACK_POINTS;
        count--;
        LOG_WARN("Tracker: Buffer full, oldest point dropped");
    }

    TrackRecord &rec = buffer[head];
    rec.data.timestamp = currentTime;
    rec.data.lat_i = me->position.latitude_i;
    rec.data.lon_i = me->position.longitude_i;
    rec.data.alt = (int16_t)me->position.altitude;
    rec.data.battery = powerStatus ? powerStatus->getBatteryChargePercent() : 0;
    rec.data.status = 0;
    rec.is_pending = false;
    rec.msg_id = 0;
    rec.sent_at_ms = 0;

    lastStoredLat = me->position.latitude_i;
    lastStoredLon = me->position.longitude_i;
    lastSaveMs = now;
    hasInitialPos = true;

    head = (head + 1) % MAX_TRACK_POINTS;
    count++;

    LOG_INFO("Tracker: Point saved [ts=%u lat=%.5f lon=%.5f bat=%u%%]. Buffer: %u/%u", currentTime,
             me->position.latitude_i * 1e-7, me->position.longitude_i * 1e-7, rec.data.battery, count, MAX_TRACK_POINTS);
}

// ─────────────────────────────────────────────────────────────────────────────
// Сброс флага is_pending у точек, чьё ACK не пришло вовремя
// ─────────────────────────────────────────────────────────────────────────────
void TrackerModule::expirePendingPoints()
{
    if (!buffer)
        return;

    uint32_t now = millis();
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (tail + i) % MAX_TRACK_POINTS;
        TrackRecord &rec = buffer[idx];

        if (rec.is_pending && rec.sent_at_ms > 0 && (now - rec.sent_at_ms) > GATEWAY_ACK_TIMEOUT_MS) {
            rec.is_pending = false;
            rec.msg_id = 0;
            rec.sent_at_ms = 0;
            LOG_WARN("Tracker: Point %u ACK timed out, will retry", idx);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Отправка батча точек на шлюз
// ─────────────────────────────────────────────────────────────────────────────
void TrackerModule::sendBatchToGateway()
{
    if (!buffer || count == 0)
        return;

    // Собираем только точки, которые НЕ ждут ACK (не is_pending)
    uint16_t toSend = 0;
    uint16_t candidates[POINTS_PER_BATCH];

    for (uint16_t i = 0; i < count && toSend < POINTS_PER_BATCH; i++) {
        uint16_t idx = (tail + i) % MAX_TRACK_POINTS;
        if (!buffer[idx].is_pending) {
            candidates[toSend++] = idx;
        }
    }

    if (toSend == 0) {
        LOG_DEBUG("Tracker: All buffered points are pending ACK, skipping send");
        return;
    }

    uint32_t target = findGatewayID();

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = target;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    p->want_ack = true;
    p->decoded.portnum = ourPortNum;

    // FIX: Проверяем, что payload не превысит лимит перед записью.
    const size_t requiredBytes = toSend * sizeof(TrackPoint);
    if (requiredBytes > sizeof(p->decoded.payload.bytes)) {
        LOG_ERROR("Tracker: Payload too large (%u bytes), reducing batch", requiredBytes);
        toSend = sizeof(p->decoded.payload.bytes) / sizeof(TrackPoint);
    }

    uint8_t *payloadPtr = p->decoded.payload.bytes;
    for (uint16_t i = 0; i < toSend; i++) {
        uint16_t idx = candidates[i];
        memcpy(payloadPtr, &buffer[idx].data, sizeof(TrackPoint));
        payloadPtr += sizeof(TrackPoint);

        // Помечаем точки как «ожидают ACK»
        buffer[idx].is_pending = true;
        buffer[idx].msg_id = p->id;
        buffer[idx].sent_at_ms = millis();
    }

    p->decoded.payload.size = toSend * sizeof(TrackPoint);

    LOG_INFO("Tracker: Sending batch of %u points to 0x%x (MsgID=0x%x)", toSend, target, p->id);
    service->sendToMesh(p);
    lastSendAttemptMs = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
// Обработка полученного пакета (и для шлюза, и для спортсмена)
// ─────────────────────────────────────────────────────────────────────────────
ProcessMessage TrackerModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // ── РЕЖИМ ШЛЮЗА: принимаем данные спортсменов ────────────────────────────
    if (_isGateway) {
        if (mp.decoded.portnum == ourPortNum) {
            handleIncomingTrackData(mp);
            return ProcessMessage::STOP;
        }
        return ProcessMessage::CONTINUE;
    }

    // ── РЕЖИМ СПОРТСМЕНА: обрабатываем ACK от Mesh-слоя ──────────────────────
    // Routing-данные не являются полем MeshPacket — они приходят как protobuf-payload
    // на порту ROUTING_APP и требуют явного декодирования.
    if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        meshtastic_Routing routing = meshtastic_Routing_init_default;
        pb_istream_t stream = pb_istream_from_buffer(mp.decoded.payload.bytes, mp.decoded.payload.size);
        bool decoded = pb_decode(&stream, meshtastic_Routing_fields, &routing);

        if (!decoded) {
            LOG_WARN("Tracker: Failed to decode ROUTING_APP payload from 0x%x", mp.from);
        } else if (routing.error_reason == meshtastic_Routing_Error_NONE) {
            // ACK — пакет доставлен, очищаем буфер
            onAckReceived(mp.decoded.request_id);
        } else {
            // NACK — снимаем is_pending немедленно, не ждём таймаут
            LOG_WARN("Tracker: NACK for MsgID=0x%x, error=%d", mp.decoded.request_id, routing.error_reason);
            if (buffer) {
                for (uint16_t i = 0; i < count; i++) {
                    uint16_t idx = (tail + i) % MAX_TRACK_POINTS;
                    if (buffer[idx].msg_id == mp.decoded.request_id) {
                        buffer[idx].is_pending = false;
                        buffer[idx].msg_id = 0;
                        buffer[idx].sent_at_ms = 0;
                    }
                }
            }
        }
    }

    return ProcessMessage::CONTINUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Шлюз: разбор входящего батча и вывод в Serial для Python-скрипта
// ─────────────────────────────────────────────────────────────────────────────
void TrackerModule::handleIncomingTrackData(const meshtastic_MeshPacket &mp)
{
    size_t payloadSize = mp.decoded.payload.size;

    if (payloadSize == 0 || payloadSize % sizeof(TrackPoint) != 0) {
        LOG_WARN("Gateway: Invalid payload size %u from 0x%x", payloadSize, mp.from);
        return;
    }

    size_t n = payloadSize / sizeof(TrackPoint);
    const TrackPoint *pkts = (const TrackPoint *)mp.decoded.payload.bytes;

    for (size_t i = 0; i < n; i++) {
        // Дополнительная валидация каждой точки перед выводом
        if (pkts[i].timestamp < MIN_VALID_TIMESTAMP) {
            LOG_WARN("Gateway: Skipping point with invalid timestamp %u from 0x%x", pkts[i].timestamp, mp.from);
            continue;
        }

        // Вывод в Serial: Python-скрипт читает строки с префиксом @TRACK|
        Serial.printf("@TRACK|{\"dev\":%u,\"ts\":%u,\"lat\":%.7f,\"lon\":%.7f,"
                      "\"alt\":%d,\"bat\":%u,\"rssi\":%d,\"snr\":%.1f}\n",
                      mp.from, pkts[i].timestamp, pkts[i].lat_i * 1e-7, pkts[i].lon_i * 1e-7, pkts[i].alt, pkts[i].battery,
                      mp.rx_rssi, // Добавляем RSSI и SNR — полезно для анализа покрытия
                      mp.rx_snr);
    }

    LOG_INFO("Gateway: Forwarded %u points from 0x%x to Serial", n, mp.from);
}

// ─────────────────────────────────────────────────────────────────────────────
// Спортсмен: обработка ACK — удаление подтверждённых точек из буфера
// ─────────────────────────────────────────────────────────────────────────────
void TrackerModule::onAckReceived(uint32_t msgId)
{
    if (!buffer || count == 0 || msgId == 0)
        return;

    // FIX: Старая логика предполагала, что все точки одного батча лежат
    // подряд начиная с tail — это неверно, если между отправкой и ACK
    // в буфер успели добавиться новые точки (saveCurrentPosition() работает
    // независимо). Теперь проходим весь буфер и удаляем точки по msg_id.
    //
    // Удаление из середины кольцевого буфера нетривиально, поэтому
    // используем «мягкое» удаление: снимаем is_pending и обнуляем msg_id,
    // затем сжимаем tail если подтверждённые точки оказались в начале.

    uint16_t freed = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (tail + i) % MAX_TRACK_POINTS;
        if (buffer[idx].msg_id == msgId && buffer[idx].is_pending) {
            buffer[idx].is_pending = false;
            buffer[idx].msg_id = 0;
            buffer[idx].sent_at_ms = 0;
            buffer[idx].data.status |= 0x80; // Бит 7 = «доставлено», для отладки
            freed++;
        }
    }

    // Сдвигаем tail вперёд через подтверждённые точки в начале очереди,
    // чтобы освободить место в буфере
    while (count > 0 && !buffer[tail].is_pending && buffer[tail].data.status & 0x80) {
        buffer[tail].data.status = 0;
        tail = (tail + 1) % MAX_TRACK_POINTS;
        count--;
    }

    LOG_INFO("Tracker: Batch 0x%x ACKed. Freed=%u, buffer remaining=%u", msgId, freed, count);
}

// ─────────────────────────────────────────────────────────────────────────────
// Основной цикл (вызывается каждые 5 сек)
// ─────────────────────────────────────────────────────────────────────────────
int32_t TrackerModule::runOnce()
{
    // Шлюзу в runOnce() делать нечего — он работает через handleReceived()
    if (_isGateway)
        return 60000;

    if (!buffer) {
        LOG_ERROR("Tracker: No buffer, module disabled");
        return 60000;
    }

    uint32_t now = millis();

    // 1. Сохраняем позицию если нужно
    saveCurrentPosition();

    // 2. Сбрасываем точки, чьё ACK не пришло вовремя (они уйдут повторно)
    expirePendingPoints();

    // 3. Решаем, нужно ли отправлять батч
    bool batchFull = (count >= POINTS_PER_BATCH);
    bool retryNeeded = (count > 0) && (now - lastSendAttemptMs > SEND_RETRY_INTERVAL_MS);
    bool throttleOk = (now - lastSendAttemptMs > SEND_THROTTLE_MS); // FIX: заменили магическое 10000

    if ((batchFull || retryNeeded) && throttleOk) {
        sendBatchToGateway();
    }

    return 5000;
}