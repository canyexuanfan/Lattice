#include <Windows.h>

#include "app/App.h"
#include "testing/SmokeCommands.h"
#include "util/ComInit.h"

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR commandLine,
    int showCommand) {
    ComInit com;
    if (const std::optional<int> commandResult =
            RunSmokeOrPreviewCommand(instance, commandLine)) {
        return *commandResult;
    }

    App app(instance);
    if (!app.Initialize(showCommand)) {
        return 1;
    }
    return app.Run();
}
