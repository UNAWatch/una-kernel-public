/**
 * @file CLIInput_test.cpp
 * @brief Line editing in the USB command line stays inside its buffer.
 *
 * The cursor (bufferInsPos) must never pass the end of the line (bufferEndPos),
 * and the line must stay inside the CLI_MANAGER_BUFFER_LEN-byte buffer: the
 * insert loop copies (end - cursor) bytes, so a cursor past the end turns that
 * count into tens of kilobytes.
 */

#include <gtest/gtest.h>

#include "CLI/CLIInput.h"
#include "CLI/CLIConsole.h"
#include "CLI/CLIKeyboard.h"

#include <cstdint>
#include <deque>

namespace {

/// Keys waiting for CLIInput_refresh(), in the codes CLIKeyboard_getKey() returns.
std::deque<uint16_t> gKeys;

} // namespace

// CLIInput reads keys and echoes through these. Only the key source matters here.
extern "C" {
bool CLIKeyboard_kbhit() { return !gKeys.empty(); }

uint16_t CLIKeyboard_getKey()
{
    const uint16_t key = gKeys.front();
    gKeys.pop_front();
    return key;
}

void CLIKeyboard_reset() { gKeys.clear(); }

int CLIConsole_putchar(int ch) { return ch; }

void CLIConsole_print(const char* format, ...) { (void) format; }
}

namespace {

class CLIInputTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        gKeys.clear();
        CLIInput_init(&mIn);
    }

    void TearDown() override { CLIInput_deInit(&mIn); }

    /// One key at a time, checking the positions after each. Once a check has
    /// failed, later keys are dropped: with the cursor past the end, the next
    /// insert would copy outside the buffer.
    void press(uint16_t key)
    {
        if (HasFatalFailure()) {
            return;
        }
        gKeys.push_back(key);
        CLIInput_refresh(&mIn);
        ASSERT_LE(mIn.bufferInsPos, mIn.bufferEndPos) << "cursor past the end of the line";
        ASSERT_LT(mIn.bufferEndPos, CLI_MANAGER_BUFFER_LEN) << "line past the buffer";
    }

    CLIInput_t mIn {};
};

TEST_F(CLIInputTest, InsertingIntoAFullLineStaysInBuffer)
{
    for (int i = 0; i < CLI_MANAGER_BUFFER_LEN - 1; ++i) {
        press('a');
    }
    press(cliKeyLEFT);
    press('b');                         // inserted before the last character
    press('c');
}

} // namespace
