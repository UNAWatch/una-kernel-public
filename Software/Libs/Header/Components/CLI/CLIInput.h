#ifndef __CLIInput_h__
#define __CLIInput_h__

#ifdef __cplusplus
 extern "C" {
#endif

#include "CLIOptions.h"
#include "CLI/CLIHistory.h"

////////////////////////////////////////////////////////////////////////////////
/// Data types
////////////////////////////////////////////////////////////////////////////////

typedef struct CLIInputStruct_t CLIInput_t;

typedef void (*CLIInput_handleSymbol_t)(CLIInput_t* in, char symbol);

struct CLIInputStruct_t {
    char    buffer[CLI_MANAGER_BUFFER_LEN];
    uint16_t bufferInsPos;
    uint16_t bufferEndPos;

    int   argC;
    char* argV[CLI_PARAMS_COUNT];

    CLIHistory_t history;

    bool replaceMode;
    CLIInput_handleSymbol_t handleSymbol;   // It depends on replaceMode

    bool echoEnable;
};

////////////////////////////////////////////////////////////////////////////////
/// Public code
////////////////////////////////////////////////////////////////////////////////
void         CLIInput_init(CLIInput_t* in);
void         CLIInput_deInit(CLIInput_t* in);
void         CLIInput_echoEnable(CLIInput_t* in, bool value);
bool         CLIInput_echoEnabled(CLIInput_t* in);
void         CLIInput_reset(CLIInput_t* in);
bool         CLIInput_refresh(CLIInput_t* in);
void         CLIInput_pushCommand(CLIInput_t* in, const char* cmd);
int          CLIInput_getArgC(CLIInput_t* in);
const char** CLIInput_getArgV(CLIInput_t* in);

#ifdef __cplusplus
 }
#endif

#endif
