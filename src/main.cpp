#include "app.h"

#include <windows.h>

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    App app(instance);
    if (!app.Initialize(show_command)) {
        return 1;
    }
    return app.Run();
}
