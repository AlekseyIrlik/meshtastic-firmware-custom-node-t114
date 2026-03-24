#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "gps/GeoCoord.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>

AutoNeighborMessage *autoNeighborMessage = nullptr;

AutoNeighborMessage::AutoNeighborMessage()
    : SinglePortModule("AutoNeighborMessage", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("AutoNeighborMessage")
{
    LOG_INFO("AutoNeighborMessage constructed");
}

float AutoNeighborMessage::calculateDistance(float lat1, float lon1, float lat2, float lon2)
{
    return GeoCoord::latLongToMeter(lat1, lon1, lat2, lon2);
}

void AutoNeighborMessage::sendMessage(float lat, float lon)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "POS: %.6f, %.6f", lat, lon);

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("allocDataPacket failed");
        return;
    }

    size_t len = strlen(msg);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, msg, len);
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    service->sendToMesh(p);

    lastSendTime = millis();
    lastLat = lat;
    lastLon = lon;
    hasLastPos = true;

    LOG_INFO("Sent position: %.6f %.6f", lat, lon);
}

int32_t AutoNeighborMessage::runOnce()
{
    constexpr float DIST_THRESHOLD_M = 20.0f;        // нормальный GPS порог
    constexpr uint32_t TIME_INTERVAL_MS = 1800000UL; // 30 мин

    auto myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!myNode || !myNode->has_position) {
        return 10000;
    }

    float lat = myNode->position.latitude_i / 1e7f;
    float lon = myNode->position.longitude_i / 1e7f;

    bool shouldSend = false;

    // движение
    if (!hasLastPos) {
        LOG_INFO("First position send");
        shouldSend = true;
    }

    else {
        float dist = calculateDistance(lat, lon, lastLat, lastLon);
        if (dist > DIST_THRESHOLD_M) {
            LOG_INFO("Moved %.1f meters", dist);
            shouldSend = true;
        }
    }

    uint32_t now = millis();
    if (!shouldSend && (now - lastSendTime) > TIME_INTERVAL_MS) {
        LOG_INFO("Timer triggered send");
        shouldSend = true;
    }

    if (shouldSend) {
        sendMessage(lat, lon);
    }

    return 10000; // проверка каждые 10 сек
}