/**
 *******************************************************************************
 * @file    UtilBoot.cpp
 * @brief   Test-mode CLI utility that leaves test mode: the watch restarts and
 *          boots normally, without waiting for a test-mode connection.
 *
 *******************************************************************************
 *
 *******************************************************************************
 */

#include "Components/CLIUtils/UtilBoot.hpp"
#include "CLI/ICommand.h"
#include "CLI/CLIConsole.h"

#include "Instances/InstanceCLI.hpp"

Components::CliUtils::UtilBoot::UtilBoot() :
        UtilBase("boot"),
        mCommands{
            .execute = UtilBoot::execute,
            .getName = UtilBoot::getName,
            .getDesc = UtilBoot::getDesc,
            .getHelp = UtilBoot::getHelp,
        }
{
}

Components::CliUtils::UtilBoot& Components::CliUtils::UtilBoot::getInstance()
{
    static UtilBoot instance;
    return instance;
}

ICLICommand_t *Components::CliUtils::UtilBoot::getCommands()
{
    return &mCommands;
}

const char* Components::CliUtils::UtilBoot::getName(ICLICommand_t* cmd)
{
    (void) cmd;
    return UtilBoot::getInstance().getUtilName();
}

const char* Components::CliUtils::UtilBoot::getDesc(ICLICommand_t* cmd)
{
    (void) cmd;
    return "leaves test mode and boots normally";
}

const char* Components::CliUtils::UtilBoot::getHelp(ICLICommand_t* cmd)
{
    (void) cmd;
    CLIConsole_print(
        "Leave test mode: the watch restarts and boots normally, without the\n"
        "test-mode window on that boot, so the serial port comes back in work\n"
        "mode and any input is safe. Closing the port also leaves test mode,\n"
        "but the restart that follows opens the window again.\n");
    return "";
}

void Components::CliUtils::UtilBoot::execute(ICLICommand_t* cmd,
                                             CLIInput_t*    input,
                                             int            argc,
                                             const char**   argv)
{
    (void) cmd;
    (void) input;
    (void) argc;
    (void) argv;

    InstanceCLI::BootNormally();
}
