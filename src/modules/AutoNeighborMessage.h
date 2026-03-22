#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

class AutoNeighborMessage : public SinglePortModule, public concurrency::OSThread
{
  public:
    AutoNeighborMessage();

  protected:
    virtual int32_t runOnce() override;

  private:
    uint32_t lastSendTime = 0;
    void sendMessage();
    float calculateDistance(float lat1, float lon1, float lat2, float lon2);
};

extern AutoNeighborMessage *autoNeighborMessage;