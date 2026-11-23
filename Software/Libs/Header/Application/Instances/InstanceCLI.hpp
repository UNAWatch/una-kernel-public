/**
 *******************************************************************************
 * @file    InstanceLogger.hpp
 * @brief   Initialization of the Logger
 * 
 *******************************************************************************
 *
 *******************************************************************************
 */

#pragma once

#include "Porting.h"
#include "CLI/CLIManager.h"
#include "Components/CLIUtils/utilClearScreen.h"
#include "UsbDevice.hpp"
#include "OS.hpp"

////////////////////////////////////////////////////////////////////////////////
//// Public code
////////////////////////////////////////////////////////////////////////////////

class InstanceCLI final {
public:
    /// Blocking entry to run “test mode handshake” flow and then switch to work mode.
    static void WaitTestMode(bool enableMSC);

    /// Start CLI thread in work mode (non-blocking).
    static void On();

    /// Stop CLI thread and cleanup (bounded wait).
    static void Off();

    /// Leave test mode: restart and boot normally, with no test-mode window on
    /// that one boot. Does not return.
    [[noreturn]] static void BootNormally();

private:
    // no instances
    InstanceCLI() = delete;

    static void Init();

    static bool     ConnectedStub();
    static void     SendStub(const uint8_t* buff, uint16_t len);
    static uint16_t InQueueStub();
    static uint16_t RecvStub(uint8_t* pBuff, uint16_t len);

    // Internal helpers
    static void InitCliConsole(bool workMode);
    static void CreateUtilsSet(bool workMode);
    static void Task(void* arg);

    // Internal state
    static UsbDevice*        sUsb;
    static CLIConsole_t      sConsole;
    static CLIManager_t      sMgr;
    static UtilClearScreen_t sCls;
    static osThreadId_t      sThread;
    static osEventFlagsId_t  sEvent;

    enum : uint32_t { CLI_STOP = 1u << 0, CLI_DONE = 1u << 1 };
};

