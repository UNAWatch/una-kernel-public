/**
 *******************************************************************************
 * @file    InstanceLogger.cpp
 * @brief   Initialization of the Command Line Interface
 * 
 *******************************************************************************
 *
 *******************************************************************************
 */

#define LOG_MODULE_PRX   "InstCLI"
#define LOG_MODULE_LEVEL LOG_LEVEL_DEBUG
#include "Logger.h"

#include "Instances/InstanceCLI.hpp"
#include "CLI/CLIConsole.h"

#include "Components/CLI/UtilBase.hpp"
#include "Components/CLIUtils/UtilReset.hpp"
#include "Components/CLIUtils/UtilCrash.hpp"
#include "Components/CLIUtils/UtilButton.hpp"
#include "Components/CLIUtils/UtilTime.hpp"
#include "Components/CLIUtils/UtilTest.hpp"
#include "Components/CLIUtils/UtilMagCal.hpp"
#include "Components/CLIUtils/UtilModuleInfo.hpp"
#include "Components/CLIUtils/UtilFormat.hpp"
#include "Components/CLIUtils/UtilUsbMsd.hpp"
#include "Components/CLIUtils/UtilDev.hpp"
#include "Components/CLIUtils/UtilUsb.hpp"
#include "Components/CLIUtils/FS/utilFS.hpp"

#include "SwTimer.hpp"

#include <string.h>

#include "Pmic.hpp"

// Static storage
UsbDevice*        InstanceCLI::sUsb   = nullptr;
CLIConsole_t      InstanceCLI::sConsole{};
CLIManager_t      InstanceCLI::sMgr{};
UtilClearScreen_t InstanceCLI::sCls{};
osThreadId_t      InstanceCLI::sThread = nullptr;
osEventFlagsId_t  InstanceCLI::sEvent  = nullptr;

static Driver::SwTimer mTimer;
static bool            mUsbConnected;

void InstanceCLI::Init()
{
    if (!sUsb) {
        sUsb = &UsbDevice::GetInstance();
    }
}

void InstanceCLI::WaitTestMode(bool enableMSC)
{
    Init();

    sUsb->init(false, 0);
    sUsb->enable();

    InitCliConsole(false);
    CreateUtilsSet(false);

    LOG_WARNING("Waiting for host connection...\n");
    const uint32_t deadline = OS::GetTimeMs() + 10000;
    while (OS::GetTimeMs() < deadline) {
        OS::Delay(10);
        if (sUsb->isSerialPortOpen() || !Pmic::GetInstance().isUsbDetected()) break;
    }

    if (sUsb->isSerialPortOpen()) {
        uint8_t  d[1];
        uint32_t br = 0;
        LOG_WARNING("Waiting for any command...\n");
        if (sUsb->receive(d, 1, br, 10000)) {
            LOG_WARNING("device in test mode\n");
            CLIManager_start(&sMgr);
            while (true) {
                CLIManager_refresh(&sMgr);
                OS::Delay(10);
                if (!sUsb->isSerialPortOpen()) {
                    LOG_INFO("system reset\n");
                    WAIT(100);
                    NVIC_SystemReset();
                }
            }
        }
    }

    LOG_WARNING("host did not connect\n");
    sUsb->disable();
    sUsb->deinit();

    InitCliConsole(true);
    CreateUtilsSet(true);
}

void InstanceCLI::On()
{
    if (sThread) {
        return;
    }

    Init();

    InitCliConsole(true);
    CreateUtilsSet(true);

    if (!sEvent) {
        sEvent = osEventFlagsNew(nullptr);
    }

    osThreadAttr_t attr{};
    attr.name = "CLI"; attr.stack_size = 10 * 1024; attr.priority = osPriorityNormal;
    sThread = osThreadNew(&InstanceCLI::Task, nullptr, &attr);
}

void InstanceCLI::Off()
{
    if (!sThread) {
        return;
    }

    osThreadFlagsSet(sThread, CLI_STOP);
    osEventFlagsWait(sEvent, CLI_DONE, osFlagsWaitAny, 3000);

    if (osThreadGetState(sThread) != osThreadTerminated) {
        osThreadTerminate(sThread);
    }

    sThread = nullptr;

    // The computer that injected sensor values or asked for the log has
    // gone: the sensors go back to their own readings.
    Components::CliUtils::UtilDev::stopAll();
    // osEventFlagsDelete(sEvent); sEvent = nullptr; // keep if you plan to reuse
}

void InstanceCLI::InitCliConsole(bool workMode)
{
    mTimer.start(1000);
    mUsbConnected = Pmic::GetInstance().isUsbDetected();

    CLIConsoleConfig_t cfg = {
        .connected  = ConnectedStub,
        .send       = SendStub,
        .getInQueue = InQueueStub,
        .recv       = RecvStub
    };
    CLIConsole_init(&sConsole, &cfg);

    const CLIManagerColorMap_t map = workMode
        ? CLIManagerColorMap_t{
              .attrDef        = ASCII_TEXT_WHITE   ASCII_BG_BLACK,
              .attrPrompt     = ASCII_TEXT_CYAN    ASCII_BG_BLACK,
              .attrCmdUnknown = ASCII_TEXT_MAGENTA ASCII_BG_BLACK,
              .attrCmdHelp    = ASCII_TEXT_YELLOW  ASCII_BG_BLACK }
        : CLIManagerColorMap_t{
              .attrDef        = "",
              .attrPrompt     = "",
              .attrCmdUnknown = "",
              .attrCmdHelp    = "" };

    CLIManager_init(&sMgr, &sConsole, &map, workMode);
    CLIManager_setPrompt(&sMgr, workMode ? "una" : "una-test");

    if (workMode) {
        colorReset();
        setDisplayAttr(ATTR_RESET);
        setDisplayAttr(ATTR_BRIGHT);
        setDisplayAttr(B_BLACK);
        setDisplayAttr(F_WHITE);
    }
}

void InstanceCLI::CreateUtilsSet(bool workMode)
{
    CLIManager_clearCommands(&sMgr);

    utilClearScreen_init(&sCls);
    CLIManager_addCommand(&sMgr, utilClearScreen_getIntf(&sCls));

    // common
    CLIManager_addCommand(&sMgr, Components::CliUtils::UtilReset::getInstance().getCommands());
    CLIManager_addCommand(&sMgr, Components::CliUtils::UtilButton::getInstance().getCommands());
    CLIManager_addCommand(&sMgr, Components::CliUtils::UtilTime::getInstance().getCommands());
    CLIManager_addHiddenCommand(&sMgr, Components::CliUtils::UtilCrash::getInstance().getCommands());

    if (workMode) {
        // work-only set
        // The USB drive: open/close it with the cable in, and whether it
        // opens by itself. Work mode only, as it posts to the Backend.
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilUsb::getInstance().getCommands());

        // Developer mode: a computer drives the watch (buttons, screenshots,
        // injected sensor data, the log). Refused unless the owner turned
        // developer mode on, on the watch.
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilDev::getInstance().getCommands());

        // Hidden bench tool: fit the magnetometer's hard- and soft-iron
        // correction and keep it. Work mode only, and not by preference - test
        // mode enters its command loop before scanModules() runs, so no
        // magnetometer is ever built there and this would only reach the stub.
        CLIManager_addHiddenCommand(&sMgr, Components::CliUtils::UtilMagCal::getInstance().getCommands());
    } else {
        // test-only set
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilTest::getInstance().getCommands());
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilFormat::getInstance().getCommands());
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilUsbMsd::getInstance().getCommands());
        CLIManager_addHiddenCommand(&sMgr, Components::CliUtils::UtilModuleInfo::getInstance().getCommands());
        CLIManager_addCommand(&sMgr, Components::CliUtils::UtilFS::getInstance().getCommands());
    }
}

void InstanceCLI::Task(void* /*arg*/)
{
    CLIManager_start(&sMgr);

    for (;;) {
        const uint32_t f = osThreadFlagsWait(CLI_STOP, osFlagsWaitAny, 10);

        if ((f & CLI_STOP) == CLI_STOP) {
            break;
        }

        CLIManager_refresh(&sMgr);
        Components::CliUtils::UtilDev::poll();
    }

    if (sEvent) {
        osEventFlagsSet(sEvent, CLI_DONE);
    }

    osThreadExit();
}

// Stubs bound to UsbDevice
bool InstanceCLI::ConnectedStub()
{
    if (mTimer.check()) {
        mUsbConnected = Pmic::GetInstance().isUsbDetected();
    }

    return mUsbConnected;
}

void InstanceCLI::SendStub(const uint8_t* buff, uint16_t len)
{
    while (len) {
        const uint16_t chunk = (len > USB_CDC_ACM_TX_FIFO) ? USB_CDC_ACM_TX_FIFO : len;

        InstanceCLI::sUsb->transmit(buff, chunk);

        buff += chunk;
        len  -= chunk;
    }
}

uint16_t InstanceCLI::InQueueStub()
{
    return InstanceCLI::sUsb->getRxBytesAvailable();
}

uint16_t InstanceCLI::RecvStub(uint8_t* pBuff, uint16_t len)
{
    uint32_t br = 0;
    InstanceCLI::sUsb->receive(pBuff, len, br);

//    LOG_INFO_WP("\n <");
//    LOG_INFO_DUMP_WP(pBuff, len);

    return static_cast<uint16_t>(br);
}
