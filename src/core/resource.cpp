/*******************************************************************************
 * Copyright (c) 2005, 2023 ACIN, Profactor GmbH, fortiss GmbH,
 *                          Johannes Kepler University,
 *                          Primetals Technologies Austria GmbH,
 *                          Martin Erich Jobst
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *    Alois Zoitl, Rene Smodic, Gerhard Ebenhofer, Thomas Strasser,
 *    Martin Melik Merkumians,
 *      - initial implementation and rework communication infrastructure
 *    Martin Melik Merkumians - modify for removed implicit constructors
 *         for primitive types
 *    Martin Jobst - add CTF tracing integration
 *    Fabio Gandolfi - send also subapps on requested resources
 *******************************************************************************/
#include "fortenew.h"
#include "resource.h"
#ifdef FORTE_ENABLE_GENERATED_SOURCE_CPP
#include "resource_gen.cpp"
#endif
#include "device.h"
#include "adapter.h"
#include "adapterconn.h"
#include "if2indco.h"
#include "utils/criticalregion.h"
#include "utils/fixedcapvector.h"
#include "ecet.h"

#ifdef FORTE_DYNAMIC_TYPE_LOAD
#include "lua/luaengine.h"
#include "lua/luacfbtypeentry.h"
#include "lua/luabfbtypeentry.h"
#include "lua/luaadaptertypeentry.h"
#endif

#include <string>

using namespace std::string_literals;

CResource::CResource(forte::core::CFBContainer &paDevice, const SFBInterfaceSpec *paInterfaceSpec, const CStringDictionary::TStringId paInstanceNameId) :
    CFunctionBlock(paDevice, paInterfaceSpec, paInstanceNameId), forte::core::CFBContainer(CStringDictionary::scmInvalidStringId, paDevice), // the fbcontainer of resources does not have a seperate name as it is stored in the resource
    mResourceEventExecution(CEventChainExecutionThread::createEcet()), mResIf2InConnections(nullptr)
#ifdef FORTE_SUPPORT_MONITORING
, mMonitoringHandler(*this)
#endif
#ifdef FORTE_TRACE_CTF
, tracePlatformContext(paInstanceNameId, FORTE_TRACE_CTF_BUFFER_SIZE)
#endif
{}

CResource::CResource(const SFBInterfaceSpec *paInterfaceSpec, const CStringDictionary::TStringId paInstanceNameId) :
    CFunctionBlock(*this, paInterfaceSpec, paInstanceNameId), forte::core::CFBContainer(CStringDictionary::scmInvalidStringId, *this), // the fbcontainer of resources does not have a seperate name as it is stored in the resource
    mResourceEventExecution(nullptr), mResIf2InConnections(nullptr)
#ifdef FORTE_SUPPORT_MONITORING
, mMonitoringHandler(*this)
#endif
#ifdef FORTE_TRACE_CTF
, tracePlatformContext(paInstanceNameId, FORTE_TRACE_CTF_BUFFER_SIZE)
#endif
{}

bool CResource::initialize() {
  if(!CFunctionBlock::initialize()) {
    return false;
  }
#ifdef FORTE_DYNAMIC_TYPE_LOAD
  luaEngine = new CLuaEngine();
#endif
  initializeResIf2InConnections();
  return true;
}

CResource::~CResource(){
#ifdef FORTE_DYNAMIC_TYPE_LOAD
  delete luaEngine;
#endif
  delete mResourceEventExecution;
  delete[] mResIf2InConnections;
}

EMGMResponse CResource::executeMGMCommand(forte::core::SManagementCMD &paCommand){
  EMGMResponse retVal = EMGMResponse::InvalidDst;

  if(CStringDictionary::scmInvalidStringId == paCommand.mDestination){
    switch (paCommand.mCMD){
      case EMGMCommandType::CreateFBInstance: {
        forte::core::TNameIdentifier::CIterator itRunner(paCommand.mFirstParam.begin());
        retVal = createFB(itRunner, paCommand.mSecondParam.front());
      }
        break;
      case EMGMCommandType::CreateFBType:
#ifdef FORTE_DYNAMIC_TYPE_LOAD
        retVal = createFBTypeFromLua(paCommand.mFirstParam.front(), paCommand.mAdditionalParams.c_str());
#else
        retVal = EMGMResponse::UnsupportedCmd;
#endif
        break;
      case EMGMCommandType::CreateAdapterType:
#ifdef FORTE_DYNAMIC_TYPE_LOAD
        retVal = createAdapterTypeFromLua(paCommand.mFirstParam.front(), paCommand.mAdditionalParams.c_str());
#else
        retVal = EMGMResponse::UnsupportedCmd;
#endif
        break;
      case EMGMCommandType::DeleteFBInstance: {
        forte::core::TNameIdentifier::CIterator itRunner(paCommand.mFirstParam.begin());
        retVal = deleteFB(itRunner);
      }
        break;
      case EMGMCommandType::CreateConnection:
        retVal = createConnection(paCommand.mFirstParam, paCommand.mSecondParam);
        break;
      case EMGMCommandType::DeleteConnection:
        retVal = deleteConnection(paCommand.mFirstParam, paCommand.mSecondParam);
        break;
      case EMGMCommandType::Read:
        retVal = readValue(paCommand.mFirstParam, paCommand.mAdditionalParams);
        break;
      case EMGMCommandType::Write:
        retVal = writeValue(paCommand.mFirstParam, paCommand.mAdditionalParams);
        break;
      case EMGMCommandType::Start:
      case EMGMCommandType::Stop:
      case EMGMCommandType::Kill:
      case EMGMCommandType::Reset:
        retVal = handleExecutionStateCmd(paCommand.mCMD, paCommand.mFirstParam);
        break;
#ifdef FORTE_SUPPORT_QUERY_CMD
        case EMGMCommandType::QueryFBTypes:
        retVal = queryAllFBTypes(paCommand.mAdditionalParams);
        break;
        case EMGMCommandType::QueryAdapterTypes:
        retVal = queryAllAdapterTypes(paCommand.mAdditionalParams);
        break;
        case EMGMCommandType::QueryFB:
        retVal = queryFBs(paCommand.mAdditionalParams);
        break;
        case EMGMCommandType::QueryFBType:
        retVal = createFBTypeResponseMessage(paCommand.mFirstParam.front(), paCommand.mAdditionalParams);
        break;
        case EMGMCommandType::QueryAdapterType:
        retVal = createAdapterTypeResponseMessage(paCommand.mFirstParam.front(), paCommand.mAdditionalParams);
        break;
        case EMGMCommandType::QueryConnection:
        retVal = queryConnections(paCommand.mAdditionalParams, *this);
        break;
#endif //FORTE_SUPPORT_QUERY_CMD
      default:
#ifdef FORTE_SUPPORT_MONITORING
        retVal = mMonitoringHandler.executeMonitoringCommand(paCommand);
#else
        retVal = EMGMResponse::UnsupportedCmd;
#endif
        break;
    }
  }
  return retVal;
}

EMGMResponse CResource::changeFBExecutionState(EMGMCommandType paCommand){
  EMGMResponse retVal = CFunctionBlock::changeFBExecutionState(paCommand);
  if(EMGMResponse::Ready == retVal){
    retVal = changeContainedFBsExecutionState(paCommand);
    if(EMGMResponse::Ready == retVal){
      if(EMGMCommandType::Start == paCommand && nullptr != mInterfaceSpec) { //on start, sample inputs
        for(TPortId i = 0; i < mInterfaceSpec->mNumDIs; ++i) {
          CDataConnection *conn = *getDIConUnchecked(i);
          if(nullptr != conn) {
            conn->readData(*getDI(i));
          }
        }
      }
      if(nullptr != mResourceEventExecution){
        // if we have a mResourceEventExecution handle it
        mResourceEventExecution->changeExecutionState(paCommand);
      }
    }
  }
  return retVal;
}

EMGMResponse CResource::handleExecutionStateCmd(EMGMCommandType paCMD, forte::core::TNameIdentifier &paTarget){
  EMGMResponse retVal = EMGMResponse::NoSuchObject;
  CFunctionBlock *fb = this;

  if(!paTarget.isEmpty()){
    forte::core::TNameIdentifier::CIterator itRunner(paTarget.begin());
    fb = getContainedFB(itRunner);
  }

  if(nullptr != fb){
    retVal = fb->changeFBExecutionState(paCMD);
  }
  return retVal;
}

EMGMResponse CResource::createConnection(forte::core::SManagementCMD &paCommand){
  return createConnection(paCommand.mFirstParam, paCommand.mSecondParam);
}

EMGMResponse CResource::createConnection(forte::core::TNameIdentifier &paSrcNameList, forte::core::TNameIdentifier &paDstNameList){
  EMGMResponse retVal = EMGMResponse::NoSuchObject;

  CConnection *const con = getConnection(paSrcNameList);
  if(nullptr != con){
    CStringDictionary::TStringId portName = paDstNameList.back();
    paDstNameList.popBack();
    forte::core::TNameIdentifier::CIterator runner(paDstNameList.begin());
    CFunctionBlock *dstFB = getContainedFB(runner);
    if((nullptr != dstFB) && (runner.isLastEntry())){
      retVal = con->connect(dstFB, portName);
    }
  }

  return retVal;
}

EMGMResponse CResource::deleteConnection(forte::core::TNameIdentifier &paSrcNameList, forte::core::TNameIdentifier &paDstNameList){
  EMGMResponse retVal = EMGMResponse::NoSuchObject;

  CConnection *const con = getConnection(paSrcNameList);
  if(nullptr != con){
    CStringDictionary::TStringId portName = paDstNameList.back();
    paDstNameList.popBack();
    forte::core::TNameIdentifier::CIterator runner(paDstNameList.begin());
    CFunctionBlock *dstFB = getContainedFB(runner);
    if((nullptr != dstFB) && (runner.isLastEntry())){
      retVal = con->disconnect(dstFB, portName);
    }
  }

  return retVal;
}

EMGMResponse CResource::writeValue(forte::core::TNameIdentifier &paNameList, const CIEC_STRING & paValue, bool paForce){
  EMGMResponse retVal = EMGMResponse::NoSuchObject;

  CStringDictionary::TStringId portName = paNameList.back();
  paNameList.popBack();
  forte::core::TNameIdentifier::CIterator runner(paNameList.begin());

  CFunctionBlock *fb = this;
  if(paNameList.size() >= 1){
    //this is not an identifier for the resource interface
    fb = getContainedFB(runner); // the last entry is the input name therefore reduce list here by one
  }

  if((nullptr != fb) && (runner.isLastEntry())){
    CIEC_ANY *const var = fb->getVar(&portName, 1);
    if(nullptr != var){
      // 0 is not supported in the fromString method
      if((paValue.length() > 0) && (paValue.length() == var->fromString(paValue.getStorage().c_str()))){
        //if we cannot parse the full value the value is not valid
        if(paForce){
          var->setForced(true);
          CDataConnection *con = fb->getDOConnection(portName);
          if(nullptr != con){
            //if we have got a connection it was a DO mirror the forced value there
            con->writeData(*var);
          }
        }
        retVal = EMGMResponse::Ready;
      }
      else{
        retVal = EMGMResponse::BadParams;
      }
    }
  }
  return retVal;
}

EMGMResponse CResource::readValue(forte::core::TNameIdentifier &paNameList, CIEC_STRING & paValue){
  EMGMResponse retVal = EMGMResponse::NoSuchObject;
  CIEC_ANY *const var = getVariable(paNameList);
  if(nullptr != var){
    int nUsedChars = -1;
    switch (var->getDataTypeID()){
      case CIEC_ANY::e_WSTRING: {
        const size_t bufferSize = var->getToStringBufferSize() + forte::core::util::getExtraSizeForXMLEscapedChars(static_cast<CIEC_WSTRING&>(*var).getValue());
        char *buffer = new char[bufferSize]();
        nUsedChars = static_cast<CIEC_WSTRING &>(*var).toUTF8(buffer, bufferSize, false);
        if(bufferSize != var->getToStringBufferSize() && 0 < nUsedChars) { //avoid re-running on strings which were already proven not to have any special character
          nUsedChars += static_cast<int>(forte::core::util::transformNonEscapedToEscapedXMLText(buffer));
        }
        if(-1 < nUsedChars && static_cast<size_t>(nUsedChars) < CIEC_STRING::scmMaxStringLen){
          paValue.assign(buffer, static_cast<TForteUInt16>(nUsedChars));
        }
        delete[] (buffer);
        break;
      }
      case CIEC_ANY::e_STRING: {
        const size_t bufferSize = var->getToStringBufferSize() + forte::core::util::getExtraSizeForXMLEscapedChars(static_cast<CIEC_STRING&>(*var).getStorage().c_str());
        char *buffer = new char[bufferSize]();
        nUsedChars = static_cast<CIEC_STRING &>(*var).toUTF8(buffer, bufferSize, false);
        if(bufferSize != var->getToStringBufferSize() && 0 < nUsedChars) { //avoid re-running on strings which were already proven not to have any special character
          nUsedChars += static_cast<int>(forte::core::util::transformNonEscapedToEscapedXMLText(buffer));
        }
        if(-1 < nUsedChars && static_cast<size_t>(nUsedChars) < CIEC_STRING::scmMaxStringLen){
          paValue.assign(buffer, static_cast<TForteUInt16>(nUsedChars));
        }
        delete[] (buffer);
        break;
      }
      default:
        const size_t bufferSize = var->getToStringBufferSize();
        char *buffer = new char[bufferSize]();
        nUsedChars = var->toString(buffer, sizeof(buffer));
        if(-1 < nUsedChars && static_cast<size_t>(nUsedChars) < CIEC_STRING::scmMaxStringLen){
          paValue.assign(buffer, static_cast<TForteUInt16>(nUsedChars));
        }
        delete[] (buffer);
        break;
    }

    if(-1 != nUsedChars){
      retVal = EMGMResponse::Ready;
    }
    else{
      retVal = EMGMResponse::InvalidObject;
    }
  }
  return retVal;
}

#ifdef FORTE_SUPPORT_QUERY_CMD

EMGMResponse CResource::queryAllFBTypes(CIEC_STRING & paValue){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;

  CTypeLib::CTypeEntry *fbTypeRunner = CTypeLib::getFBLibStart();
  if(fbTypeRunner != nullptr){
    retVal = EMGMResponse::Ready;
    for(; fbTypeRunner != nullptr; fbTypeRunner = fbTypeRunner->mNext){
      paValue.append(CStringDictionary::getInstance().get(fbTypeRunner->getTypeNameId()));
      if(fbTypeRunner->mNext != nullptr){
        paValue.append(", ");
      }
    }
  }
  return retVal;
}

EMGMResponse CResource::queryAllAdapterTypes(CIEC_STRING & paValue){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;

  CTypeLib::CTypeEntry *adapterTypeRunner = CTypeLib::getAdapterLibStart();
  if(adapterTypeRunner != nullptr){
    retVal = EMGMResponse::Ready;
    for(; adapterTypeRunner != nullptr; adapterTypeRunner = adapterTypeRunner->mNext){
      paValue.append(CStringDictionary::getInstance().get(adapterTypeRunner->getTypeNameId()));
      if(adapterTypeRunner->mNext != nullptr){
        paValue.append(", ");
      }
    }
  }
  return retVal;
}

EMGMResponse CResource::queryFBs(CIEC_STRING & paValue){

  for(TFunctionBlockList::iterator itRunner = getFBList().begin(); itRunner != getFBList().end(); ++itRunner){
    if(itRunner != getFBList().begin()){
      paValue.append("\n");
    }
    createFBResponseMessage(*static_cast<CFunctionBlock *>(*itRunner) ,(static_cast<CFunctionBlock *>(*itRunner))->getInstanceName(),paValue);
  }

  return querySubapps(paValue, *this, "");
}


EMGMResponse CResource::querySubapps(CIEC_STRING & paValue, CFBContainer& container, const std::string prefix){

  for(TFBContainerList::iterator itRunner(container.getSubContainerList().begin()); itRunner != container.getSubContainerList().end(); ++itRunner){
    CFBContainer* subapp = (static_cast<CFBContainer*>(*itRunner));
    std::string subapp_prefix = prefix;

    if(!prefix.empty()){
      subapp_prefix += ".";
    }
    subapp_prefix += subapp->getName();

    for(TFunctionBlockList::iterator itRunner2 = subapp->getFBList().begin(); itRunner2 != subapp->getFBList().end(); ++itRunner2){

      std::string fullFBName = (static_cast<CFunctionBlock *>(*itRunner2))->getInstanceName();
      fullFBName = "." + fullFBName;
      fullFBName = subapp_prefix + fullFBName;

      if(itRunner2 != getFBList().begin()){
        paValue.append("\n");
      }

      createFBResponseMessage(*static_cast<CFunctionBlock *>(*itRunner2) ,fullFBName.c_str(),paValue);
    }
    querySubapps(paValue,*subapp,subapp_prefix);
  }
  return EMGMResponse::Ready;
}

EMGMResponse CResource::queryConnections(CIEC_STRING & paReqResult, CFBContainer& container){

  EMGMResponse retVal = EMGMResponse::UnsupportedType;
  for(TFunctionBlockList::iterator itRunner2 = container.getFBList().begin(); itRunner2 != container.getFBList().end(); ++itRunner2){
    createEOConnectionResponse(**itRunner2, paReqResult);
    createDOConnectionResponse(**itRunner2, paReqResult);
    createAOConnectionResponse(**itRunner2, paReqResult);
  }

  for(TFBContainerList::iterator itRunner(container.getSubContainerList().begin()); itRunner != container.getSubContainerList().end(); ++itRunner) {
    CFBContainer* subapp = (static_cast<CFBContainer*>(*itRunner));
    queryConnections(paReqResult,*subapp);
  }
  retVal = EMGMResponse::Ready;
  return retVal;
}

void CResource::createEOConnectionResponse(const CFunctionBlock& paFb, CIEC_STRING& paReqResult){
  const SFBInterfaceSpec * const spec = paFb.getFBInterfaceSpec();
  if(spec->mNumEOs > 0){
    for(size_t i = 0; spec->mEONames[i] != spec->mEONames[spec->mNumEOs]; i++){
      const CEventConnection* eConn = paFb.getEOConnection(spec->mEONames[i]);
      for(const auto& it : eConn->getDestinationList()){
        if(it != eConn->getDestinationList().front()){
          paReqResult.append("\n");
        }
        createConnectionResponseMessage(spec->mEONames[i], it.mFB->getFBInterfaceSpec()->mEINames[it.mPortId], *it.mFB, paFb, paReqResult);
      }
    }
  }
}

void CResource::createDOConnectionResponse(const CFunctionBlock& paFb, CIEC_STRING& paReqResult){
  const SFBInterfaceSpec * const spec = paFb.getFBInterfaceSpec();
  if(spec->mNumDOs > 0){
    for(size_t i = 0; spec->mDONames[i] != spec->mDONames[spec->mNumDOs]; i++){
      const CDataConnection * const dConn = paFb.getDOConnection(spec->mDONames[i]);
      for(const auto& it : dConn->getDestinationList()){
        if(it != dConn->getDestinationList().front()){
          paReqResult.append("\n");
        }
        createConnectionResponseMessage(spec->mDONames[i], it.mFB->getFBInterfaceSpec()->mDINames[it.mPortId], *it.mFB, paFb, paReqResult);
      }
    }
  }
}

void CResource::createAOConnectionResponse(const CFunctionBlock& paFb, CIEC_STRING& paReqResult){
  const SFBInterfaceSpec * const spec = paFb.getFBInterfaceSpec();
  if(spec->mNumAdapters > 0){
    for(size_t i = 0; i < spec->mNumAdapters; i++){
      const CAdapter * const adapter = paFb.getAdapter(spec->mAdapterInstanceDefinition[i].mAdapterNameID);
      const CAdapterConnection* aConn = adapter->getAdapterConnection();
      if(spec->mAdapterInstanceDefinition[i].mIsPlug && nullptr != aConn){
        if(i != 0){
          paReqResult.append("\n");
        }
        if(!aConn->isEmpty()){
          const auto & dest = aConn->getDestinationList().front();
          createConnectionResponseMessage(spec->mAdapterInstanceDefinition[i].mAdapterNameID,
              dest.mFB->getFBInterfaceSpec()->mAdapterInstanceDefinition[dest.mPortId].mAdapterNameID, *dest.mFB, paFb,
              paReqResult);
        }
      }
    }
  }
}

void CResource::createFBResponseMessage(const CFunctionBlock& paFb, const char* fullName, CIEC_STRING& paValue){
  paValue.append("<FB name=\"");
  paValue.append(fullName);
  paValue.append("\" type=\"");
  paValue.append(CStringDictionary::getInstance().get(paFb.getFBTypeId()));
  paValue.append("\"/>");
}

void CResource::createConnectionResponseMessage(const CStringDictionary::TStringId srcId, const CStringDictionary::TStringId dstId,
    const CFunctionBlock& paDstFb, const CFunctionBlock& paSrcFb, CIEC_STRING& paReqResult) const {
  paReqResult.append("<Connection Source=\""s);

  std::string fullName;
  fullName.reserve(cgStringInitialSize);
  fullName = paSrcFb.getInstanceName();
  fullName += "."s;
  fullName += CStringDictionary::getInstance().get(srcId);

  CFBContainer* parent = &(paSrcFb.getContainer());
  const CDevice *dev = getDevice();

  if(paSrcFb.getInstanceNameId() != g_nStringIdSTART ){
    while(parent != dev && parent->getName() != 0){
      fullName.insert(0, "."s);
      fullName.insert(0, parent->getName());
      parent = &parent->getParent();
    }
  }
  paReqResult.append(fullName);
  paReqResult.append("\" Destination=\""s);
  fullName = paDstFb.getInstanceName();
  fullName += "."s;
  fullName += CStringDictionary::getInstance().get(dstId);

  parent = &(paDstFb.getContainer());
  while(parent != dev && parent->getName() != 0){
    fullName.insert(0, "."s);
    fullName.insert(0, parent->getName());
    parent = &parent->getParent();
  }
  fullName.shrink_to_fit();
  paReqResult.append(fullName);
  paReqResult.append("\"/>"s);
}

EMGMResponse CResource::createFBTypeResponseMessage(const CStringDictionary::TStringId paValue, CIEC_STRING & paReqResult){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;
  CTypeLib::CFBTypeEntry* fbType = static_cast<CTypeLib::CFBTypeEntry *>(CTypeLib::findType(paValue, CTypeLib::getFBLibStart()));
  if(nullptr != fbType){
    retVal = createXTypeResponseMessage(fbType, paValue, retVal, paReqResult);
  }
  return retVal;
}

EMGMResponse CResource::createAdapterTypeResponseMessage(const CStringDictionary::TStringId paValue, CIEC_STRING & paReqResult){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;
  CTypeLib::CAdapterTypeEntry* adapterType = static_cast<CTypeLib::CAdapterTypeEntry *>(CTypeLib::findType(paValue, CTypeLib::getAdapterLibStart()));
  if(nullptr != adapterType){
    retVal = createXTypeResponseMessage(adapterType, paValue, retVal, paReqResult);
  }
  return retVal;
}

EMGMResponse  CResource::createXTypeResponseMessage(const CTypeLib::CSpecTypeEntry* paTypeEntry, const CStringDictionary::TStringId paValue, EMGMResponse retVal, CIEC_STRING& paReqResult){
  const SFBInterfaceSpec* paInterfaceSpec = paTypeEntry->getInterfaceSpec();
  if(nullptr != paInterfaceSpec){
    paReqResult.append("Name=\"");
    paReqResult.append(CStringDictionary::getInstance().get(paValue));
    paReqResult.append("\">\n    <InterfaceList>\n      ");
    createEventInterfaceResponseMessage(paInterfaceSpec, paReqResult);
    createDataInterfaceResponseMessage(paInterfaceSpec, paReqResult);
    createAdapterInterfaceResponseMessage(paInterfaceSpec, paReqResult);
    paReqResult.append("</InterfaceList>\n");
    retVal = EMGMResponse::Ready;
  }
  return retVal;
}

void CResource::createEventInterfaceResponseMessage(const SFBInterfaceSpec* paInterfaceSpec, CIEC_STRING& paReqResult){
  if(paInterfaceSpec->mNumEIs > 0){
    paReqResult.append("<EventInputs>\n         ");
    createInterfaceResponseMessages(paReqResult, "Event", paInterfaceSpec->mEINames, nullptr, paInterfaceSpec->mNumEIs, paInterfaceSpec->mEIWith, paInterfaceSpec->mEIWithIndexes, paInterfaceSpec->mDINames);
    paReqResult.append("</EventInputs>\n   ");
  }
  if(paInterfaceSpec->mNumEOs > 0){
    paReqResult.append("<EventOutputs>\n         ");
    createInterfaceResponseMessages(paReqResult, "Event", paInterfaceSpec->mEONames, nullptr, paInterfaceSpec->mNumEOs, paInterfaceSpec->mEOWith, paInterfaceSpec->mEOWithIndexes, paInterfaceSpec->mDONames);
    paReqResult.append("</EventOutputs>\n   ");
  }
}

void CResource::createDataInterfaceResponseMessage(const SFBInterfaceSpec* paInterfaceSpec, CIEC_STRING& paReqResult){
  if(paInterfaceSpec->mNumDIs > 0){
    paReqResult.append("<InputVars>\n         ");
    createInterfaceResponseMessages(paReqResult, "VarDeclaration", paInterfaceSpec->mDINames, paInterfaceSpec->mDIDataTypeNames, paInterfaceSpec->mNumDIs);
    paReqResult.append("</InputVars>\n   ");
  }
  if(paInterfaceSpec->mNumDOs > 0){
    paReqResult.append("<OutputVars>\n         ");
    createInterfaceResponseMessages(paReqResult, "VarDeclaration", paInterfaceSpec->mDONames, paInterfaceSpec->mDODataTypeNames, paInterfaceSpec->mNumDOs);
    paReqResult.append("</OutputVars>\n   ");
  }
}

void CResource::createAdapterInterfaceResponseMessage(const SFBInterfaceSpec* paInterfaceSpec, CIEC_STRING& paReqResult){
  if(paInterfaceSpec->mNumAdapters > 0){
    CIEC_STRING sockets;
    CIEC_STRING plugs;
    for(TPortId i = 0; i < paInterfaceSpec->mNumAdapters; i++){
      if(paInterfaceSpec->mAdapterInstanceDefinition[i].mIsPlug){
        const char *adapterName = CStringDictionary::getInstance().get(paInterfaceSpec->mAdapterInstanceDefinition[i].mAdapterNameID);
        const char *adapterTypeName = CStringDictionary::getInstance().get(paInterfaceSpec->mAdapterInstanceDefinition[i].mAdapterTypeNameID);
        createInterfaceResponseMessage(plugs, "AdapterDeclaration", CIEC_STRING(adapterName, strlen(adapterName)), CIEC_STRING(adapterTypeName, strlen(adapterTypeName)));
      }
      else{
        const char *adapterName = CStringDictionary::getInstance().get(paInterfaceSpec->mAdapterInstanceDefinition[i].mAdapterNameID);
        const char *adapterTypeName = CStringDictionary::getInstance().get(paInterfaceSpec->mAdapterInstanceDefinition[i].mAdapterTypeNameID);
        createInterfaceResponseMessage(sockets, "AdapterDeclaration", CIEC_STRING(adapterName, strlen(adapterName)), CIEC_STRING(adapterTypeName, strlen(adapterTypeName)));
      }
    }
    if(!plugs.empty()){
      paReqResult.append("<Plugs>\n         ");
      paReqResult.append(plugs);
      paReqResult.append("</Plugs>\n   ");
    }
    if(!sockets.empty()){
      paReqResult.append("<Sockets>\n         ");
      paReqResult.append(sockets);
      paReqResult.append("</Sockets>\n   ");
    }
  }
}

void CResource::createInterfaceResponseMessages(CIEC_STRING &paReqResult, const char *paCommand,
    const CStringDictionary::TStringId* paNameList, const CStringDictionary::TStringId* paTypeList,
    const TEventID paNumberOfElements, const TDataIOID* paEWith, const TForteInt16* paEWithIndexes, const CStringDictionary::TStringId* paDNameList){
  for(TEventID nIndex = 0; nIndex < paNumberOfElements; nIndex++){
    if(nullptr != paTypeList){
      const char *name = CStringDictionary::getInstance().get(paNameList[nIndex]);
      const char *type = CStringDictionary::getInstance().get(paTypeList[nIndex]);
      createInterfaceResponseMessage(paReqResult, paCommand, CIEC_STRING(name, strlen(name)), CIEC_STRING(type, strlen(type)));
    }
    else{
      const char *name = CStringDictionary::getInstance().get(paNameList[nIndex]);
      constexpr char event[] = "Event";
      createInterfaceResponseMessage(paReqResult, paCommand, CIEC_STRING(name, strlen(name)), CIEC_STRING(event, std::char_traits<char>::length(event)), paEWith, paEWithIndexes, nIndex, paDNameList);
    }
  }
}

void CResource::createInterfaceResponseMessage(CIEC_STRING& paReqResult, const char* paCommand, const CIEC_STRING& paName, const CIEC_STRING& paType,
    const TDataIOID* paEWith, const TForteInt16* paEWithIndexes, const TEventID paIndex, const CStringDictionary::TStringId* paENameList) const {
  paReqResult.append("<");
  paReqResult.append(paCommand);
  paReqResult.append(" Name=\"");
  paReqResult.append(paName);
  paReqResult.append("\" Type=\"");
  paReqResult.append(paType);
  if(nullptr != paEWithIndexes && -1 != paEWithIndexes[paIndex]){
    paReqResult.append("\">\n         ");
    for(int nRunIndex = paEWithIndexes[paIndex]; scmWithListDelimiter != paEWith[nRunIndex]; nRunIndex++){
      paReqResult.append("<With Var=\"");
      paReqResult.append(CStringDictionary::getInstance().get(paENameList[paEWith[nRunIndex]]));
      paReqResult.append("\"/>\n      ");
    }
    paReqResult.append("</Event>\n      ");
  }
  else{
    paReqResult.append("\"/>\n");
  }
}
#endif //FORTE_SUPPORT_QUERY_CMD

#ifdef FORTE_DYNAMIC_TYPE_LOAD
EMGMResponse CResource::createFBTypeFromLua(CStringDictionary::TStringId typeNameId,
    const std::string& paLuaScriptAsString){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;

  if(paLuaScriptAsString.find("internalFBs") !=std::string::npos){ // CFBType
    if(CLuaCFBTypeEntry::createLuaFBTypeEntry(typeNameId, paLuaScriptAsString) != nullptr){
      retVal = EMGMResponse::Ready;
    }
    else{
      retVal = EMGMResponse::InvalidOperation;
    }
  }
  else{ // BFBType
    if(CLuaBFBTypeEntry::createLuaFBTypeEntry(typeNameId, paLuaScriptAsString) != nullptr){
      retVal = EMGMResponse::Ready;
    }else{
      retVal = EMGMResponse::InvalidOperation;
    }
  }
  return retVal;
}

EMGMResponse CResource::createAdapterTypeFromLua(CStringDictionary::TStringId typeNameId,
    const std::string& paLuaScriptAsString){
  EMGMResponse retVal = EMGMResponse::UnsupportedType;
  if(CLuaAdapterTypeEntry::createLuaAdapterTypeEntry(typeNameId, paLuaScriptAsString) != nullptr){
     retVal = EMGMResponse::Ready;
  }else{
   retVal = EMGMResponse::InvalidOperation;
  }
  return retVal;
}

#endif //FORTE_DYNAMIC_TYPE_LOAD

CIEC_ANY *CResource::getVariable(forte::core::TNameIdentifier &paNameList){
  CStringDictionary::TStringId portName = paNameList.back();
  paNameList.popBack();
  forte::core::TNameIdentifier::CIterator runner(paNameList.begin());

  CFunctionBlock *fb = this;
  if(paNameList.size() >= 1){
    //this is not an identifier for the resource interface
    fb = getContainedFB(runner); // the last entry is the input name therefore reduce list here by one
  }

  CIEC_ANY *var = nullptr;
  if((nullptr != fb) && (runner.isLastEntry())){
    var = fb->getVar(&portName, 1);
  }
  return var;
}

CConnection *CResource::getConnection(forte::core::TNameIdentifier &paSrcNameList){
  CConnection *con = nullptr;
  if(1 == paSrcNameList.size()){
    con = getResIf2InConnection(paSrcNameList[0]);
  }
  else if(paSrcNameList.size() > 1){
    CStringDictionary::TStringId portName = paSrcNameList.back();
    paSrcNameList.popBack();
    forte::core::TNameIdentifier::CIterator runner(paSrcNameList.begin());

    CFunctionBlock *srcFB = getContainedFB(runner);
    if((nullptr != srcFB) && (runner.isLastEntry())) {
      //only use the found result if we have really the last result in the list
      con = srcFB->getEOConnection(portName);
      if(nullptr == con) {
        //it is not an event connection try inout data connection next
        con = srcFB->getDIOOutConnection(portName);
        if(nullptr == con) {
          //it is not an inout connection try data connection next
          con = srcFB->getDOConnection(portName);
          if(nullptr == con) {
            //it is not an data connection try data connection next
            //TODO think if it would be better to move this to CFunctionBlock
            CAdapter *adp = srcFB->getAdapter(portName);
            if((nullptr != adp) && (adp->isPlug())){
              //only plugs hold the connection object
              con = adp->getAdapterConnection();
            }
          }
        }
      }
    }
  }
  return con;
}

CConnection *CResource::getResIf2InConnection(CStringDictionary::TStringId paResInput) const{
  CConnection *con = nullptr;
  if(nullptr != mInterfaceSpec){
    TPortId inPortId = getDIID(paResInput);
    if(cgInvalidPortId != inPortId){
      con = mResIf2InConnections + inPortId;
    }
  }
  return con;
}

void CResource::initializeResIf2InConnections(){
  if(nullptr != mInterfaceSpec){
    mResIf2InConnections = new CInterface2InternalDataConnection[mInterfaceSpec->mNumDIs];
    for(TPortId i = 0; i < mInterfaceSpec->mNumDIs; i++){
      (mResIf2InConnections + i)->setSource(this, i);
    }
  }
}
