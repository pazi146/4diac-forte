/*******************************************************************************
 * Copyright (c) 2017 - 2018 fortiss GmbH
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Johannes Messmer - initial API and implementation and/or initial documentation
 *   Jose Cabral - Cleaning of namespaces
 *******************************************************************************/

#include "io_controller.h"

#include "../processinterface.h"
#include "../configFB/io_configFB_controller.h"
#include "criticalregion.h"

using namespace forte::core::io;

IODeviceController::IODeviceController(CDeviceExecution& paDeviceExecution) :
    CExternalEventHandler(paDeviceExecution), mNotificationType(NotificationType::UnknownNotificationType), mNotificationAttachment(nullptr), mNotificationHandled(true), mError(nullptr),
        mDelegate(nullptr), mInitDelay(0) {
}

void IODeviceController::run() {
  // Delay initialization
  if(mInitDelay > 0) {
    CThread::sleepThread(mInitDelay * 1000);
  }

  mError = init();

  if(!hasError()) {
    notifyConfigFB(NotificationType::Success);

    runLoop();

    if(hasError()) {
      notifyConfigFB(NotificationType::Error, mError);
    }
  } else {
    notifyConfigFB(NotificationType::Error, mError);
  }

  dropHandles();
  deInit();

  while(isAlive()) {
    CThread::sleepThread(10);
  }
}

void IODeviceController::addHandle(HandleDescriptor &paHandleDescriptor) {
  IOHandle* handle = createIOHandle(paHandleDescriptor);

  if(nullptr == handle) {
    DEVLOG_WARNING("[IODeviceController] Failed to initialize handle '%s'. Check initHandle method.\n", paHandleDescriptor.mId.c_str());
    return;
  }

  switch(handle->getDirection()){
    case IOMapper::In:
      addHandle(&mInputHandles, paHandleDescriptor.mId, handle);
      break;
    case IOMapper::Out:
      addHandle(&mOutputHandles, paHandleDescriptor.mId, handle);
      break;
    default:
      break;
  }
}

void IODeviceController::fireIndicationEvent(IOObserver* paObserver) {
  startNewEventChain((CProcessInterface*) paObserver);
}

void IODeviceController::handleChangeEvent(IOHandle*) {
  // EMPTY - Override
}

bool IODeviceController::hasError() const {
  return mError != nullptr;
}

void IODeviceController::notifyConfigFB(NotificationType paType, const void* paAttachment) {
  if(nullptr == mDelegate) {
    DEVLOG_WARNING("[IODeviceController] No receiver for notification is available. Notification is dropped.\n");
    return;
  }

  if(!mNotificationHandled) {
    DEVLOG_WARNING("[IODeviceController] Notification has not yet been handled by the configuration fb. Notification is dropped.\n");
    return;
  }

  this->mNotificationType = paType;
  this->mNotificationAttachment = paAttachment;

  mNotificationHandled = false;
  startNewEventChain(mDelegate);
}

void IODeviceController::checkForInputChanges() {
  CCriticalRegion criticalRegion(mHandleMutex);

  // Iterate over input handles and check for changes
  THandleList::Iterator itEnd = mInputHandles.end();
  for(THandleList::Iterator it = mInputHandles.begin(); it != itEnd; ++it) {
    if((*it)->hasObserver() && !isHandleValueEqual(*it)) {
      // Inform Process Interface about change
      (*it)->onChange();
    }
  }
}

void IODeviceController::setInitDelay(int paDelay) {
  mInitDelay = paDelay;
}

void IODeviceController::dropHandles() {
  CCriticalRegion criticalRegion(mHandleMutex);

  IOMapper& mapper = IOMapper::getInstance();

  THandleList::Iterator itEnd = mInputHandles.end();
  for(THandleList::Iterator it = mInputHandles.begin(); it != itEnd; ++it) {
    mapper.deregisterHandle(*it);
    delete *it;
  }
  itEnd = mOutputHandles.end();
  for(THandleList::Iterator it = mOutputHandles.begin(); it != itEnd; ++it) {
    mapper.deregisterHandle(*it);
    delete *it;
  }

  mInputHandles.clearAll();
  mOutputHandles.clearAll();

}

bool IODeviceController::isHandleValueEqual(IOHandle*) {
  return true;
}

void IODeviceController::addHandle(THandleList* paList, std::string const &paId, IOHandle* paHandle) {
  if(!paId.empty() && IOMapper::getInstance().registerHandle(paId, paHandle)) {
    CCriticalRegion criticalRegion(mHandleMutex);
    paList->pushBack(paHandle);
  } else {
    delete paHandle;
  }
}

