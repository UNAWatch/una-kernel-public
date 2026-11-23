/**
 *******************************************************************************
 * @file    UtilBoot.hpp
 * @brief   Test-mode CLI utility that leaves test mode: the watch restarts and
 *          boots normally, without waiting for a test-mode connection.
 *
 *******************************************************************************
 *
 *******************************************************************************
 */

#ifndef UTIL_BOOT_HPP
#define UTIL_BOOT_HPP

#include <Porting.h>
#include "CLI/UtilBase.hpp"
#include "CLI/ICommand.h"

namespace Components {

namespace CliUtils {

class UtilBoot : public UtilBase
{
public:
    static UtilBoot &getInstance();

    UtilBoot(UtilBoot const&)            = delete;
    void operator=(UtilBoot const&)      = delete;

    UtilBoot(UtilBoot&&)                 = delete;
    UtilBoot& operator=(UtilBoot&&)      = delete;

    ICLICommand_t *getCommands() override;

private:
    ICLICommand_t mCommands;

    explicit UtilBoot();
    ~UtilBoot() override = default;

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

#endif // UTIL_BOOT_HPP
