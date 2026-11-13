cmake_minimum_required(VERSION 3.21)
project(una-host-tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for host-test static analysis" FORCE)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# These host tests cover kernel-side code only. Tests that target SDK code live
# in the SDK repo (SDK/Tests/Host), so this harness compiles no SDK sources.
# SDK *headers* are on the include path because kernel classes implement SDK
# interfaces (e.g. MemAllocator : IAppMemory); the submodule must be populated
# (git submodule update --init), as it already is in CI.
set(UNA_KERNEL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/FetchGoogleTest.cmake)

set(KERNEL_LIB_SOURCES
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Sensors/Cadence/CadenceEstimator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/DailyHealth/DailyLedger.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/DailyHealth/HealthCalculator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Sensors/Grade/GradeEstimator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Hardware/MS5837.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Barometer/BaroMs5837.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Sensors/Barometer/BaroP0Adjuster.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Hardware/BMM350/BMM350.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagBosch.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagCalibration.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagCalibrationSession.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagCalibrationStoreRtc.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagCorrection.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagCalibrator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagEllipsoidFit.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/Magnetometer/MagSphereFit.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/Activity/Features/MotionTracker.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/Activity/Features/ActivityTracker.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/ChargeThermalMonitor.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/ErrorArbiter.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/EventRouter.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/ProfileHrs.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/RevolutionRateEstimator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/AccessoryAdvParser.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/AccessoryScanner.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/ProfileCps.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Accessory/ProfileCsc.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Sensors/Body/HrSourceArbiter.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Core/SampleRateAdapter.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/SensorLayer/Core/SensorDataQueue.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Discovery/PeripheralLinkClients.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Services/AMS/ParserAMS.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Services/ANCS/ParserANCS.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Services/CAMS/ParserCAMS.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/External/BLE/Services/CANS/ParserCANS.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/ImageRegion.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/MemAllocator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/MessagePool.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/MessageManager.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/DualAppComm.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/SensorListener.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/Validator.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/App/AliasRules.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/Crc/Crc32.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Hardware/AG3335/AirohaEpoParser.cpp"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/CLI/CLIInput.c"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/CLI/CLIHistory.c"
    "${UNA_KERNEL_ROOT}/Software/Libs/Source/Components/LinkedList/LinkedList.c"
)

# The CLI's build options live in the firmware's Core/Inc, next to its Porting.h,
# which pulls in the STM32 HAL. Copy just the two headers the CLI needs, so their
# "Porting.h" resolves to the host stub in include/.
configure_file("${UNA_KERNEL_ROOT}/Software/App/UnaWatch-CubeIDE/Core/Inc/CLIOptions.h"
               "${CMAKE_CURRENT_BINARY_DIR}/firmware-config/CLIOptions.h" COPYONLY)
configure_file("${UNA_KERNEL_ROOT}/Software/App/UnaWatch-CubeIDE/Core/Inc/ASCIICodes.h"
               "${CMAKE_CURRENT_BINARY_DIR}/firmware-config/ASCIICodes.h" COPYONLY)

# The Bosch BMM350 SensorAPI is vendor C source that BMM350.cpp sits on top of.
# Not held to the warning set above, which is for code this repository owns. It
# carries one local deviation, marked as such in read_otp_word() and to be
# re-applied whenever the file is updated from upstream.
set(BMM350_SENSOR_API
    "${UNA_KERNEL_ROOT}/Software/ThirdParty/BOSCH/BMM350_SensorAPI/bmm350.c"
)

if(MSVC)
    set_source_files_properties(${BMM350_SENSOR_API} PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${BMM350_SENSOR_API} PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(una_host_test_support STATIC
    ${KERNEL_LIB_SOURCES}
    ${BMM350_SENSOR_API}
)

target_include_directories(una_host_test_support PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
    # ParserCANS.cpp includes the Logger with a bare "Logger.h".
    "${CMAKE_CURRENT_LIST_DIR}/include/Logger"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/Shared"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/Magnetometer"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Accessory"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Discovery"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Services/AMS"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Services/ANCS"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Services/CAMS"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Components/External/BLE/Services/CANS"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Interfaces/Components"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Interfaces/Drivers"
    "${UNA_KERNEL_ROOT}/Software/Libs/Header/Hardware"
    "${UNA_KERNEL_ROOT}/Software/ThirdParty/BOSCH/BMM350_SensorAPI"
    "${UNA_KERNEL_ROOT}/SDK/Libs/Header"
    "${CMAKE_CURRENT_BINARY_DIR}/firmware-config"
)

add_executable(una-host-tests
    kernel/AccessoryAdvParser_test.cpp
    kernel/AccessoryScanner_test.cpp
    kernel/ActivityTracker_test.cpp
    kernel/AppSurfaceInfo_test.cpp
    kernel/AppType_test.cpp
    kernel/BaroCadence_test.cpp
    kernel/BaroMs5837_test.cpp
    kernel/BaroP0Adjuster_test.cpp
    kernel/BaroRecoveryPolicy_test.cpp
    kernel/Bmi270FifoFrames_test.cpp
    kernel/Bmi270FifoGrid_test.cpp
    kernel/BMM350_test.cpp
    kernel/CadenceEstimator_test.cpp
    kernel/ChargeThermalMonitor_test.cpp
    kernel/CLIInput_test.cpp
    kernel/ConnParamNegotiation_test.cpp
    kernel/DailyLedger_test.cpp
    kernel/DualAppCommOwnership_test.cpp
    kernel/DualAppCommPushTimeout_test.cpp
    kernel/EpoParser_test.cpp
    kernel/ErrorArbiter_test.cpp
    kernel/EventRouter_test.cpp
    kernel/FaceTilt_test.cpp
    kernel/FramePacer_test.cpp
    kernel/GravityTracker_test.cpp
    kernel/PeripheralLinkClients_test.cpp
    kernel/GradeEstimator_test.cpp
    kernel/ParserAMS_test.cpp
    kernel/ParserANCS_test.cpp
    kernel/ParserCAMS_test.cpp
    kernel/ParserCANS_test.cpp
    kernel/PhoneNotification_test.cpp
    kernel/GuardPath_test.cpp
    kernel/HrSourceArbiter_test.cpp
    kernel/ImuFifoLatency_test.cpp
    kernel/ImuTimeBase_test.cpp
    kernel/LatencyAggregate_test.cpp
    kernel/LogThrottle_test.cpp
    kernel/MagBosch_test.cpp
    kernel/MagCalibrationSession_test.cpp
    kernel/MagCalibrationStoreRtc_test.cpp
    kernel/MagCorrection_test.cpp
    kernel/MagCalibration_test.cpp
    kernel/MagCalibrator_test.cpp
    kernel/MagEllipsoidFit_test.cpp
    kernel/MagLinearSolve_test.cpp
    kernel/MagSphereFit_test.cpp
    kernel/MagTimeBase_test.cpp
    kernel/MagRateHandover_test.cpp
    kernel/MagRecoveryPolicy_test.cpp
    kernel/ImageRegion_test.cpp
    kernel/MemAllocator_test.cpp
    kernel/MessageManagerSweep_test.cpp
    kernel/MessagePool_test.cpp
    kernel/MotionTracker_test.cpp
    kernel/MS5837_test.cpp
    kernel/NowPlayingTracker_test.cpp
    kernel/ProfileCps_test.cpp
    kernel/ProfileCsc_test.cpp
    kernel/ProfileHrs_test.cpp
    kernel/RevolutionRateEstimator_test.cpp
    kernel/SampleRateAdapter_test.cpp
    kernel/SensorDataQueue_test.cpp
    kernel/SensorListener_test.cpp
    kernel/SessionFiles_test.cpp
    kernel/SleepSchedule_test.cpp
    kernel/SourceStepTrack_test.cpp
    kernel/SourceTick_test.cpp
    kernel/VariantAlias_test.cpp
    kernel/WordMove_test.cpp
)

# AppSurfaceInfo_test.cpp runs a writer thread against a reader. GoogleTest
# links Threads::Threads itself, but this does not rely on that.
find_package(Threads REQUIRED)

target_link_libraries(una-host-tests PRIVATE
    una_host_test_support
    GTest::gtest_main
    GTest::gmock
    Threads::Threads
)

# On-device trace replayed by CadenceEstimator_test.cpp.
target_compile_definitions(una-host-tests PRIVATE
    CADENCE_TRACE_PATH="${CMAKE_CURRENT_LIST_DIR}/kernel/data/cadence_debug.csv"
)

enable_testing()
add_test(NAME una-host-tests COMMAND una-host-tests)
