/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WakeSignal.h"

#if defined(TARGET_WINDOWS)
#include "utils/XTimeUtils.h"

#include <Windows.h>
#else
#include "Event.h"
#endif

namespace XbmcThreads
{

#if defined(TARGET_WINDOWS)
struct CWakeSignal::Impl
{
  HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS); // auto-reset, unsignaled
  ~Impl()
  {
    if (event)
      CloseHandle(event);
  }
};
#else
struct CWakeSignal::Impl
{
  CEvent event;
};
#endif

CWakeSignal::CWakeSignal() : m_impl(std::make_unique<Impl>())
{
}

CWakeSignal::~CWakeSignal() = default;

void CWakeSignal::Signal()
{
#if defined(TARGET_WINDOWS)
  if (m_impl->event)
    SetEvent(m_impl->event);
#else
  m_impl->event.Set();
#endif
}

bool CWakeSignal::Wait(std::chrono::nanoseconds timeout, bool windowInput)
{
#if defined(TARGET_WINDOWS)
  HANDLE handles[2] = {m_impl->event, KODI::TIME::ArmHighResolutionTimer(timeout)};
  const DWORD count = handles[1] ? 2 : 1;
  // without a high-resolution timer fall back to a (tick-rounded) millisecond timeout
  const DWORD ms = handles[1] ? INFINITE
                              : static_cast<DWORD>(
                                    std::chrono::ceil<std::chrono::milliseconds>(timeout).count());
  if (!m_impl->event)
  {
    KODI::TIME::Sleep(timeout);
    return false;
  }

  DWORD res;
#if defined(TARGET_WINDOWS_DESKTOP)
  if (windowInput)
    // input, posted and sent messages; not WM_PAINT/WM_TIMER, which can be permanently pending
    res = MsgWaitForMultipleObjectsEx(count, handles, ms,
                                      QS_INPUT | QS_POSTMESSAGE | QS_SENDMESSAGE | QS_HOTKEY,
                                      MWMO_INPUTAVAILABLE);
  else
#endif
    res = WaitForMultipleObjectsEx(count, handles, FALSE, ms, FALSE);

  // event (index 0) or window input (index == count) wake us; the timer (index 1) or WAIT_TIMEOUT do not
  return res == WAIT_OBJECT_0 || res == WAIT_OBJECT_0 + count;
#else
  return m_impl->event.Wait(timeout);
#endif
}

} // namespace XbmcThreads
