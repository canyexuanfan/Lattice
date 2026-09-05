#include <Windows.h>

#include "app/App.h"
#ifndef NDEBUG
#include "testing/SmokeCommands.h"
#endif
#include "util/ComInit.h"

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR commandLine,
    int showCommand) {
    ComInit com;
#ifndef NDEBUG
    if (const std::optional<int> commandResult =
            RunSmokeOrPreviewCommand(instance, commandLine)) {
        return *commandResult;
    }
#else
    (void)commandLine;
#endif

    App app(instance);
    if (!app.Initialize(showCommand)) {
        return 1;
    }
    return app.Run();
}
