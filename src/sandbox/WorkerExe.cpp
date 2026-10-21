// ShiftDropWorker.exe: the sealed engine host. It only ever runs as a child of ShiftDrop, in an
// AppContainer with no capabilities, on the handles it was given (src/sandbox/Worker.h).
#include "sandbox/Worker.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return sd::sandbox::WorkerMain(sd::sandbox::WorkerArgs());
}
