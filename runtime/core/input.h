#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot; the game code window starts it earlier
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);
// the trigger motors of a controller that has them (Xbox One / Series: impulse triggers), for ms; false without
bool RumbleTriggers(int port, uint16_t left, uint16_t right, uint32_t ms);
// a relaunch: the ports that had a controller, handed to the new process (kRelaunchPads), which reports them plugged
// in (idle) until its own controllers turn up, so the game doesn't ask for a controller while they are found again
uint32_t ConnectedPorts();
inline constexpr wchar_t kRelaunchPads[] = L"XBE_RELAUNCH_PADS";

}  // namespace input
