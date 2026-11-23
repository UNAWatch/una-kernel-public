/**
 *******************************************************************************
 * @file    UtilDev.cpp
 * @brief   CLI utility that lets a computer drive the watch over USB in
 *          developer mode.
 *
 * Files go over the USB drive as usual (see the usb command); dev crc
 * checks a copied file against the one on the computer.
 *
 * Every command but 'dev' itself needs developer mode, which is switched on
 * the watch in Settings > Device. Replies are made for a program to read:
 * each command ends with a line "#OK ..." or "#ERR <reason>", and binary data
 * follows a "#DATA <bytes> ..." line. Log text sent by 'dev log on' arrives
 * between commands, framed by the bytes 0x1E and 0x1F.
 *
 *******************************************************************************
 *
 *******************************************************************************
 */

#include "Components/CLIUtils/UtilDev.hpp"
#include "CLI/ICommand.h"
#include "CLI/CLIConsole.h"

#include "Backend.hpp"
#include "HWButtons.hpp"
#include "FrameCapture.hpp"
#include "SettingsLocalManager.hpp"
#include "App/Manager.hpp"
#include "SensorLayer/Core/SensorManager.hpp"
#include "SensorLayer/Core/SensorDriver.hpp"
#include "LogBuffer/LogBuffer.hpp"
#include "Crc/Crc32.hpp"
#include "FileSystem/File.hpp"
#include "OS.hpp"
#include "Power.hpp"
#include "System.hpp"
#include "main.h"
#include "ff.h"

#include <atomic>
#include <memory>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace {

/// Bumped when a command or a reply changes.
constexpr uint32_t kProtocolVersion = 3;

/// Read size for 'dev crc'.
constexpr uint32_t kChunk = 256;

constexpr char kLogStart = 0x1E;
constexpr char kLogEnd   = 0x1F;

std::atomic<bool> sLogOn {false};
uint64_t          sLogCursor = 0;

constexpr size_t  kMaxInjected = 16;
SDK::Sensor::Type sInjected[kMaxInjected] {};

/// Buttons a 'press' left down, one bit per Interface::IButtons::Id.
uint8_t sHeld = 0;

void ok()
{
    CLIConsole_print("#OK\n");
}

void fail(const char *reason)
{
    CLIConsole_print("#ERR %s\n", reason);
}

bool developerMode()
{
    return Settings::LocalManager::GetInstance().getSettings().developerMode;
}

/// A path on the user volume: "2:/..." as given, or "/..." with "2:" added.
bool userPath(const char *in, char *out, size_t outSize)
{
    if (strncmp(in, "2:/", 3) == 0) {
        return snprintf(out, outSize, "%s", in) < static_cast<int>(outSize);
    }
    if (in[0] == '/') {
        return snprintf(out, outSize, "2:%s", in) < static_cast<int>(outSize);
    }
    return false;
}

bool driveIsOpen()
{
    if (Backend::GetInstance().isMsdOpen()) {
        fail("the USB drive is open: run 'usb drive off' first");
        return true;
    }
    return false;
}

// --- buttons ----------------------------------------------------------------

bool parseButton(const char *name, Interface::IButtons::Id &id)
{
    // The SDK's names: L1 top left, L2 bottom left, R1 top right, R2 bottom right.
    if (strcmp(name, "L1") == 0) { id = Interface::IButtons::Id::SW1; return true; }
    if (strcmp(name, "R1") == 0) { id = Interface::IButtons::Id::SW2; return true; }
    if (strcmp(name, "L2") == 0) { id = Interface::IButtons::Id::SW3; return true; }
    if (strcmp(name, "R2") == 0) { id = Interface::IButtons::Id::SW4; return true; }
    return false;
}

void button(int argc, const char **argv)
{
    Interface::IButtons::Id id;
    if (argc < 4 || !parseButton(argv[2], id)) {
        fail("usage: dev button L1|L2|R1|R2 click|press|release|hold <ms>");
        return;
    }

    HWButtons &buttons = HWButtons::GetInstance();
    // Each level must outlast the 50 ms debounce to be seen.
    constexpr uint32_t kSettleMs = 80;

    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(id));

    if (strcmp(argv[3], "press") == 0) {
        sHeld |= bit;
        buttons.inject(id, true);
        WAIT(kSettleMs);
    } else if (strcmp(argv[3], "release") == 0) {
        buttons.inject(id, false);
        sHeld &= static_cast<uint8_t>(~bit);
        WAIT(kSettleMs);
    } else if (strcmp(argv[3], "click") == 0) {
        // Marked held across the wait: if the port closes meanwhile, the
        // thread is stopped here and stopAll() lets the button go.
        sHeld |= bit;
        buttons.inject(id, true);
        WAIT(150);    // A click is shorter than 500 ms
        buttons.inject(id, false);
        sHeld &= static_cast<uint8_t>(~bit);
        WAIT(kSettleMs);
    } else if (strcmp(argv[3], "hold") == 0 && argc == 5) {
        const uint32_t ms = static_cast<uint32_t>(strtoul(argv[4], nullptr, 10));
        if (ms == 0 || ms > 60000) {
            fail("hold time must be 1..60000 ms");
            return;
        }
        sHeld |= bit;
        buttons.inject(id, true);
        WAIT(ms);
        buttons.inject(id, false);
        sHeld &= static_cast<uint8_t>(~bit);
        WAIT(kSettleMs);
    } else {
        fail("usage: dev button L1|L2|R1|R2 click|press|release|hold <ms>");
        return;
    }
    ok();
}

// --- screenshot -------------------------------------------------------------

void screen()
{
    if (!FrameCapture::hold(2000)) {
        fail("no frame from the GUI");
        return;
    }

    const uint16_t w     = FrameCapture::width();
    const uint16_t h     = FrameCapture::height();
    const uint32_t bytes = static_cast<uint32_t>(w) * h;
    const uint8_t *px    = FrameCapture::pixels();
    const uint32_t crc   = CRC32::calculate(px, bytes);

    CLIConsole_print("#DATA %lu %u %u abgr2222 %08lX\n",
                     static_cast<unsigned long>(bytes), w, h,
                     static_cast<unsigned long>(crc));
    for (uint32_t sent = 0; sent < bytes; ) {
        const uint32_t n = (bytes - sent > 0x8000) ? 0x8000 : (bytes - sent);
        CLIConsole_send(px + sent, static_cast<uint16_t>(n));
        sent += n;
    }
    FrameCapture::release();

    CLIConsole_print("\n");
    ok();
}

// --- files ------------------------------------------------------------------

void crcOf(int argc, const char **argv)
{
    char path[FF_MAX_LFN + 1];
    if ((argc != 3 && argc != 4) || !userPath(argv[2], path, sizeof(path))) {
        fail("usage: dev crc <path> [bytes]");
        return;
    }
    if (driveIsOpen()) {
        return;
    }

    // With a byte count the CRC covers only that much of the file. A .uapp
    // ends with the CRC of what comes before it, which makes the CRC of any
    // whole intact .uapp the same constant: leaving those 4 bytes out gives a
    // value that tells one build from another.
    const uint32_t limit = (argc == 4)
                         ? static_cast<uint32_t>(strtoul(argv[3], nullptr, 10)) : UINT32_MAX;

    std::unique_ptr<FS::File> file(new (std::nothrow) FS::File(path));
    if (!file || !file->open(false, false)) {
        fail("cannot open the file");
        return;
    }

    char     buf[kChunk];
    uint32_t total = 0;
    uint32_t crc   = 0;
    for (;;) {
        size_t br = 0;
        if (!file->read(buf, sizeof(buf), br)) {
            file->close();
            fail("read failed");
            return;
        }
        if (br == 0) {
            break;
        }
        if (total < limit) {
            const uint32_t take = (limit - total < br) ? (limit - total) : static_cast<uint32_t>(br);
            crc = CRC32::calculate(reinterpret_cast<const uint8_t*>(buf), take, crc);
        }
        total += br;
    }
    file->close();

    CLIConsole_print("#OK %lu %08lX\n", static_cast<unsigned long>(total),
                     static_cast<unsigned long>(crc));
}

// --- kernel update ----------------------------------------------------------

/// Installs a kernel the way the phone app's BLE command does: the .ota, copied
/// to /Update/ over the USB drive, goes to the bootloader's volume and the watch
/// restarts into the bootloader, which installs it.
void update(int argc, const char **argv)
{
    char path[FF_MAX_LFN + 1];
    if (argc != 3 || !userPath(argv[2], path, sizeof(path))) {
        fail("usage: dev update /Update/<file>.ota");
        return;
    }
    if (driveIsOpen()) {
        return;
    }

    // The checks Backend::stageFwUpdate() makes, made here first so that a
    // refusal arrives as a reason rather than a bare failure.
    const size_t len = strlen(path);
    if (strncmp(path, UPDATE_DIR_PATH, strlen(UPDATE_DIR_PATH)) != 0 ||
            len < strlen(OTA_FILE_EXTENSION) ||
            strcmp(path + len - strlen(OTA_FILE_EXTENSION), OTA_FILE_EXTENSION) != 0) {
        fail("the file must be /Update/<name>.ota");
        return;
    }
    if (!FS::File::Exist(path)) {
        fail("no such file");
        return;
    }
    if (Power::GetInstance().getBatteryLevel() < 10) {
        fail("battery below 10%: charge the watch first");
        return;
    }

    // The BLE command's own staging, with the path relative to the apps volume
    // as that command passes it. #OK only once the update is in place, then
    // the restart that installs it.
    if (!Backend::GetInstance().stageFwUpdate(path + strlen("2:"))) {
        fail("the update could not be put in place: see the log");
        return;
    }
    CLIConsole_print("#OK installing; the watch restarts into the bootloader\n");
    WAIT(100);    // Let the line out before the port drops, as 'reset' does
    Power::GetInstance().reboot();
}

// --- apps -------------------------------------------------------------------

void apps()
{
    static const struct {
        App::Type   type;
        const char *name;
    } kTypes[] = {
        { App::Type::kActivity,  "activity"  },
        { App::Type::kUtility,   "utility"   },
        { App::Type::kGlance,    "glance"    },
        { App::Type::kClockface, "clockface" },
    };

    // Tab-separated, as folder and app names may hold spaces:
    // service pid, type, app id, .uapp path, name. The app id is printed in
    // two halves: this C library's printf has no 64-bit conversion.
    App::Manager &mgr = App::Manager::GetInstance();
    for (const auto &t : kTypes) {
        for (const App::Item &item : mgr.getAppList(t.type)) {
            const App::Application *app = mgr.findByPID(item.pidService);
            const char *path = (app != nullptr && app->getInfo().file)
                             ? app->getInfo().file->getPath() : "";
            CLIConsole_print("%08lX\t%s\t%08lX%08lX\t%s\t%s\n",
                             static_cast<unsigned long>(item.pidService), t.name,
                             static_cast<unsigned long>(item.appId >> 32),
                             static_cast<unsigned long>(item.appId & 0xFFFFFFFFu),
                             path, item.name);
        }
    }
    ok();
}

void launch(int argc, const char **argv, bool start)
{
    if (argc != 3) {
        fail(start ? "usage: dev launch <pid-hex>" : "usage: dev stop <pid-hex>");
        return;
    }
    const uint32_t given = static_cast<uint32_t>(strtoul(argv[2], nullptr, 16));
    App::Application *app = (given != 0) ? App::Manager::GetInstance().findByPID(given) : nullptr;
    if (app == nullptr) {
        fail("no app with that pid");
        return;
    }

    // Either of the app's pids is accepted, but the start goes out with the
    // GUI's, as the launcher sends it: the app screen waits for a frame from
    // the pid it was started with, and frames come from the GUI process.
    const uint32_t pid = app->getGuiPID();

    IGuiBackend *backend = &Backend::GetInstance();
    const bool posted = start ? backend->sendEventToBackend(G2BEvent::UserAppStart{pid, 0})
                              : backend->sendEventToBackend(G2BEvent::UserAppStop{pid});
    if (!posted) {
        fail("failed to post the event");
        return;
    }
    ok();
}

// --- sensors ----------------------------------------------------------------

void forget(SDK::Sensor::Type type)
{
    for (auto &t : sInjected) {
        if (t == type) {
            t = SDK::Sensor::Type::UNKNOWN;
        }
    }
}

/// Records an injected type, so that stopping all finds it. False when the
/// record is full: the injection must then not be made.
bool remember(SDK::Sensor::Type type)
{
    for (auto &t : sInjected) {
        if (t == type) {
            return true;
        }
    }
    for (auto &t : sInjected) {
        if (t == SDK::Sensor::Type::UNKNOWN) {
            t = type;
            return true;
        }
    }
    return false;
}

void stopInjections()
{
    for (auto &t : sInjected) {
        if (t != SDK::Sensor::Type::UNKNOWN) {
            Sensor::Driver *d = Sensor::Manager::getInstance().getDefaultSensor(t);
            if (d != nullptr) {
                d->stopInjection();
            }
            t = SDK::Sensor::Type::UNKNOWN;
        }
    }
}

bool anyInjected()
{
    for (auto t : sInjected) {
        if (t != SDK::Sensor::Type::UNKNOWN) {
            return true;
        }
    }
    return false;
}

/// "12.5" is a float, "3u" unsigned, "-4i" signed.
bool parseField(const char *s, SDK::Sensor::Data::Field &f)
{
    char *end = nullptr;
    const size_t len = strlen(s);
    if (len > 1 && s[len - 1] == 'u') {
        f.u32 = static_cast<uint32_t>(strtoul(s, &end, 0));
        return end == s + len - 1;
    }
    if (len > 1 && s[len - 1] == 'i') {
        f.i32 = static_cast<int32_t>(strtol(s, &end, 0));
        return end == s + len - 1;
    }
    f.f = strtof(s, &end);
    return end == s + len && len > 0;
}

void sensor(int argc, const char **argv)
{
    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        stopInjections();
        ok();
        return;
    }
    if (argc < 4) {
        fail("usage: dev sensor <type-hex> <values...>|off, or dev sensor off");
        return;
    }

    const auto type = static_cast<SDK::Sensor::Type>(strtoul(argv[2], nullptr, 16));
    Sensor::Driver *d = Sensor::Manager::getInstance().getDefaultSensor(type);
    if (d == nullptr) {
        fail("no sensor of that type");
        return;
    }

    if (argc == 4 && strcmp(argv[3], "off") == 0) {
        d->stopInjection();
        forget(type);
        ok();
        return;
    }

    const int count = argc - 3;
    if (count != d->getFieldCount()) {
        CLIConsole_print("#ERR this sensor has %u fields\n", d->getFieldCount());
        return;
    }

    SDK::Sensor::Data::Field values[CLI_PARAMS_COUNT] {};
    for (int i = 0; i < count; ++i) {
        if (!parseField(argv[3 + i], values[i])) {
            CLIConsole_print("#ERR bad value '%s'\n", argv[3 + i]);
            return;
        }
    }

    // Recorded first: an injection nothing records could not be stopped.
    if (!remember(type)) {
        fail("too many sensors injected at once");
        return;
    }
    if (!d->inject(values, static_cast<uint16_t>(count))) {
        forget(type);
        fail("injection failed");
        return;
    }
    CLIConsole_print("#OK %s\n", d->isActive() ? "active" : "idle");
}

// --- log --------------------------------------------------------------------

void log(int argc, const char **argv)
{
    if (argc == 3 && strcmp(argv[2], "on") == 0) {
        sLogCursor = LogBuffer::totalWritten();
        sLogOn     = true;
        ok();
    } else if (argc == 3 && strcmp(argv[2], "off") == 0) {
        sLogOn = false;
        ok();
    } else {
        fail("usage: dev log on|off");
    }
}

} // namespace

Components::CliUtils::UtilDev::UtilDev() :
        UtilBase("dev"),
        mCommands{
            .execute = UtilDev::execute,
            .getName = UtilDev::getName,
            .getDesc = UtilDev::getDesc,
            .getHelp = UtilDev::getHelp,
        }
{
}

Components::CliUtils::UtilDev& Components::CliUtils::UtilDev::getInstance()
{
    static UtilDev instance;
    return instance;
}

ICLICommand_t *Components::CliUtils::UtilDev::getCommands()
{
    return &mCommands;
}

const char* Components::CliUtils::UtilDev::getName(ICLICommand_t* cmd)
{
    (void) cmd;
    return UtilDev::getInstance().getUtilName();
}

const char* Components::CliUtils::UtilDev::getDesc(ICLICommand_t* cmd)
{
    (void) cmd;
    return "drives the watch from a computer (developer mode)";
}

const char* Components::CliUtils::UtilDev::getHelp(ICLICommand_t* cmd)
{
    (void) cmd;
    CLIConsole_print(
        "Drive the watch from a computer. Turn on Developer mode on the watch\n"
        "first, in Settings > Device.\n"
        "\n"
        "  dev                              protocol, developer mode on/off,\n"
        "                                   firmware version\n"
        "  dev button L1|L2|R1|R2 click|press|release|hold <ms>\n"
        "  dev screen                       the screen, 240x240 ABGR2222\n"
        "  dev crc <path> [bytes]           size and CRC-32 of a file (of its\n"
        "                                   first bytes if given), to check a\n"
        "                                   copy made over the USB drive\n"
        "  dev apps                         installed apps, tab-separated:\n"
        "                                   pid, type, app id, file, name\n"
        "  dev launch <pid> | dev stop <pid>\n"
        "  dev sensor <type-hex> <v1> ...   publish these values instead of\n"
        "                                   the sensor's (3u unsigned, 3i signed)\n"
        "  dev sensor <type-hex> off | dev sensor off\n"
        "                                   (injection also ends when the port\n"
        "                                   closes or developer mode goes off)\n"
        "  dev log on|off                   stream the log between commands,\n"
        "                                   framed by 0x1E ... 0x1F\n"
        "  dev update /Update/<file>.ota    install a kernel, as the phone app\n"
        "                                   does: the watch restarts into the\n"
        "                                   bootloader, which installs it\n"
        "\n"
        "Paths are on the apps volume: /Apps/... or 2:/Apps/...\n"
        "Each command ends with '#OK ...' or '#ERR <reason>'.\n"
        "'dev screen' sends '#DATA <bytes> <w> <h> abgr2222 <crc32>' then the\n"
        "pixels.\n");

    return "";
}

void Components::CliUtils::UtilDev::execute(ICLICommand_t* cmd,
                                            CLIInput_t*    input,
                                            int            argc,
                                            const char**   argv)
{
    (void) cmd;
    (void) input;

    const bool on = developerMode();
    if (!on) {
        // Switched off on the watch: nothing started in developer mode
        // outlives it.
        stopAll();
    }

    if (argc == 1) {
        CLIConsole_print("developer mode: %s\n", on ? "on" : "off");
        CLIConsole_print("#OK %lu %s %s\n", static_cast<unsigned long>(kProtocolVersion),
                         on ? "on" : "off", ::Driver::System::GetBuildVersionStr());
        return;
    }

    if (!on) {
        fail("developer mode is off: turn it on in Settings > Device");
        return;
    }

    const char *sub = argv[1];
    if      (strcmp(sub, "button") == 0) { button(argc, argv); }
    else if (strcmp(sub, "screen") == 0) { screen(); }
    else if (strcmp(sub, "crc")    == 0) { crcOf(argc, argv); }
    else if (strcmp(sub, "apps")   == 0) { apps(); }
    else if (strcmp(sub, "launch") == 0) { launch(argc, argv, true); }
    else if (strcmp(sub, "stop")   == 0) { launch(argc, argv, false); }
    else if (strcmp(sub, "sensor") == 0) { sensor(argc, argv); }
    else if (strcmp(sub, "log")    == 0) { log(argc, argv); }
    else if (strcmp(sub, "update") == 0) { update(argc, argv); }
    else { fail("unknown command: see 'help dev'"); }
}

void Components::CliUtils::UtilDev::stopAll()
{
    sLogOn = false;
    stopInjections();

    // A button a 'press' left down comes back up.
    for (uint8_t i = 0; i < static_cast<uint8_t>(Interface::IButtons::Id::MAX_NUM); ++i) {
        if (sHeld & (1u << i)) {
            HWButtons::GetInstance().inject(static_cast<Interface::IButtons::Id>(i), false);
        }
    }
    sHeld = 0;
}

void Components::CliUtils::UtilDev::poll()
{
    // Developer switched off on the watch ends whatever was started in
    // developer mode, without waiting for the next command. Read once a
    // second, and only while something is running.
    static uint32_t sLastCheckMs = 0;
    if (sLogOn || anyInjected() || sHeld != 0) {
        const uint32_t now = OS::GetTimeMs();
        if (now - sLastCheckMs >= 1000) {
            sLastCheckMs = now;
            if (!developerMode()) {
                stopAll();
                return;
            }
        }
    }

    if (!sLogOn) {
        return;
    }

    uint8_t  buf[256];
    uint32_t lost = 0;
    uint32_t n    = LogBuffer::read(buf + 1, sizeof(buf) - 2, &sLogCursor, &lost);
    if (n == 0 && lost == 0) {
        return;
    }

    if (lost != 0) {
        CLIConsole_print("%c[log: %lu bytes lost]\n%c", kLogStart,
                         static_cast<unsigned long>(lost), kLogEnd);
    }
    if (n != 0) {
        buf[0]     = kLogStart;
        buf[n + 1] = kLogEnd;
        CLIConsole_send(buf, static_cast<uint16_t>(n + 2));
    }
}
