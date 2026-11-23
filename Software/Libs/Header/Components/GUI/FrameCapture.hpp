/**
 *******************************************************************************
 * @file    FrameCapture.hpp
 * @brief   Read a complete frame from the GUI framebuffer, for the
 *          developer-mode screenshot command.
 *
 *******************************************************************************
 *
 *******************************************************************************
 */

#ifndef FRAME_CAPTURE_HPP
#define FRAME_CAPTURE_HPP

#include <cstdint>

namespace FrameCapture {

/**
 * @brief   Wait for the GUI to finish a frame, then hold it there.
 *
 * @details Asks the GUI for a frame and returns once it has ended one. The
 *          GUI thread then waits, so the framebuffer holds that whole frame
 *          until release() (or 3 s, whichever is first). Call release() as
 *          soon as the frame is copied or sent. Not for the GUI thread.
 *
 * @param   timeoutMs  How long to wait for the frame.
 *
 * @return  true with the frame held, false if no frame ended in time.
 */
bool hold(uint32_t timeoutMs);

/// @brief Let the GUI go on after hold().
void release();

/// @brief The framebuffer: width() x height() pixels, one byte each (ABGR2222).
const uint8_t* pixels();

uint16_t width();
uint16_t height();

} // namespace FrameCapture

#endif // FRAME_CAPTURE_HPP
