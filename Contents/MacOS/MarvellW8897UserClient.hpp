//
//  MarvellW8897UserClient.hpp
//  MarvellW8897
//
//  Created by Bryan on 16/11/2025.
//

#ifndef _MARVELL_W8897_USERCLIENT_HPP
#define _MARVELL_W8897_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>

class MarvellW8897Controller;

enum {
    kMarvellMethodGetPciInfo = 0,
    kMarvellMethodGetBarInfoLegacy = 1,
    kMarvellMethodReadReg32FromBar = 2,
    kMarvellMethodWriteReg32ToBar = 3,
    kMarvellMethodGetBarInfoV2 = 4,
    kMarvellMethodGetTransportState = 5,
    kMarvellMethodWriteReg32ToBarWithFlush = 6,
    kMarvellMethodGetDebugAbiInfo = 7,
    kMarvellMethodGetBringupResult = 8,
    kMarvellMethodUploadFirmware = 9,
    kMarvellMethodRegisterMlanCore = 10,
    kMarvellMethodStartMlanFirmware = 11,
    kMarvellMethodGetMlanCoreStatus = 12,
    kMarvellMethodRequestMlanScan = 13,
    kMarvellMethodGetMlanScanStatus = 14,
    kMarvellMethodRequestMlanConnect = 15,
    kMarvellMethodGetMlanConnectStatus = 16,
    kMarvellMethodGetMlanTelemetry = 17,
    kMarvellMethodCount
};

class MarvellW8897UserClient : public IOUserClient
{
    OSDeclareDefaultStructors(MarvellW8897UserClient)

private:
    task_t                  fTask { nullptr };
    MarvellW8897Controller* fOwner { nullptr };

public:
    bool start(IOService* provider) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;

    bool initWithTask(task_t owningTask,
                      void* securityID,
                      UInt32 type,
                      OSDictionary* properties) APPLE_KEXT_OVERRIDE;

    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments* args,
                            IOExternalMethodDispatch* dispatch,
                            OSObject* target,
                            void* reference) APPLE_KEXT_OVERRIDE;

    static IOReturn sGetPciInfo(MarvellW8897UserClient* target,
                                void* ref,
                                IOExternalMethodArguments* args);
    static IOReturn sGetBarInfoLegacy(MarvellW8897UserClient* target,
                                      void* ref,
                                      IOExternalMethodArguments* args);
    static IOReturn sGetBarInfoV2(MarvellW8897UserClient* target,
                                  void* ref,
                                  IOExternalMethodArguments* args);
    static IOReturn sReadReg32FromBar(MarvellW8897UserClient* target,
                                      void* ref,
                                      IOExternalMethodArguments* args);
    static IOReturn sWriteReg32ToBar(MarvellW8897UserClient* target,
                                     void* ref,
                                     IOExternalMethodArguments* args);
    static IOReturn sGetTransportState(MarvellW8897UserClient* target,
                                       void* ref,
                                       IOExternalMethodArguments* args);
    static IOReturn sWriteReg32ToBarWithFlush(MarvellW8897UserClient* target,
                                              void* ref,
                                              IOExternalMethodArguments* args);
    static IOReturn sGetDebugAbiInfo(MarvellW8897UserClient* target,
                                     void* ref,
                                     IOExternalMethodArguments* args);
    static IOReturn sGetBringupResult(MarvellW8897UserClient* target,
                                      void* ref,
                                      IOExternalMethodArguments* args);
    static IOReturn sUploadFirmware(MarvellW8897UserClient* target,
                                    void* ref,
                                    IOExternalMethodArguments* args);
    static IOReturn sRegisterMlanCore(MarvellW8897UserClient* target,
                                      void* ref,
                                      IOExternalMethodArguments* args);
    static IOReturn sStartMlanFirmware(MarvellW8897UserClient* target,
                                       void* ref,
                                       IOExternalMethodArguments* args);
    static IOReturn sGetMlanCoreStatus(MarvellW8897UserClient* target,
                                       void* ref,
                                       IOExternalMethodArguments* args);
    static IOReturn sRequestMlanScan(MarvellW8897UserClient* target,
                                     void* ref,
                                     IOExternalMethodArguments* args);
    static IOReturn sGetMlanScanStatus(MarvellW8897UserClient* target,
                                       void* ref,
                                       IOExternalMethodArguments* args);
    static IOReturn sRequestMlanConnect(MarvellW8897UserClient* target,
                                        void* ref,
                                        IOExternalMethodArguments* args);
    static IOReturn sGetMlanConnectStatus(MarvellW8897UserClient* target,
                                          void* ref,
                                          IOExternalMethodArguments* args);
    static IOReturn sGetMlanTelemetry(MarvellW8897UserClient* target,
                                      void* ref,
                                      IOExternalMethodArguments* args);
};

#endif
