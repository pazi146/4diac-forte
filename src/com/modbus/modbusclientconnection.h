/*******************************************************************************
 * Copyright (c) 2012 -2014 AIT
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Filip Andren - initial API and implementation and/or initial documentation
 *******************************************************************************/
#ifndef _MODBUSCLIENTCONNECTION_H_
#define _MODBUSCLIENTCONNECTION_H_

#include "modbusconnection.h"
#include "modbustimedevent.h"
#include "fortelist.h"

class CModbusPoll;

namespace modbus_connection_event {
  class CModbusConnectionEvent : public CModbusTimedEvent{
    public:
      explicit CModbusConnectionEvent(long pa_nReconnectInterval); //ReconnectInterval = 0 => only one connection try
      ~CModbusConnectionEvent() override = default;

      int executeEvent(modbus_t *pa_pModbusConn, void *pa_pRetVal) override;
  };
}

class CModbusClientConnection : public CModbusConnection{
  public:
    explicit CModbusClientConnection(CModbusHandler* pa_modbusHandler);
    ~CModbusClientConnection() override;

    int readData(void *pa_pData) override;
    int writeData(const void *pa_pData, unsigned int pa_nDataSize);
    int connect() override;
    void disconnect() override;

    void addNewPoll(TForteUInt32 pa_nPollInterval, unsigned int pa_nFunctionCode, unsigned int pa_nStartAddress, unsigned int pa_nNrAddresses);
    void addNewSend(unsigned int pa_nSendFuncCode, unsigned int pa_nStartAddress, unsigned int pa_nNrAddresses);

    void setSlaveId(unsigned int pa_nSlaveId);

  protected:
    void run() override;

  private:
    void tryConnect();
    void tryPolling();

    struct SSendInformation {
      unsigned int m_nSendFuncCode;
      unsigned int m_nStartAddress;
      unsigned int m_nNrAddresses;
    };

    modbus_connection_event::CModbusConnectionEvent *m_pModbusConnEvent;

    typedef CSinglyLinkedList<CModbusPoll*> TModbusPollList;
    TModbusPollList m_lstPollList;

    typedef CSinglyLinkedList<SSendInformation> TModbusSendList;
    TModbusSendList m_lstSendList;

    unsigned int m_nNrOfPolls;
    unsigned int m_anRecvBuffPosition[100];

    unsigned int m_nSlaveId;

    uint8_t m_acRecvBuffer[cg_unIPLayerRecvBufferSize];
    unsigned int m_unBufFillSize;

    CSyncObject m_oModbusLock;
};

#endif
