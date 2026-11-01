//
//  MarvellW8897UserClient.cpp
//  MarvellW8897
//
//  Created by Bryan on 16/11/2025.
//

#include "MarvellW8897UserClient.hpp"
#include "MarvellW8897Controller.hpp"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(MarvellW8897UserClient, IOUserClient)

static const IOExternalMethodDispatch gDispatchTable[kMarvellMethodCount] = {
    { // getPciInfo
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetPciInfo,
        0,
        0,
        10,
        0
    },
    { // getBarInfoLegacy
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetBarInfoLegacy,
        1,
        0,
        8,
        0
    },
    { // readReg32FromBar
        (IOExternalMethodAction)&MarvellW8897UserClient::sReadReg32FromBar,
        2,
        0,
        1,
        0
    },
    { // writeReg32ToBar
        (IOExternalMethodAction)&MarvellW8897UserClient::sWriteReg32ToBar,
        3,
        0,
        0,
        0
    },
    { // getBarInfoV2
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetBarInfoV2,
        1,
        0,
        11,
        0
    },
    { // getTransportState
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetTransportState,
        0,
        0,
        8,
        0
    },
    { // writeReg32ToBarWithFlush
        (IOExternalMethodAction)&MarvellW8897UserClient::sWriteReg32ToBarWithFlush,
        3,
        0,
        1,
        0
    },
    { // getDebugAbiInfo
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetDebugAbiInfo,
        0,
        0,
        6,
        0
    },
    { // getBringupResult
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetBringupResult,
        0,
        0,
        MarvellW8897Regs::kBringupResultCount,
        0
    },
    { // uploadFirmware: exact Surface firmware, root only
        (IOExternalMethodAction)&MarvellW8897UserClient::sUploadFirmware,
        0,
        kIOUCVariableStructureSize,
        6,
        0
    },
    { // register NXP MLAN software core after firmware is ready, root only
        (IOExternalMethodAction)&MarvellW8897UserClient::sRegisterMlanCore,
        0,
        0,
        0,
        0
    },
    { // start MLAN firmware initialization and PCIe interrupt path, root only
        (IOExternalMethodAction)&MarvellW8897UserClient::sStartMlanFirmware,
        0,
        0,
        0,
        0
    },
    { // read MLAN core status
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetMlanCoreStatus,
        0,
        0,
        6,
        0
    },
    { // asynchronous MLAN scan request, root only
        (IOExternalMethodAction)&MarvellW8897UserClient::sRequestMlanScan,
        0,
        36,
        0,
        0
    },
    { // scan state, completion status, results state, BSS count
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetMlanScanStatus,
        0,
        0,
        4,
        0
    },
    { // manual open or WPA2-PSK association, root only
        (IOExternalMethodAction)&MarvellW8897UserClient::sRequestMlanConnect,
        0,
        104,
        0,
        0
    },
    { // connection stage/result/associated state
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetMlanConnectStatus,
        0,
        0,
        4,
        0
    },
    { // IRQ, deferred-work, and frame-boundary accounting
        (IOExternalMethodAction)&MarvellW8897UserClient::sGetMlanTelemetry,
        0,
        0,
        13,
        0
    }
};

bool MarvellW8897UserClient::initWithTask(task_t owningTask,
                                          void* securityID,
                                          UInt32 type,
                                          OSDictionary* properties)
{
    if (!super::initWithTask(owningTask, securityID, type, properties))
        return false;
    fTask = owningTask;
    return true;
}

bool MarvellW8897UserClient::start(IOService* provider)
{
    fOwner = OSDynamicCast(MarvellW8897Controller, provider);
    if (!fOwner)
        return false;
    if (!super::start(provider))
        return false;
    return true;
}

IOReturn MarvellW8897UserClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::externalMethod(uint32_t selector,
                                                IOExternalMethodArguments* args,
                                                IOExternalMethodDispatch* dispatch,
                                                OSObject* target,
                                                void* reference)
{
    if (selector >= kMarvellMethodCount)
        return kIOReturnUnsupported;

    dispatch = (IOExternalMethodDispatch*)&gDispatchTable[selector];
    target = this;
    return super::externalMethod(selector, args, dispatch, target, reference);
}

IOReturn MarvellW8897UserClient::sGetPciInfo(MarvellW8897UserClient* target,
                                             void*,
                                             IOExternalMethodArguments* args)
{
    uint64_t info[10] = { 0 };
    if (!target->fOwner->getPciIdentity(info)) {
        return kIOReturnError;
    }

    for (uint32_t i = 0; i < 10; ++i) {
        args->scalarOutput[i] = info[i];
    }
    args->scalarOutputCount = 10;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetBarInfoLegacy(MarvellW8897UserClient* target,
                                                   void*,
                                                   IOExternalMethodArguments* args)
{
    const uint32_t barIndex = static_cast<uint32_t>(args->scalarInput[0]);

    uint64_t info[8] = { 0 };
    if (!target->fOwner->getBarInfoLegacy(barIndex, info)) {
        return kIOReturnBadArgument;
    }

    for (uint32_t i = 0; i < 8; ++i) {
        args->scalarOutput[i] = info[i];
    }
    args->scalarOutputCount = 8;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetBarInfoV2(MarvellW8897UserClient* target,
                                               void*,
                                               IOExternalMethodArguments* args)
{
    const uint32_t barIndex = static_cast<uint32_t>(args->scalarInput[0]);

    uint64_t info[11] = { 0 };
    if (!target->fOwner->getBarInfoV2(barIndex, info)) {
        return kIOReturnBadArgument;
    }

    for (uint32_t i = 0; i < 11; ++i) {
        args->scalarOutput[i] = info[i];
    }
    args->scalarOutputCount = 11;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sReadReg32FromBar(MarvellW8897UserClient* target,
                                                   void*,
                                                   IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    const uint32_t barIndex = static_cast<uint32_t>(args->scalarInput[0]);
    const uint32_t offset = static_cast<uint32_t>(args->scalarInput[1]);

    uint32_t value = 0;
    if (!target->fOwner->readReg32FromBar(barIndex, offset, value)) {
        return kIOReturnBadArgument;
    }

    args->scalarOutput[0] = value;
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sWriteReg32ToBar(MarvellW8897UserClient* target,
                                                  void*,
                                                  IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    const uint32_t barIndex = static_cast<uint32_t>(args->scalarInput[0]);
    const uint32_t offset = static_cast<uint32_t>(args->scalarInput[1]);
    const uint32_t value  = static_cast<uint32_t>(args->scalarInput[2]);

    if (!target->fOwner->writeReg32ToBar(barIndex, offset, value)) {
        return kIOReturnBadArgument;
    }

    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetTransportState(MarvellW8897UserClient* target,
                                                    void*,
                                                    IOExternalMethodArguments* args)
{
    uint64_t state[8] = { 0 };
    if (!target->fOwner->getTransportState(state)) {
        return kIOReturnError;
    }

    for (uint32_t i = 0; i < 8; ++i) {
        args->scalarOutput[i] = state[i];
    }
    args->scalarOutputCount = 8;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sWriteReg32ToBarWithFlush(MarvellW8897UserClient* target,
                                                           void*,
                                                           IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    const uint32_t barIndex = static_cast<uint32_t>(args->scalarInput[0]);
    const uint32_t offset = static_cast<uint32_t>(args->scalarInput[1]);
    const uint32_t value  = static_cast<uint32_t>(args->scalarInput[2]);

    uint32_t readBack = 0;
    if (!target->fOwner->writeReg32ToBarWithFlush(barIndex, offset, value, readBack)) {
        return kIOReturnBadArgument;
    }

    args->scalarOutput[0] = readBack;
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetDebugAbiInfo(MarvellW8897UserClient* target,
                                                  void*,
                                                  IOExternalMethodArguments* args)
{
    uint64_t info[6] = { 0 };
    if (!target->fOwner->getDebugAbiInfo(info)) {
        return kIOReturnError;
    }

    for (uint32_t i = 0; i < 6; ++i) {
        args->scalarOutput[i] = info[i];
    }
    args->scalarOutputCount = 6;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetBringupResult(MarvellW8897UserClient* target,
                                                   void*,
                                                   IOExternalMethodArguments* args)
{
    uint64_t state[MarvellW8897Regs::kBringupResultCount] = { 0 };
    if (!target->fOwner->getBringupResult(state)) {
        return kIOReturnError;
    }

    for (uint32_t i = 0; i < MarvellW8897Regs::kBringupResultCount; ++i) {
        args->scalarOutput[i] = state[i];
    }
    args->scalarOutputCount = MarvellW8897Regs::kBringupResultCount;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sUploadFirmware(MarvellW8897UserClient* target,
                                                void*,
                                                IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        return kIOReturnNotPrivileged;
    }
    if (!args->structureInput || args->structureInputSize != 723540) {
        return kIOReturnBadArgument;
    }

    MarvellW8897FirmwareProtocol::Result result;
    const IOReturn status = target->fOwner->uploadFirmware(
        args->structureInput, args->structureInputSize, result);
    if (status != kIOReturnSuccess) return status;

    args->scalarOutput[0] = static_cast<uint64_t>(result.status);
    args->scalarOutput[1] = result.bytesTransferred;
    args->scalarOutput[2] = result.blockCount;
    args->scalarOutput[3] = result.retryCount;
    args->scalarOutput[4] = result.lastRequest;
    args->scalarOutput[5] = result.lastFirmwareStatus;
    args->scalarOutputCount = 6;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sRegisterMlanCore(MarvellW8897UserClient* target,
                                                   void*, IOExternalMethodArguments*)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    return target->fOwner->registerMlanCore();
}

IOReturn MarvellW8897UserClient::sStartMlanFirmware(MarvellW8897UserClient* target,
                                                    void*, IOExternalMethodArguments*)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    return target->fOwner->startMlanFirmware();
}

IOReturn MarvellW8897UserClient::sGetMlanCoreStatus(MarvellW8897UserClient* target,
                                                    void*, IOExternalMethodArguments* args)
{
    uint64_t status[6] = {};
    if (!target->fOwner->getMlanCoreStatus(status)) return kIOReturnError;
    for (unsigned i = 0; i < 6; ++i) args->scalarOutput[i] = status[i];
    args->scalarOutputCount = 6;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sRequestMlanScan(MarvellW8897UserClient* target,
                                                  void*, IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!args->structureInput || args->structureInputSize != 36)
        return kIOReturnBadArgument;
    const uint8_t* input = static_cast<const uint8_t*>(args->structureInput);
    uint32_t length = 0;
    bcopy(input, &length, sizeof(length));
    if (length > 32) return kIOReturnBadArgument;
    return target->fOwner->requestMlanScan(input + 4, length);
}

IOReturn MarvellW8897UserClient::sGetMlanScanStatus(MarvellW8897UserClient* target,
                                                    void*, IOExternalMethodArguments* args)
{
    uint64_t status[4] = {};
    if (!target->fOwner->getMlanScanStatus(status)) return kIOReturnError;
    for (unsigned i = 0; i < 4; ++i) args->scalarOutput[i] = status[i];
    args->scalarOutputCount = 4;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sRequestMlanConnect(MarvellW8897UserClient* target,
                                                     void*, IOExternalMethodArguments* args)
{
    if (clientHasPrivilege(target->fTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!args->structureInput || args->structureInputSize != 104)
        return kIOReturnBadArgument;
    const uint8_t* input = static_cast<const uint8_t*>(args->structureInput);
    uint32_t ssidLength = 0;
    uint32_t passphraseLength = 0;
    bcopy(input, &ssidLength, sizeof(ssidLength));
    bcopy(input + 4, &passphraseLength, sizeof(passphraseLength));
    if (ssidLength == 0 || ssidLength > 32 ||
        (passphraseLength != 0 && (passphraseLength < 8 || passphraseLength > 63)))
        return kIOReturnBadArgument;
    return target->fOwner->requestMlanConnect(
        input + 8, ssidLength, input + 40, passphraseLength);
}

IOReturn MarvellW8897UserClient::sGetMlanConnectStatus(MarvellW8897UserClient* target,
                                                       void*, IOExternalMethodArguments* args)
{
    uint64_t status[4] = {};
    if (!target->fOwner->getMlanConnectStatus(status)) return kIOReturnError;
    for (unsigned i = 0; i < 4; ++i) args->scalarOutput[i] = status[i];
    args->scalarOutputCount = 4;
    return kIOReturnSuccess;
}

IOReturn MarvellW8897UserClient::sGetMlanTelemetry(MarvellW8897UserClient* target,
                                                    void*, IOExternalMethodArguments* args)
{
    uint64_t telemetry[13] = {};
    if (!target->fOwner->getMlanTelemetry(telemetry)) return kIOReturnError;
    for (unsigned i = 0; i < 13; ++i) args->scalarOutput[i] = telemetry[i];
    args->scalarOutputCount = 13;
    return kIOReturnSuccess;
}
