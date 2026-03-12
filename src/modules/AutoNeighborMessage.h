#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

/**
 * Модуль автоматической отправки широковещательных сообщений соседям через заданный интервал.
 */
class AutoNeighborMessage : public SinglePortModule, public concurrency::OSThread
{
  public:
    AutoNeighborMessage();

  protected:
    virtual int32_t runOnce() override;
};

extern AutoNeighborMessage *autoNeighborMessage;