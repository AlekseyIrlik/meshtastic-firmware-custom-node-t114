#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>
#include <math.h>

AutoNeighborMessage *autoNeighborMessage;

AutoNeighborMessage::AutoNeighborMessage()
    : SinglePortModule("AutoNeighborMessage", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("AutoNeighborMessage")
{
    LOG_INFO("AutoNeighborMessage module constructed");
}

float AutoNeighborMessage::calculateDistance(float lat1, float lon1, float lat2, float lon2)
{
    const float R = 6371000; // радиус Земли в метрах
    float dlat = (lat2 - lat1) * M_PI / 180.0;
    float dlon = (lon2 - lon1) * M_PI / 180.0;
    float a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    float c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

void AutoNeighborMessage::sendMessage()
{
    float lat = 0.0f, lon = 0.0f;
    bool hasPos = false;

    auto node = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (node && node->has_position && node->position.latitude_i != 0 && node->position.longitude_i != 0) {
        lat = node->position.latitude_i / 1e7;
        lon = node->position.longitude_i / 1e7;
        hasPos = true;
    }

    char msg[100];
    if (hasPos) {
        snprintf(msg, sizeof(msg), "My position: lat=%.6f, lon=%.6f", lat, lon);
    } else {
        strcpy(msg, "Position unknown");
    }

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("allocDataPacket failed");
        return;
    }

    size_t len = strlen(msg);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, msg, len);
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    service->sendToMesh(p, RX_SRC_LOCAL, false);

    LOG_INFO("Message sent: %s", msg);
    lastSendTime = millis();
}

int32_t AutoNeighborMessage::runOnce()
{
    uint32_t sendIntervalSecs = 300; // 5 минут
    uint32_t distanceThresholdM = 0; // 0 = отключено

    // TODO:
    // if (moduleConfig.has_auto_neighbor_message) {
    //     sendIntervalSecs = moduleConfig.auto_neighbor_message.send_interval_secs;
    //     distanceThresholdM = moduleConfig.auto_neighbor_message.distance_threshold_m;
    // }

    uint32_t now = millis();
    bool shouldSend = false;

    // Периодическая отправка
    if (sendIntervalSecs > 0 && (now - lastSendTime) >= (sendIntervalSecs * 1000UL)) {
        shouldSend = true;
    }

    // Отправка при превышении расстояния до любого соседа
    if (distanceThresholdM > 0) {
        auto myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (myNode && myNode->has_position && myNode->position.latitude_i != 0 && myNode->position.longitude_i != 0) {
            float myLat = myNode->position.latitude_i / 1e7;
            float myLon = myNode->position.longitude_i / 1e7;

            for (size_t i = 0; i < nodeDB->numMeshNodes; i++) {
                auto *node = nodeDB->getMeshNodeByIndex(i);
                if (node->num == nodeDB->getNodeNum())
                    continue;

                if (node->has_position && node->position.latitude_i != 0 && node->position.longitude_i != 0) {
                    float lat = node->position.latitude_i / 1e7;
                    float lon = node->position.longitude_i / 1e7;
                    float dist = calculateDistance(myLat, myLon, lat, lon);
                    if (dist > distanceThresholdM) {
                        LOG_DEBUG("Distance to node 0x%x = %.0f m exceeds threshold", node->num, dist);
                        shouldSend = true;
                        break;
                    }
                }
            }
        }
    }

    if (shouldSend) {
        sendMessage();
    }

    // Возвращаем интервал до следующего вызова: 10 секунд при активной проверке расстояния, иначе 60 секунд
    return (distanceThresholdM > 0) ? 10000 : 60000;
}