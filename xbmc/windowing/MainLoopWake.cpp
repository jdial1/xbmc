/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MainLoopWake.h"

#include "threads/WakeSignal.h"

#include <algorithm>
#include <atomic>

namespace KODI::WINDOWING
{
namespace
{
XbmcThreads::CWakeSignal& MainLoopSignal()
{
  static XbmcThreads::CWakeSignal signal;
  return signal;
}

constexpr unsigned int ACTIVE_IDLE_FRAMES = 30; // ~0.5 s at 60 Hz
constexpr std::chrono::nanoseconds IDLE_WAIT = std::chrono::milliseconds(40);
} // namespace

// set by the first wake after the main loop's last wait: further wakes skip the kernel call (callers such
// as add-on GUI-lock calls can come in at a very high rate)
std::atomic_bool s_wakePending{false};

void WakeMainLoop()
{
  if (!s_wakePending.exchange(true, std::memory_order_acq_rel))
    MainLoopSignal().Signal();
}

bool WaitMainLoop(std::chrono::nanoseconds timeout, bool windowInput)
{
  const bool woken = MainLoopSignal().Wait(timeout, windowInput);
  // cleared before the loop processes queued work, so a wake for work queued after this point signals again
  s_wakePending.store(false, std::memory_order_release);
  return woken;
}

std::chrono::nanoseconds CIdleFramePacer::NextIdleWait(bool rendered, float refreshRate)
{
  if (rendered)
  {
    m_idleFrames = 0;
    return std::chrono::nanoseconds::zero();
  }

  std::chrono::nanoseconds timeout = IDLE_WAIT;
  if (m_idleFrames < ACTIVE_IDLE_FRAMES && refreshRate > 1.0f)
    timeout = std::min(IDLE_WAIT, std::chrono::nanoseconds(static_cast<long long>(1e9 / refreshRate)));
  if (m_idleFrames < ACTIVE_IDLE_FRAMES)
    ++m_idleFrames;
  return timeout;
}

void CIdleFramePacer::FrameDone(bool rendered, float refreshRate, bool windowInput)
{
  const std::chrono::nanoseconds timeout = NextIdleWait(rendered, refreshRate);
  if (timeout > std::chrono::nanoseconds::zero())
    WaitMainLoop(timeout, windowInput);
}

} // namespace KODI::WINDOWING
