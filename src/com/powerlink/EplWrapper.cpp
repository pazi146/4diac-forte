/*******************************************************************************
 * Copyright (c) 2012 - 2014 AIT, ACIN, fortiss
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Filip Andren, Alois Zoitl, Ewald Weinhandl - initial API and implementation and/or initial documentation
 *******************************************************************************/
#include "EplWrapper.h"
#include "ProcessImageMatrix.h"
#include "EplXmlReader.h"
#include <forte_thread.h>

#if (TARGET_SYSTEM == _WIN32_)
#define _WINSOCKAPI_ // prevent windows.h from including winsock.h
#endif  // (TARGET_SYSTEM == _WIN32_)
/* includes */
#include "Epl.h"

#undef EPL_STACK_VERSION
#define EPL_STACK_VERSION(ver,rev,rel)      (((((ver)) & 0xFF)<<24)|((((rev))&0xFF)<<16)|(((rel))&0xFFFF))

#if (TARGET_SYSTEM == _LINUX_)
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <string.h>
#include <termios.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <errno.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#ifndef CONFIG_POWERLINK_USERSTACK
#include <pthread.h>
#else
#include <pcap.h>
#endif

#elif (TARGET_SYSTEM == _WIN32_)
#include <Iphlpapi.h>
#include <pcap.h>
#endif  // (TARGET_SYSTEM == _WIN32_)
#include <EplTgtConio.h>
//#include <conio.h>

/***************************************************************************/
/*                                                                         */
/*                                                                         */
/*          G L O B A L   D E F I N I T I O N S                            */
/*                                                                         */
/*                                                                         */
/***************************************************************************/

//---------------------------------------------------------------------------
// const defines
//---------------------------------------------------------------------------
#if (TARGET_SYSTEM == _LINUX_)

#define SET_CPU_AFFINITY
#define MAIN_THREAD_PRIORITY            20

#elif (TARGET_SYSTEM == _WIN32_)

// TracePoint support for realtime-debugging
#ifdef _DBG_TRACE_POINTS_
void PUBLIC TgtDbgSignalTracePoint (BYTE bTracePointNumber_p);
#define TGT_DBG_SIGNAL_TRACE_POINT(p)   TgtDbgSignalTracePoint(p)
#else
#define TGT_DBG_SIGNAL_TRACE_POINT(p)
#endif

#endif // (TARGET_SYSTEM == _WIN32_)
const DWORD NODEID = 0xF0; //=> MN
const DWORD IP_ADDR = 0xc0a86401; // 192.168.100.1
const DWORD SUBNET_MASK = 0xFFFFFF00; // 255.255.255.0
const char* HOSTNAME = "EPL Stack";

//---------------------------------------------------------------------------
// module global vars
//---------------------------------------------------------------------------

CONST BYTE abMacAddr[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static unsigned int uiNodeId_g = EPL_C_ADR_INVALID;

static tEplApiProcessImageCopyJob AppProcessImageCopyJob_g;

static bool waitingUntilOperational;

#ifdef CONFIG_POWERLINK_USERSTACK

//static char* pszCdcFilename_g = "mnobd.cdc";

#else

static pthread_t eventThreadId;
static pthread_t syncThreadId;

void *powerlinkEventThread(void * arg);
void *powerlinkSyncThread(void * arg);

#endif

//---------------------------------------------------------------------------
// local function prototypes
//---------------------------------------------------------------------------

// This function is the entry point for your object dictionary. It is defined
// in OBJDICT.C by define EPL_OBD_INIT_RAM_NAME. Use this function name to define
// this function prototype here. If you want to use more than one Epl
// instances then the function name of each object dictionary has to differ.

extern "C" tEplKernel PUBLIC EplObdInitRam (tEplObdInitParam MEM* pInitParam_p);

tEplKernel PUBLIC AppCbEvent(
    tEplApiEventType EventType_p,// IN: event type (enum)
    tEplApiEventArg* pEventArg_p,// IN: event argument (union)
    void GENERIC* pUserArg_p);

tEplKernel PUBLIC AppCbSync(void);

char* CEplStackWrapper::allocProcImage(unsigned int n_bytes){
  char* procImage = (char*) malloc(n_bytes);
  for(unsigned int i = 0; i < n_bytes; i++){
    procImage[i] = 0x00;
  }

  return procImage;
}

//=========================================================================//
//                                                                         //
//          S T A T I C   F U N C T I O N S                                //
//                                                                         //
//=========================================================================//

DEFINE_SINGLETON(CEplStackWrapper);

void CEplStackWrapper::eplMainInit(){
#if (TARGET_SYSTEM == _LINUX_)
  sigset_t    mask;

  /*
   * We have to block the real time signals used by the timer modules so
   * that they are able to wait on them using sigwaitinfo!
   */
  sigemptyset(&mask);
  sigaddset(&mask, SIGRTMIN);
  sigaddset(&mask, SIGRTMIN + 1);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
#endif
}

//=========================================================================//
//                                                                         //
//          C L A S S   F U N C T I O N S                                  //
//                                                                         //
//=========================================================================//

CEplStackWrapper::CEplStackWrapper(){
}

CEplStackWrapper::~CEplStackWrapper(){
}

int CEplStackWrapper::eplStackInit(char* paXmlFile, char* paCdcFile, char* paEthDeviceName){
  tEplKernel EplRet;
  static tEplApiInitParam EplApiInitParam;
  const char* sHostname = HOSTNAME;

  // Read and process XML file
  CEplXmlReader xmlReader(&mProcMatrixIn, &mProcMatrixOut);
  xmlReader.readXmlFile(paXmlFile);

  mProcInSize = mProcMatrixIn.getProcessImageSize();
  mProcOutSize = mProcMatrixOut.getProcessImageSize();

  mAppProcessImageIn_g = allocProcImage(mProcInSize);
  mAppProcessImageOut_g = allocProcImage(mProcOutSize);

#ifdef CONFIG_POWERLINK_USERSTACK
#if (TARGET_SYSTEM == _LINUX_)
  struct sched_param schedParam;
#endif

  // variables for Pcap
  char sErr_Msg[PCAP_ERRBUF_SIZE];
  pcap_if_t *alldevs;
  pcap_if_t *seldev;
  int i = 0;
#endif

#ifdef CONFIG_POWERLINK_USERSTACK

#if (TARGET_SYSTEM == _LINUX_)
  /* adjust process priority */
  if(nice(-20) == -1) // push nice level in case we have no RTPreempt
  {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
  EPL_DBGLVL_ERROR_TRACE("%s() couldn't set nice value! (%s)\n", __func__, strerror(errno));
#else
    EPL_DBGLVL_ERROR_TRACE2("%s() couldn't set nice value! (%s)\n", __func__, strerror(errno));
#endif
  
  }
  schedParam.sched_priority = MAIN_THREAD_PRIORITY;
  if(pthread_setschedparam(pthread_self(), SCHED_RR, &schedParam) != 0){
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)  
  EPL_DBGLVL_ERROR_TRACE("%s() couldn't set thread scheduling parameters! %d\n", __func__, schedParam.__sched_priority);
#else
    EPL_DBGLVL_ERROR_TRACE2("%s() couldn't set thread scheduling parameters! %d\n", __func__, schedParam.__sched_priority);
#endif
  }

  /* Initialize target specific stuff */
  // EplTgtInit();
#elif (TARGET_SYSTEM == _WIN32_)

  // activate realtime priority class
  SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
  // lower the priority of this thread
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);

#endif // (TARGET_SYSTEM == _WIN32_)
#endif // CONFIG_POWERLINK_USERSTACK
  /* Enabling ftrace for debugging */
  FTRACE_OPEN();
  FTRACE_ENABLE (TRUE);

  EPL_MEMSET(&EplApiInitParam, 0, sizeof(EplApiInitParam));
  EplApiInitParam.mSizeOfStruct = sizeof(EplApiInitParam);

#ifdef CONFIG_POWERLINK_USERSTACK

  ////////////////////////////////////////////////////////////////////////////////
  // Find ETH card specified by user //
  ////////////////////////////////////////////////////////////////////////////////

  bool macFound;
  char correctDevName[1024];

  macFound = findMAC(paEthDeviceName, &correctDevName[0]);

  /* Retrieve the device list on the local machine */

  if(pcap_findalldevs(&alldevs, sErr_Msg) == -1){
    fprintf(stderr, "Error in pcap_findalldevs: %s\n", sErr_Msg);
    EplRet = kEplNoResource;
    return EplRet;
  }

  printf("\n");
  for(seldev = alldevs, i = 0; seldev != nullptr; seldev = seldev->next, i++){
    if(seldev->description){
      printf("%d: %s\n      %s\n", i, seldev->description, seldev->name);
    }
    else{
      printf("%d: %s\n", i, seldev->name);
    }

    if(macFound){
      const char* userDescLoc = strstr(seldev->name, correctDevName);
      if(userDescLoc != nullptr){
        if(seldev->description){
          printf("\nChosen Ethernet Card: %s\n      %s\n", seldev->description, seldev->name);
        }
        else{
          printf("\nChosen Ethernet Card: %s\n", seldev->name);
        }
        break;
      }
    }

    if(seldev->description){
      const char* userDescLoc = strstr(seldev->description, paEthDeviceName);
      if(userDescLoc != nullptr){
        printf("\nChosen Ethernet Card: %s\n", seldev->description);
        break;
      }
    }
    else{
      const char* userDescLoc = strstr(seldev->name, paEthDeviceName);
      if(userDescLoc != nullptr){
        printf("Chosen Ethernet Card: %s\n", seldev->name);
        break;
      }
    }
  }

  // Check if a device was found, otherwise shutdown stack
  if(!seldev){
    fprintf(stderr, "%s(Err/Warn): Invalid MAC address or device name specified. Shutting down Powerlink stack\n", __func__);
    EplRet = kEplNoResource;
    return EplRet;
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Setup EplApiInitParam (some of them can be removed as we have obd?) //
  ////////////////////////////////////////////////////////////////////////////////

  // pass selected device name to Edrv
  char devName[128];
  strncpy(devName, seldev->name, 127);
  EplApiInitParam.m_HwParam.mDevName = devName;

#endif

  EplApiInitParam.mNodeId = uiNodeId_g = NODEID;
  EplApiInitParam.mIpAddress = (0xFFFFFF00 & IP_ADDR) | EplApiInitParam.mNodeId;

  /* write 00:00:00:00:00:00 to MAC address, so that the driver uses the real hardware address */
  EPL_MEMCPY(EplApiInitParam.mMacAddress, abMacAddr, sizeof(EplApiInitParam.mMacAddress));

  EplApiInitParam.mAsyncOnly = FALSE;

  EplApiInitParam.mFeatureFlags = -1;
  EplApiInitParam.mCycleLen = 10000; // required for error detection
  EplApiInitParam.mIsochrTxMaxPayload = 256; // const
  EplApiInitParam.mIsochrRxMaxPayload = 256; // const
  EplApiInitParam.mPresMaxLatency = 50000; // const; only required for IdentRes
  EplApiInitParam.mPreqActPayloadLimit = 36; // required for initialisation (+28 bytes)
  EplApiInitParam.mPresActPayloadLimit = 36; // required for initialisation of Pres frame (+28 bytes)
  EplApiInitParam.mAsndMaxLatency = 150000; // const; only required for IdentRes
  EplApiInitParam.mMultiplCycleCnt = 0; // required for error detection
  EplApiInitParam.mAsyncMtu = 1500; // required to set up max frame size
  EplApiInitParam.mPrescaler = 2; // required for sync
  EplApiInitParam.mLossOfFrameTolerance = 500000;
  EplApiInitParam.mAsyncSlotTimeout = 3000000;
  EplApiInitParam.mWaitSocPreq = 150000;
  EplApiInitParam.mDeviceType = -1; // NMT_DeviceType_U32
  EplApiInitParam.mVendorId = -1; // NMT_IdentityObject_REC.VendorId_U32
  EplApiInitParam.mProductCode = -1; // NMT_IdentityObject_REC.ProductCode_U32
  EplApiInitParam.mRevisionNumber = -1; // NMT_IdentityObject_REC.RevisionNo_U32
  EplApiInitParam.mSerialNumber = -1; // NMT_IdentityObject_REC.SerialNo_U32

  EplApiInitParam.mSubnetMask = SUBNET_MASK;
  EplApiInitParam.mDefaultGateway = 0;
  EPL_MEMCPY(EplApiInitParam.mHostname, sHostname, sizeof(EplApiInitParam.mHostname));
  EplApiInitParam.mSyncNodeId = EPL_C_ADR_SYNC_ON_SOA;
  EplApiInitParam.mSyncOnPrcNode = FALSE;

  // set callback functions
  EplApiInitParam.mCbEvent = AppCbEvent;

#ifdef CONFIG_POWERLINK_USERSTACK
  EplApiInitParam.mObdInitRam = EplObdInitRam;
  EplApiInitParam.mCbSync = AppCbSync;
#else
  EplApiInitParam.mCbSync = nullptr;
#endif
  ////////////////////////////////////////////////////////////////////////////////

  ////////////////////////////////////////////////////////////////////////////////
  // Initialize Powerlink Stack //
  ////////////////////////////////////////////////////////////////////////////////
  printf("\n\n Powerlink %s running.\n  (build: %s / %s)\n\n", (NODEID == EPL_C_ADR_MN_DEF_NODE_ID ? "Managing Node" : "Controlled Node"), __DATE__, __TIME__);

  // initialize POWERLINK stack
  EplRet = EplApiInitialize(&EplApiInitParam);
  if(EplRet != kEplSuccessful){
    return EplRet;
  }

#ifdef CONFIG_POWERLINK_USERSTACK
  /* At this point, we don't need any more the device list. Free it */
  pcap_freealldevs(alldevs);

  EplRet = EplApiSetCdcFilename(paCdcFile);
  if(EplRet != kEplSuccessful){
    return EplRet;
  }
#else
  // create event thread
  if(pthread_create(&eventThreadId, nullptr, &powerlinkEventThread, nullptr) != 0){
    return EplRet;
  }

  // create sync thread
  if(pthread_create(&syncThreadId, nullptr, &powerlinkSyncThread, nullptr) != 0){
    return EplRet;
  }
#endif

  AppProcessImageCopyJob_g.mNonBlocking = FALSE;
  AppProcessImageCopyJob_g.mPriority = 0;
  AppProcessImageCopyJob_g.m_In.mPart = mAppProcessImageIn_g;
  AppProcessImageCopyJob_g.m_In.mOffset = 0;
  AppProcessImageCopyJob_g.m_In.mSize = mProcInSize;
  AppProcessImageCopyJob_g.m_Out.mPart = mAppProcessImageOut_g;
  AppProcessImageCopyJob_g.m_Out.mOffset = 0;
  AppProcessImageCopyJob_g.m_Out.mSize = mProcOutSize;

  EplRet = EplApiProcessImageAlloc(mProcInSize, mProcOutSize, 2, 2);
  if(EplRet != kEplSuccessful){
    eplStackShutdown();
  }

  EplRet = EplApiProcessImageSetup();
  if(EplRet != kEplSuccessful){
    eplStackShutdown();
  }

  // start processing
  EplRet = EplApiExecNmtCommand(kEplNmtEventSwReset);
  if(EplRet != kEplSuccessful){
    eplStackShutdown();
  }

  waitingUntilOperational = false;
  if(mWait == true){
    while(!waitingUntilOperational){
      // Waiting
      CThread::sleepThread(1);
    }
  }

  return EplRet;
}

////////////////////////////////////////////////////////////////////////////////
// Stop the stack //
////////////////////////////////////////////////////////////////////////////////
int CEplStackWrapper::eplStackShutdown(){
  tEplKernel EplRet;

  // halt the NMT state machine
  // so the processing of POWERLINK frames stops
  EplApiExecNmtCommand(kEplNmtEventSwitchOff);

  // delete process image
  EplApiProcessImageFree();

  // delete instance for all modules
  EplRet = EplApiShutdown();
  printf("EplApiShutdown():  0x%X\n", EplRet);

  mProcMatrixIn.clearAll();
  mProcMatrixOut.clearAll();
  mCallbackList.clearAll();
  free(mAppProcessImageIn_g);
  free(mAppProcessImageOut_g);

  return EplRet;
}

CProcessImageMatrix* CEplStackWrapper::getProcessImageMatrixIn(){
  return &mProcMatrixIn;
}

CProcessImageMatrix* CEplStackWrapper::getProcessImageMatrixOut(){
  return &mProcMatrixOut;
}

char* CEplStackWrapper::getProcImageIn(){
  return mAppProcessImageIn_g;
}

char* CEplStackWrapper::getProcImageOut(){
  return mAppProcessImageOut_g;
}

void CEplStackWrapper::waitUntilOperational(bool paWait){
  mWait = paWait;
}

void CEplStackWrapper::registerCallback(IEplCNCallback* paCallback){
  mSync.lock();
  mCallbackList.pushBack(paCallback);
  mSync.unlock();
}

bool CEplStackWrapper::findMAC(const char* paUserMAC, char* paDeviceName){
  //char* correctDevName;

#if (TARGET_SYSTEM == _LINUX_)

  int nSD; // Socket descriptor
  struct ifreq sIfReq; // Interface request
  struct if_nameindex *pIfList; // Ptr to interface name index
  struct if_nameindex *pListSave; // Ptr to interface name index

  //
  // Initialize this function
  //
  pIfList = nullptr;
  pListSave = nullptr;
#ifndef SIOCGIFADDR
  // The kernel does not support the required ioctls
  return (false);
#endif

  //
  // Create a socket that we can use for all of our ioctls
  //
  nSD = socket(PF_INET, SOCK_STREAM, 0);
  if(nSD < 0){
    // Socket creation failed, this is a fatal error
    printf("File %s: line %d: Socket failed\n", __FILE__, __LINE__);
    return (0);
  }

  //
  // Obtain a list of dynamically allocated structures
  //
  pIfList = pListSave = if_nameindex();

  //
  // Walk thru the array returned and query for each interface's
  // address
  //
  for(; *(char *) pIfList != 0; pIfList++){

    strncpy(sIfReq.ifr_name, pIfList->if_name, IF_NAMESIZE);

    //
    // Get the MAC address for this interface
    //
    if(ioctl(nSD, SIOCGIFHWADDR, &sIfReq) != 0){
      // We failed to get the MAC address for the interface
      printf("File %s: line %d: Ioctl failed\n", __FILE__, __LINE__);
      return false;
    }

    //
    // Determine if we are processing the interface that we
    // are interested in
    //
    char chMAC[6 * 2 + 5 + 2];
    sprintf(chMAC, "%02X-%02X-%02X-%02X-%02X-%02X", (unsigned char) sIfReq.ifr_hwaddr.sa_data[0], (unsigned char) sIfReq.ifr_hwaddr.sa_data[1], (unsigned char) sIfReq.ifr_hwaddr.sa_data[2], (unsigned char) sIfReq.ifr_hwaddr.sa_data[3], (unsigned char) sIfReq.ifr_hwaddr.sa_data[4], (unsigned char) sIfReq.ifr_hwaddr.sa_data[5]);

    if(compareMACs(chMAC, paUserMAC)){
      strncpy(paDeviceName, pIfList->if_name, IF_NAMESIZE);

      //
      // Clean up things and return
      //
      if_freenameindex(pListSave);
      close(nSD);

      return true;
    }
  }

  //
  // Clean up things and return
  //
  if_freenameindex(pListSave);
  close(nSD);

#elif (TARGET_SYSTEM == _WIN32_)

  // Find MAC address
  IP_ADAPTER_INFO AdapterInfo[16];// Allocate information for up to 16 NICs
  DWORD dwBufLen = sizeof(AdapterInfo);// Save memory size of buffer

  DWORD dwStatus = GetAdaptersInfo(AdapterInfo, &dwBufLen);
  assert(dwStatus == ERROR_SUCCESS);// Verify return value is valid, no buffer overflow

  PIP_ADAPTER_INFO pAdapterInfo = AdapterInfo;// Contains pointer to current adapter info

  do{
    char* chMAC = new char[6*2+5+1];
    BYTE *macAddr = pAdapterInfo->Address;
    for (int i = 0; i < 6*2+5; i = i+2)
    {
      if (i>0){
        chMAC[i] = '-';
        i++;
      }
      sprintf(&chMAC[i],"%02x",*macAddr++);
    }

    if (compareMACs(chMAC, paUserMAC)){
      //correctDevName = new char[strlen(pAdapterInfo->AdapterName)+1];
      strcpy(paDeviceName,pAdapterInfo->AdapterName);
      delete chMAC;

      //paDeviceName = correctDevName;
      return true;
    }

    pAdapterInfo = pAdapterInfo->Next; // Progress through linked list
    delete chMAC;
  }
  while(pAdapterInfo); // Terminate if last adapter

#endif

  //paDeviceName = nullptr; //No effect
  return false;
}

bool CEplStackWrapper::compareMACs(const char* paMACa, const char* paMACb){
  if(strcmp(paMACa, paMACb) == 0){
    return true;
  }

  char* macCopyA = new char[strlen(paMACa) + 1];
  strcpy(macCopyA, paMACa);
  char* macCopyB = new char[strlen(paMACb) + 1];
  strcpy(macCopyB, paMACb);

  // Change to upper case
  for(unsigned i = 0; i < strlen(paMACa); i++){
    switch (macCopyA[i]){
      case 'a':
        macCopyA[i] = 'A';
        break;
      case 'b':
        macCopyA[i] = 'B';
        break;
      case 'c':
        macCopyA[i] = 'C';
        break;
      case 'd':
        macCopyA[i] = 'D';
        break;
      case 'e':
        macCopyA[i] = 'E';
        break;
      case 'f':
        macCopyA[i] = 'F';
        break;
    }
  }
  for(unsigned i = 0; i < strlen(paMACb); i++){
    switch (macCopyB[i]){
      case 'a':
        macCopyB[i] = 'A';
        break;
      case 'b':
        macCopyB[i] = 'B';
        break;
      case 'c':
        macCopyB[i] = 'C';
        break;
      case 'd':
        macCopyB[i] = 'D';
        break;
      case 'e':
        macCopyB[i] = 'E';
        break;
      case 'f':
        macCopyB[i] = 'F';
        break;
    }
  }

  if(strcmp(macCopyA, macCopyB) == 0){
    delete[] macCopyA;
    delete[] macCopyB;
    return true;
  }

  delete[] macCopyA;
  delete[] macCopyB;
  return false;
}

//=========================================================================//
//                                                                         //
//          P R I V A T E   F U N C T I O N S                              //
//                                                                         //
//=========================================================================//

//---------------------------------------------------------------------------
//
// Function:    AppCbEvent
//
// Description: event callback function called by EPL API layer within
//              user part (low priority).
//
// Parameters:  EventType_p     = event type
//              pEventArg_p     = pointer to union, which describes
//                                the event in detail
//              pUserArg_p      = user specific argument
//
// Returns:     tEplKernel      = error code,
//                                kEplSuccessful = no error
//                                kEplReject = reject further processing
//                                otherwise = post error event to API layer
//
// State:
//
//---------------------------------------------------------------------------

tEplKernel PUBLIC AppCbEvent(
    tEplApiEventType EventType_p,// IN: event type (enum)
    tEplApiEventArg* pEventArg_p,// IN: event argument (union)
    void GENERIC* pUserArg_p)
{
  tEplKernel EplRet = kEplSuccessful;

  UNUSED_PARAMETER(pUserArg_p);

  // check if NMT_GS_OFF is reached
  switch (EventType_p)
  {
    case kEplApiEventNmtStateChange:
    {
      switch (pEventArg_p->m_NmtStateChange.m_NewNmtState)
      {
        case kEplNmtGsOff:
        { // NMT state machine was shut down,
          // because of user signal (CTRL-C) or critical EPL stack error
          // -> also shut down EplApiProcess() and main()
          EplRet = kEplShutdown;
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(kEplNmtGsOff) originating event = 0x%X\n", __func__, pEventArg_p->m_NmtStateChange.m_NmtEvent);
#else
          PRINTF2("%s(kEplNmtGsOff) originating event = 0x%X\n", __func__, pEventArg_p->m_NmtStateChange.m_NmtEvent);
#endif
          break;
        }

        case kEplNmtGsResetCommunication:
        {
          // continue
        }

        case kEplNmtGsResetConfiguration:
        {
          // continue
        }

        case kEplNmtMsPreOperational1:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(0x%X) originating event = 0x%X\n",
              __func__,
              pEventArg_p->m_NmtStateChange.m_NewNmtState,
              pEventArg_p->m_NmtStateChange.m_NmtEvent);
#else
          PRINTF3("%s(0x%X) originating event = 0x%X\n",
              __func__,
              pEventArg_p->m_NmtStateChange.m_NewNmtState,
              pEventArg_p->m_NmtStateChange.m_NmtEvent);
#endif

          // continue
        }

        case kEplNmtGsInitialising:
        case kEplNmtGsResetApplication:
        case kEplNmtMsNotActive:
        case kEplNmtCsNotActive:
        case kEplNmtCsPreOperational1:
        {
          break;
        }
        case kEplNmtCsOperational:
        case kEplNmtMsOperational:
        {
          break;
        }
        default:
        {
          break;
        }
      }

      break;
    }

    case kEplApiEventCriticalError:
    case kEplApiEventWarning:
    { // error or warning occured within the stack or the application
      // on error the API layer stops the NMT state machine
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
      PRINTF("%s(Err/Warn): Source=%02X EplError=0x%03X",
          __func__,
          pEventArg_p->m_InternalError.m_EventSource,
          pEventArg_p->m_InternalError.m_EplError);
#else
      PRINTF3("%s(Err/Warn): Source=%02X EplError=0x%03X",
          __func__,
          pEventArg_p->m_InternalError.m_EventSource,
          pEventArg_p->m_InternalError.m_EplError);
#endif
      // check additional argument
      switch (pEventArg_p->m_InternalError.m_EventSource)
      {
        case kEplEventSourceEventk:
        case kEplEventSourceEventu:
        { // error occured within event processing
          // either in kernel or in user part
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF(" OrgSource=%02X\n", pEventArg_p->m_InternalError.m_Arg.m_EventSource);
#else
          PRINTF1(" OrgSource=%02X\n", pEventArg_p->m_InternalError.m_Arg.m_EventSource);
#endif
          break;
        }

        case kEplEventSourceDllk:
        { // error occured within the data link layer (e.g. interrupt processing)
          // the DWORD argument contains the DLL state and the NMT event
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF(" val=%lX\n", pEventArg_p->m_InternalError.m_Arg.mArg);
#else
          PRINTF1(" val=%lX\n", pEventArg_p->m_InternalError.m_Arg.mArg);
#endif
          break;
        }

        case kEplEventSourceObdk:
        case kEplEventSourceObdu:
        { // error occured within OBD module
          // either in kernel or in user part
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF(" Object=0x%04X/%u\n", pEventArg_p->m_InternalError.m_Arg.m_ObdError.mIndex, pEventArg_p->m_InternalError.m_Arg.m_ObdError.mSubIndex);
#else
          PRINTF2(" Object=0x%04X/%u\n", pEventArg_p->m_InternalError.m_Arg.m_ObdError.mIndex, pEventArg_p->m_InternalError.m_Arg.m_ObdError.mSubIndex);
#endif
          break;
        }

        default:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("\n");
#else
          PRINTF0("\n");
#endif
          break;
        }
      }
      break;
    }

    case kEplApiEventHistoryEntry:
    { // new history entry
      PRINTF("%s(HistoryEntry): Type=0x%04X Code=0x%04X (0x%02X %02X %02X %02X %02X %02X %02X %02X)\n",
          __func__,
          pEventArg_p->m_ErrHistoryEntry.mEntryType,
          pEventArg_p->m_ErrHistoryEntry.mErrorCode,
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[0],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[1],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[2],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[3],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[4],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[5],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[6],
          (WORD) pEventArg_p->m_ErrHistoryEntry.mAddInfo[7]);
      break;
    }

    case kEplApiEventNode:
    {
      // check additional argument
      switch (pEventArg_p->m_Node.m_NodeEvent)
      {
        case kEplNmtNodeEventCheckConf:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, CheckConf)\n", __func__, pEventArg_p->m_Node.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, CheckConf)\n", __func__, pEventArg_p->m_Node.mNodeId);
#endif
          break;
        }

        case kEplNmtNodeEventUpdateConf:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, UpdateConf)\n", __func__, pEventArg_p->m_Node.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, UpdateConf)\n", __func__, pEventArg_p->m_Node.mNodeId);
#endif
          break;
        }

        case kEplNmtNodeEventNmtState:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, NmtState=0x%X)\n", __func__, pEventArg_p->m_Node.mNodeId, pEventArg_p->m_Node.m_NmtState);
#else
          PRINTF3("%s(Node=0x%X, NmtState=0x%X)\n", __func__, pEventArg_p->m_Node.mNodeId, pEventArg_p->m_Node.m_NmtState);
#endif
          if (pEventArg_p->m_Node.m_NmtState == kEplNmtCsOperational){
            printf("init finished\n");
            waitingUntilOperational = true;
          }
          break;
        }

        case kEplNmtNodeEventError:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, Error=0x%X)\n", __func__, pEventArg_p->m_Node.mNodeId, pEventArg_p->m_Node.mErrorCode);
#else
          PRINTF3("%s(Node=0x%X, Error=0x%X)\n", __func__, pEventArg_p->m_Node.mNodeId, pEventArg_p->m_Node.mErrorCode);
#endif
          break;
        }

        case kEplNmtNodeEventFound:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, Found)\n", __func__, pEventArg_p->m_Node.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, Found)\n", __func__, pEventArg_p->m_Node.mNodeId);
#endif
          break;
        }

        default:
        {
          break;
        }
      }
      break;
    }

#if (((EPL_MODULE_INTEGRATION) & (EPL_MODULE_CFM)) != 0)
    case kEplApiEventCfmProgress:
    {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
      PRINTF("%s(Node=0x%X, CFM-Progress: Object 0x%X/%u, ", __func__, pEventArg_p->m_CfmProgress.mNodeId, pEventArg_p->m_CfmProgress.mObjectIndex, pEventArg_p->m_CfmProgress.mObjectSubIndex);
      PRINTF("%u/%u Bytes", pEventArg_p->m_CfmProgress.mBytesDownloaded, pEventArg_p->m_CfmProgress.mTotalNumberOfBytes);
#else
      PRINTF4("%s(Node=0x%X, CFM-Progress: Object 0x%X/%u, ", __func__, pEventArg_p->m_CfmProgress.mNodeId, pEventArg_p->m_CfmProgress.mObjectIndex, pEventArg_p->m_CfmProgress.mObjectSubIndex);
      PRINTF2("%u/%u Bytes", pEventArg_p->m_CfmProgress.mBytesDownloaded, pEventArg_p->m_CfmProgress.mTotalNumberOfBytes);
#endif
      if ((pEventArg_p->m_CfmProgress.mSdoAbortCode != 0)
          || (pEventArg_p->m_CfmProgress.m_EplError != kEplSuccessful))
      {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
        PRINTF(" -> SDO Abort=0x%lX, Error=0x%X)\n", pEventArg_p->m_CfmProgress.mSdoAbortCode, pEventArg_p->m_CfmProgress.m_EplError);
#else
        PRINTF2(" -> SDO Abort=0x%lX, Error=0x%X)\n", pEventArg_p->m_CfmProgress.mSdoAbortCode, pEventArg_p->m_CfmProgress.m_EplError);
#endif
      }
      else
      {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
        PRINTF(")\n");
#else
        PRINTF0(")\n");
#endif
      }
      break;
    }

    case kEplApiEventCfmResult:
    {
      switch (pEventArg_p->m_CfmResult.m_NodeCommand)
      {
        case kEplNmtNodeCommandConfOk:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, ConfOk)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, ConfOk)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#endif
          break;
        }

        case kEplNmtNodeCommandConfErr:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, ConfErr)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, ConfErr)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#endif
          break;
        }

        case kEplNmtNodeCommandConfReset:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, ConfReset)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, ConfReset)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#endif
          break;
        }

        case kEplNmtNodeCommandConfRestored:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, ConfRestored)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#else
          PRINTF2("%s(Node=0x%X, ConfRestored)\n", __func__, pEventArg_p->m_CfmResult.mNodeId);
#endif
          break;
        }

        default:
        {
#if EPL_DEFINED_STACK_VERSION >= EPL_STACK_VERSION(1, 8, 2)
          PRINTF("%s(Node=0x%X, CfmResult=0x%X)\n", __func__, pEventArg_p->m_CfmResult.mNodeId, pEventArg_p->m_CfmResult.m_NodeCommand);
#else
          PRINTF3("%s(Node=0x%X, CfmResult=0x%X)\n", __func__, pEventArg_p->m_CfmResult.mNodeId, pEventArg_p->m_CfmResult.m_NodeCommand);
#endif
          break;
        }
      }
      break;
    }
#endif

    default:
    break;
  }

  return EplRet;
}

//---------------------------------------------------------------------------
//
// Function:    AppCbSync
//
// Description: sync event callback function called by event module within
//              kernel part (high priority).
//              This function sets the outputs, reads the inputs and runs
//              the control loop.
//
// Parameters:  void
//
// Returns:     tEplKernel      = error code,
//                                kEplSuccessful = no error
//                                otherwise = post error event to API layer
//
// State:
//
//---------------------------------------------------------------------------

tEplKernel PUBLIC AppCbSync(){
  tEplKernel EplRet = kEplSuccessful;

  EplRet = EplApiProcessImageExchange(&AppProcessImageCopyJob_g);

  // Loop through callback list and call each FB in the list
  CEplStackWrapper::getInstance().executeAllCallbacks();

  return EplRet;
}

void CEplStackWrapper::executeAllCallbacks(){
  mSync.lock();
  CSinglyLinkedList<IEplCNCallback*>::Iterator itEnd = mCallbackList.end();
  for(CSinglyLinkedList<IEplCNCallback*>::Iterator it = mCallbackList.begin(); it != itEnd; ++it){
    it->cnSynchCallback();
  }
  mSync.unlock();
}

#ifndef CONFIG_POWERLINK_USERSTACK

void *powerlinkEventThread(void * arg __attribute__((unused))){
  EplApiProcess();

  return nullptr;
}

void *powerlinkSyncThread(void * arg __attribute__((unused))){
  while(1){
    AppCbSync();
  }
  return nullptr;
}

#endif

// EOF
