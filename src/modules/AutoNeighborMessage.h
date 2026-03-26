#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"

/**
 * Модуль автоматической рассылки своей позиции соседям (0-hop).
 * Отправляет пакет Position либо по истечении заданного интервала,
 * либо при перемещении на расстояние больше порога.
 */
class AutoNeighborMessage : public ProtobufModule<meshtastic_Position>, public concurrency::OSThread
{
  public:
    AutoNeighborMessage();

  protected:
    virtual int32_t runOnce() override;
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Position *p) override;

  private:
    uint32_t lastSendTime = 0;
    int32_t lastLat = 0; // храним в формате *1e-7
    int32_t lastLon = 0;
    bool hasLastPos = false;

    void sendPosition(float lat, float lon);
    float calculateDistance(float lat1, float lon1, float lat2, float lon2);
};

extern AutoNeighborMessage *autoNeighborMessage;
