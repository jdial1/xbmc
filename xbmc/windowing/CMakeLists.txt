set(SOURCES GraphicContext.cpp
            MainLoopWake.cpp
            OSScreenSaver.cpp
            Resolution.cpp
            WindowSystemFactory.cpp
            WinSystem.cpp)

set(HEADERS GraphicContext.h
            MainLoopWake.h
            OSScreenSaver.h
            Resolution.h
            WinEvents.h
            WindowSystemFactory.h
            WinSystem.h
            XBMC_events.h
            VideoSync.h)

core_add_library(windowing)
