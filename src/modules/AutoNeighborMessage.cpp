#include "AutoNeighborMessage.h"
#include "MeshService.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include <Arduino.h>

// Глобальный указатель для доступа из других частей прошивки
AutoNeighborMessage *autoNeighborMessage;

AutoNeighborMessage::AutoNeighborMessage()
    : SinglePortModule("AutoNeighborMessage", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("AutoNeighborMessage")
{
    // Логируем создание модуля
    LOG_INFO("AutoNeighborMessage module constructed");
}

int32_t AutoNeighborMessage::runOnce()
{
    LOG_INFO("AutoNeighborMessage runOnce started");

    // 1. Выделяем пакет данных через SinglePortModule::allocDataPacket()
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("allocDataPacket failed");
        return 30000; // повторим через 30 секунд
    }

    // 2. Заполняем полезную нагрузку текстом
    const char *msg = "Hello neighbors!";
    size_t len = strlen(msg);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, msg, len);

    // 3. Порт уже установлен в allocDataPacket(), но для ясности можно указать явно
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    // 4. Отправляем в Mesh-сеть
    //    service - глобальный указатель (extern MeshService *service;)
    //    Если service - объект, замените -> на .
    service->sendToMesh(p, RX_SRC_LOCAL, false);

    LOG_INFO("Message sent: %s", msg);

    // 5. Возвращаем интервал до следующего запуска (30 секунд)
    return 1000;
}