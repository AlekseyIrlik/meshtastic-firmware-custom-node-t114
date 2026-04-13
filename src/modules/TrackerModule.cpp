#include "TrackerModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "airtime.h"
#include "configuration.h"
#include "main.h"
#include "memGet.h"
#include "gps/GeoCoord.h"
#include <Throttle.h>
#include <Arduino.h>

TrackerModule *trackerModule;

TrackerModule::TrackerModule()
    : SinglePortModule("tracker", meshtastic_PortNum_PRIVATE_APP),
      concurrency::OSThread("TrackerModule")
{
    gatewayMode = (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER) ||
                  (moduleConfig.store_forward.enabled && moduleConfig.store_forward.is_server);

    deviceId = nodeDB->getNodeNum();
    hasLastPosition = false;
    lastLat = 0;
    lastLon = 0;

    if (!gatewayMode) {
        initBuffer();
        LOG_INFO("TrackerModule: *** ATHLETE MODE *** device_id=0x%08x, save_interval=%ds, max_records=%u, batch_size=%u, hop_limit=%d",
                 deviceId, TRACKER_SAVE_INTERVAL_SEC, maxRecords, TRACKER_BATCH_SIZE, TRACKER_HOP_LIMIT);
    } else {
        records = nullptr;
        maxRecords = 0;
        writeIndex = 0;
        recordCount = 0;
        LOG_INFO("TrackerModule: *** GATEWAY MODE *** ready to receive tracker packets");
    }

    lastSaveMs = 0;
    lastSendAttemptMs = 0;
    setIntervalFromNow(1000);
}

bool TrackerModule::isGatewayMode() const
{
    return gatewayMode;
}

bool TrackerModule::isGatewayAvailable() const
{
    if (gatewayMode) return false;

    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (node && node->has_user && node->user.role == meshtastic_Config_DeviceConfig_Role_ROUTER) {
            if (sinceLastSeen(node) < 7200) {
                LOG_INFO("Tracker: Gateway 0x%08x is ONLINE (last seen %us ago)", node->num, sinceLastSeen(node));
                return true;
            }
        }
    }
    return false;
}

float TrackerModule::calculateDistance(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    return GeoCoord::latLongToMeter(lat1 * 1e-7, lon1 * 1e-7, lat2 * 1e-7, lon2 * 1e-7);
}

void TrackerModule::initBuffer()
{
    maxRecords = TRACKER_MAX_RECORDS;
    size_t bufferSize = maxRecords * sizeof(TrackerRecord);

#if defined(ARCH_ESP32)
    if (memGet.getPsramSize() > 0 && memGet.getFreePsram() >= bufferSize) {
        records = (TrackerRecord *)ps_malloc(bufferSize);
        LOG_INFO("Tracker: Allocated %u records in PSRAM (%u bytes)", maxRecords, bufferSize);
    } else {
        records = (TrackerRecord *)malloc(bufferSize);
        LOG_INFO("Tracker: Allocated %u records in heap (%u bytes)", maxRecords, bufferSize);
    }
#else
    records = (TrackerRecord *)malloc(bufferSize);
    LOG_INFO("Tracker: Allocated %u records in heap (%u bytes)", maxRecords, bufferSize);
#endif

    if (!records) {
        LOG_ERROR("Tracker: CRITICAL - failed to allocate buffer!");
        maxRecords = 0;
    } else {
        memset(records, 0, bufferSize);
    }

    writeIndex = 0;
    recordCount = 0;
}

void TrackerModule::savePosition(int32_t lat, int32_t lon, int32_t alt)
{
    if (gatewayMode || maxRecords == 0 || !records) return;

    uint32_t now = getTime();
    uint8_t battery = powerStatus->getBatteryChargePercent();

    // Расчёт пройденного расстояния
    if (hasLastPosition) {
        float dist = calculateDistance(lastLat, lastLon, lat, lon);
        LOG_INFO("Tracker: MOVEMENT - distance from last saved: %.1f meters", dist);
    } else {
        LOG_INFO("Tracker: First position saved, no distance calculated");
    }

    lastLat = lat;
    lastLon = lon;
    hasLastPosition = true;

    TrackerRecord rec;
    rec.packet.device_id = deviceId;
    rec.packet.timestamp = now;
    rec.packet.latitude_i = lat;
    rec.packet.longitude_i = lon;
    rec.packet.altitude = alt;
    rec.packet.battery = battery;
    rec.acked = false;
    rec.sentPacketId = 0;

    records[writeIndex] = rec;
    writeIndex = (writeIndex + 1) % maxRecords;
    if (recordCount < maxRecords) {
        recordCount++;
    } else {
        LOG_WARN("Tracker: Buffer FULL - overwriting oldest record");
    }

    LOG_INFO("Tracker: SAVED pos #%u: (%.6f, %.6f, %dm) @ %u, batt=%u%% | Buffer: %u/%u records",
             recordCount, lat * 1e-7, lon * 1e-7, alt, now, battery, recordCount, maxRecords);
}

void TrackerModule::sendPendingRecords()
{
    if (gatewayMode || recordCount == 0 || !records) return;

    uint16_t readIdx = (writeIndex + maxRecords - recordCount) % maxRecords;
    std::vector<uint16_t> unackedIndices;
    for (uint16_t i = 0; i < recordCount; i++) {
        if (!records[readIdx].acked && records[readIdx].sentPacketId == 0) {
            unackedIndices.push_back(readIdx);
        }
        readIdx = (readIdx + 1) % maxRecords;
    }

    if (unackedIndices.empty()) {
        LOG_DEBUG("Tracker: No pending records to send");
        return;
    }

    LOG_INFO("Tracker: Sending %u pending records in batches (max %u per packet)", unackedIndices.size(), TRACKER_BATCH_SIZE);

    for (size_t batchStart = 0; batchStart < unackedIndices.size(); batchStart += TRACKER_BATCH_SIZE) {
        size_t batchCount = std::min<size_t>(TRACKER_BATCH_SIZE, unackedIndices.size() - batchStart);
        if (batchCount == 0) break;

        meshtastic_MeshPacket *p = allocDataPacket();
        if (!p) {
            LOG_ERROR("Tracker: Failed to allocate MeshPacket for batch sending");
            return;
        }

        p->to = NODENUM_BROADCAST;
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        p->hop_limit = TRACKER_HOP_LIMIT;
        p->want_ack = true;
        p->decoded.portnum = ourPortNum;

        size_t payloadSize = batchCount * sizeof(TrackerPacket);
        if (payloadSize > sizeof(p->decoded.payload.bytes)) {
            LOG_ERROR("Tracker: Batch size %u exceeds payload buffer!", batchCount);
            packetPool.release(p);
            return;
        }

        uint8_t *ptr = p->decoded.payload.bytes;
        uint16_t firstIdx = unackedIndices[batchStart];

        for (size_t j = 0; j < batchCount; j++) {
            uint16_t idx = unackedIndices[batchStart + j];
            TrackerRecord &rec = records[idx];
            memcpy(ptr, &rec.packet, sizeof(TrackerPacket));
            ptr += sizeof(TrackerPacket);
        }
        p->decoded.payload.size = payloadSize;

        service->sendToMesh(p, RX_SRC_LOCAL, true);

        PendingSend ps;
        ps.requestId = p->id;
        ps.firstRecordIndex = firstIdx;
        ps.recordCount = batchCount;
        pendingSends.push_back(ps);

        for (size_t j = 0; j < batchCount; j++) {
            uint16_t idx = unackedIndices[batchStart + j];
            records[idx].sentPacketId = p->id;
        }

        LOG_INFO("Tracker: Sent batch of %u records, requestId=0x%08x, hop_limit=%d", batchCount, p->id, TRACKER_HOP_LIMIT);
    }
}

ProcessMessage TrackerModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Обработка ACK от Routing модуля
    if (mp.decoded.portnum == meshtastic_PortNum_ROUTING_APP && mp.to == nodeDB->getNodeNum()) {
        uint32_t ackedRequestId = mp.decoded.request_id;
        if (ackedRequestId != 0) {
            // Игнорируем ACK от самого себя (эхо при ретрансляции)
            if (isFromUs(&mp)) {
                LOG_DEBUG("Tracker: Ignoring local ACK (echo) for requestId=0x%08x", ackedRequestId);
                return ProcessMessage::CONTINUE;
            }

            // Проверяем, ждём ли мы подтверждение на этот requestId
            bool found = false;
            for (auto it = pendingSends.begin(); it != pendingSends.end(); ++it) {
                if (it->requestId == ackedRequestId) {
                    LOG_INFO("Tracker: ACK received from 0x%08x for requestId=0x%08x", mp.from, ackedRequestId);
                    markRecordsAcked(ackedRequestId);
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_DEBUG("Tracker: ACK for unknown requestId=0x%08x (from 0x%08x)", ackedRequestId, mp.from);
            }
        }
        return ProcessMessage::CONTINUE;
    }

    // Приём данных треков
    if (mp.decoded.portnum == ourPortNum) {
        const auto &payload = mp.decoded.payload;
        size_t numPackets = payload.size / sizeof(TrackerPacket);
        if (numPackets > 0 && payload.size % sizeof(TrackerPacket) == 0) {
            std::vector<TrackerPacket> packets(numPackets);
            memcpy(packets.data(), payload.bytes, payload.size);

            if (gatewayMode) {
                LOG_INFO("Tracker: GATEWAY received %u tracker packets from 0x%08x", numPackets, mp.from);
                outputToSerial(packets);
            } else {
                LOG_INFO("Tracker: ATHLETE received %u relayed tracker packets from 0x%08x", numPackets, mp.from);
                for (const auto &p : packets) {
                    LOG_DEBUG("Tracker:   -> device 0x%08x, pos (%.6f, %.6f), alt=%dm, batt=%u%%",
                              p.device_id, p.latitude_i * 1e-7, p.longitude_i * 1e-7, p.altitude, p.battery);
                }
            }
        } else {
            LOG_WARN("Tracker: Received malformed payload (size=%d, num=%d, remainder=%d)",
                     payload.size, numPackets, payload.size % sizeof(TrackerPacket));
        }
        return ProcessMessage::STOP;
    }

    return ProcessMessage::CONTINUE;
}

void TrackerModule::markRecordsAcked(uint32_t requestId)
{
    for (auto it = pendingSends.begin(); it != pendingSends.end(); ++it) {
        if (it->requestId == requestId) {
            uint16_t idx = it->firstRecordIndex;
            uint16_t ackedCount = 0;
            for (uint16_t i = 0; i < it->recordCount; i++) {
                if (records[idx].sentPacketId == requestId) {
                    records[idx].acked = true;
                    records[idx].sentPacketId = 0;
                    ackedCount++;
                }
                idx = (idx + 1) % maxRecords;
            }
            LOG_INFO("Tracker: Batch acked: %u/%u records confirmed, requestId=0x%08x",
                     ackedCount, it->recordCount, requestId);
            pendingSends.erase(it);
            cleanupAckedRecords();
            return;
        }
    }
    LOG_WARN("Tracker: Received ACK for unknown requestId=0x%08x", requestId);
}

void TrackerModule::cleanupAckedRecords()
{
    if (recordCount == 0 || maxRecords == 0) return;

    uint16_t readIdx = (writeIndex + maxRecords - recordCount) % maxRecords;
    uint16_t newWriteIdx = readIdx;
    uint16_t newCount = 0;

    for (uint16_t i = 0; i < recordCount; i++) {
        if (!records[readIdx].acked) {
            if (newWriteIdx != readIdx) {
                records[newWriteIdx] = records[readIdx];
            }
            newWriteIdx = (newWriteIdx + 1) % maxRecords;
            newCount++;
        }
        readIdx = (readIdx + 1) % maxRecords;
    }

    uint16_t removed = recordCount - newCount;
    recordCount = newCount;
    writeIndex = newWriteIdx;

    if (removed > 0) {
        LOG_INFO("Tracker: Cleaned %u acked records, buffer now has %u/%u records", removed, recordCount, maxRecords);
    }
}

void TrackerModule::outputToSerial(const std::vector<TrackerPacket> &packets)
{
    for (const auto &p : packets) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"device_id\":%u,\"timestamp\":%u,\"lat\":%.7f,\"lon\":%.7f,\"alt\":%d,\"battery\":%u}",
                 p.device_id, p.timestamp, p.latitude_i * 1e-7, p.longitude_i * 1e-7, p.altitude, p.battery);
        Serial.printf("@TRACK|%s\n", json);
        LOG_DEBUG("Tracker: GATEWAY output: %s", json);
    }
    Serial.flush();
}

int32_t TrackerModule::runOnce()
{
    if (gatewayMode) {
        return INT32_MAX;
    }

    uint32_t now = millis();

    // Сохранение позиции по расписанию
    if (lastSaveMs == 0 || now - lastSaveMs >= TRACKER_SAVE_INTERVAL_SEC * 1000UL) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (node && node->has_position && (node->position.latitude_i != 0 || node->position.longitude_i != 0)) {
            savePosition(node->position.latitude_i, node->position.longitude_i, node->position.altitude);
        } else {
            LOG_DEBUG("Tracker: No valid position to save");
        }
        lastSaveMs = now;
    }

    // Отправка накопленных данных при доступности шлюза или по таймеру повторных попыток
    bool gatewayAvailable = isGatewayAvailable();
    if (gatewayAvailable) {
        LOG_DEBUG("Tracker: Gateway available, attempting to send pending records");
    } else if (now - lastSendAttemptMs >= TRACKER_SEND_RETRY_INTERVAL_SEC * 1000UL) {
        LOG_DEBUG("Tracker: Retry timer expired, attempting to send (gateway not confirmed)");
    }

    if (gatewayAvailable || now - lastSendAttemptMs >= TRACKER_SEND_RETRY_INTERVAL_SEC * 1000UL) {
        if (recordCount > 0) {
            sendPendingRecords();
        }
        lastSendAttemptMs = now;
    }

    return 1000;
}