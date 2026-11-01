/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <memory>

namespace XbmcThreads
{

/*!
 \brief Auto-reset wake-up signal with a precise timeout.

 Wait() returns as soon as Signal() is called (or, optionally, window input arrives) and otherwise after the
 timeout. On Windows the timeout uses a high-resolution waitable timer, so a 1 ms timeout really is ~1 ms even
 without timeBeginPeriod (UWP); elsewhere it is a CEvent.
 */
class CWakeSignal
{
public:
  CWakeSignal();
  ~CWakeSignal();
  CWakeSignal(const CWakeSignal&) = delete;
  CWakeSignal& operator=(const CWakeSignal&) = delete;

  void Signal();

  /*!
   \param timeout maximum wait
   \param windowInput also wake on window input/messages for the calling thread (Windows desktop only)
   \return true if woken by Signal() or window input, false on timeout
   */
  bool Wait(std::chrono::nanoseconds timeout, bool windowInput = false);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace XbmcThreads
