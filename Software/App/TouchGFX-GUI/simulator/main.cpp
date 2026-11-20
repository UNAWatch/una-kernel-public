#include <platform/hal/simulator/sdl2/HALSDL2.hpp>
#include <touchgfx/hal/NoDMA.hpp>
#include <common/TouchGFXInit.hpp>
#include <gui_generated/common/SimConstants.hpp>
#include <platform/driver/touch/SDL2TouchController.hpp>
#include <platform/driver/touch/NoTouchController.hpp>
#include <touchgfx/lcd/LCD.hpp>
#include <stdlib.h>
#include <simulator/mainBase.hpp>
#include "touchgfx/Utils.hpp"

// The UNA Watch housing around the screen comes with SDK/Simulator/WatchHAL.hpp
// (una-sdk #369). With an older SDK the simulator runs without it.
#if __has_include("SDK/Simulator/WatchHAL.hpp")
#include "SDK/Simulator/WatchHAL.hpp"
#define UNA_SIM_HAL SDK::Simulator::WatchHAL
#else
#define UNA_SIM_HAL touchgfx::HALSDL2
#endif

using namespace touchgfx;

#ifdef __linux__
int main(int argc, char** argv)
{
#else
#include <shellapi.h>
#ifdef _UNICODE
#error Cannot run in unicode mode
#endif
int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    int argc;
    char** argv = touchgfx::HALSDL2::getArgv(&argc);
#endif

    touchgfx::NoDMA dma; //For windows/linux, DMA transfers are simulated
    LCD& lcd = setupLCD();
    touchgfx::NoTouchController tc;

    // WatchHAL = HALSDL2 + the watch housing (F4 cycles it) and the panel look (L).
    touchgfx::HAL& hal = touchgfx::touchgfx_generic_init<UNA_SIM_HAL>(dma, lcd, tc, SIM_WIDTH, SIM_HEIGHT, 0, 0);

    setupSimulator(argc, argv, hal);

#if __has_include("SDK/Simulator/WatchHAL.hpp")
    static_cast<SDK::Simulator::WatchHAL &>(hal).init();

    // Set custom frame rate (UNA_SIM_SLOW stretches it to the watch's pace)
    static_cast<touchgfx::HALSDL2 &>(hal).setVsyncInterval(SDK::Simulator::WatchHAL::frameIntervalMs(App::kFrameRate));
#else
    // Set custom frame rate
    static_cast<touchgfx::HALSDL2 &>(hal).setVsyncInterval(1000.0f / App::kFrameRate);
#endif

    // Ensure there is a console window to print to using printf() or
    // std::cout, and read from using e.g. fgets or std::cin.
    // Alternatively, instead of using printf(), always use
    // touchgfx_printf() which will ensure there is a console to write
    // to.
    touchgfx_enable_stdio();

    touchgfx::HAL::getInstance()->taskEntry(); //Never returns

    return EXIT_SUCCESS;
}
