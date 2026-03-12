#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "NodeDB.h" // для nodeDB
#include "configuration.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>

AutoNeighborMessage *autoNeighborMessage;

AutoNeighborMessage::AutoNeighborMessage()
    : SinglePortModule("AutoNeighborMessage", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("AutoNeighborMessage")
{
    LOG_INFO("AutoNeighborMessage module constructed");
}

int32_t AutoNeighborMessage::runOnce()
{
    LOG_INFO("AutoNeighborMessage runOnce started");

    // Получаем последнюю известную позицию из NodeDB
    float lat = 0.0f, lon = 0.0f;
    bool hasPos = false;

    auto node = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (node && node->has_position && node->position.latitude_i != 0 && node->position.longitude_i != 0) {
        lat = node->position.latitude_i / 1e7;
        lon = node->position.longitude_i / 1e7;
        hasPos = true;
        LOG_DEBUG("Using nodeDB position: lat=%f, lon=%f", lat, lon);
    } else {
        LOG_DEBUG("No valid position available");
    }

    // Формируем текстовое сообщение
    char msg[100];
    if (hasPos) {
        snprintf(msg, sizeof(msg), "My position: lat=%.6f, lon=%.6f", lat, lon);
    } else {
        strcpy(msg, "Position unknown");
    }

    // Создаём и отправляем пакет
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("allocDataPacket failed");
        return 30000;
    }

    size_t len = strlen(msg);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, msg, len);
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    service->sendToMesh(p, RX_SRC_LOCAL, false);

    LOG_INFO("Message sent: %s", msg);
    return 30000;
}