/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "powermanagement/WinIdleTimer.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPowerHandling.h"

#include <chrono>

#include <winrt/Windows.System.Display.h>

using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::System::Display;
using namespace winrt::Windows::UI::Core;

void CWinIdleTimer::StartZero()
{
  static DisplayRequest displayRequest = nullptr;
  // This runs for every rendered frame of fullscreen video; the display idle timeout is minutes, so
  // resetting it every 10 s is plenty instead of queueing a dispatcher work item 60-120 times a second.
  static std::chrono::steady_clock::time_point lastRequest{};
  constexpr auto requestInterval = std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPower = components.GetComponent<CApplicationPowerHandling>();
  if (!appPower->IsDPMSActive() && now - lastRequest >= requestInterval)
  {
    lastRequest = now;
    if (!displayRequest)
    {
      try
      {
        // this may throw an exception
        displayRequest = DisplayRequest();
      }
      catch (const winrt::hresult_error&) 
      {
        return;
      }
    }

    auto workItem = DispatchedHandler([&]()
    {
      try
      {
        // this couple of calls activate and deactivate a display-required
        // request in other words they reset display idle timer
        displayRequest.RequestActive();
        displayRequest.RequestRelease();
      }
      catch (const winrt::hresult_error&) { }
    });
    CoreWindow window = CoreApplication::MainView().CoreWindow();
    window.Dispatcher().RunAsync(CoreDispatcherPriority::High, workItem);
  }
  CStopWatch::StartZero();
}
