/**
 *******************************************************************************
 * @file    UtilDev.hpp
 * @brief   CLI utility that lets a computer drive the watch over USB in
 *          developer mode: button presses, screenshots, checking a copied
 *          file, app launch, injected sensor data and a live log stream.
 *
 *******************************************************************************
 *
 *******************************************************************************
 */

#ifndef UTIL_DEV_HPP
#define UTIL_DEV_HPP

#include <Porting.h>
#include "CLI/UtilBase.hpp"
#include "CLI/ICommand.h"

namespace Components {

namespace CliUtils {

class UtilDev : public UtilBase
{
public:
    static UtilDev &getInstance();

    UtilDev(UtilDev const&)            = delete;
    void operator=(UtilDev const&)     = delete;

    UtilDev(UtilDev&&)                 = delete;
    UtilDev& operator=(UtilDev&&)      = delete;

    ICLICommand_t *getCommands() override;

    /**
     * @brief Send new log text to the computer while 'dev log on' is set, and
     *        stop everything started in developer mode once it is switched
     *        off. Called by the CLI thread between commands.
     */
    static void poll();

    /**
     * @brief Stop the log stream and every sensor injection, and release a
     *        button left pressed. Called when
     *        developer mode is off, and when the serial port closes, so
     *        nothing outlives the computer that started it. Not while the
     *        CLI thread runs a command.
     */
    static void stopAll();

private:
    ICLICommand_t mCommands;

    explicit UtilDev();
    ~UtilDev() override = default;

    static const char *getName(ICLICommand_t *pCmd);
    static const char *getDesc(ICLICommand_t *pCmd);
    static const char *getHelp(ICLICommand_t *pCmd);
    static void        execute(ICLICommand_t *pCmd,
                               CLIInput_t    *pInput,
                               int            argc,
                               const char   **pArgv);
};

}
}

#endif // UTIL_DEV_HPP
