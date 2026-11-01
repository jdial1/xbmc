/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>

namespace KODI::WINDOWING
{

/*! \brief Wake the GUI main loop from its idle wait. Thread-safe and cheap; call when work is queued for it
 (input actions, application messages, window events). */
void WakeMainLoop();

/*! \brief Wait until WakeMainLoop() (or window input on Windows desktop) or until \p timeout.
 \return true if woken early. */
bool WaitMainLoop(std::chrono::nanoseconds timeout, bool windowInput);

/*!
 \brief Paces main-loop iterations that did not render (no dirty region).

 It replaces a fixed 40 ms sleep, which cut GUI animation that only dirties some frames down to ~16-25 fps
 and delayed every input by up to 40 ms (47 ms where the timer tick is 15.6 ms, e.g. UWP/Xbox). Recently
 active GUIs are paced at the display refresh; after ~0.5 s without any rendered frame it backs off to 40 ms
 so an idle GUI still costs about the same CPU as before. Every wait ends as soon as input or a message
 arrives.
 */
class CIdleFramePacer
{
public:
  void FrameDone(bool rendered, float refreshRate, bool windowInput);

private:
  unsigned int m_idleFrames{0};
};

} // namespace KODI::WINDOWING
