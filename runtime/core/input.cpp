// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};
bool g_trigger_failed[4] = {};  // the first failed trigger rumble on a port is logged
// in release logs too (issue #6): the first rumble the game sets on a port, its first non-zero one, the first trigger
// rumble, and the first failed main rumble, so a log tells "the game never asked" from "the pad refused"
struct RumbleSeen {
    bool any, nonzero, trigger, failed;
};
RumbleSeen g_seen[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    int n = int(slot - g_pads) + 1;
    if (!*slot) {
        LOGR("input: controller %d failed to open: %s", n, SDL_GetError());
        return;
    }
    g_trigger_failed[n - 1] = false;
    g_seen[n - 1] = {};
    // in release logs too: players' reports need the pad and what SDL says it can do (issue #6)
    const char* name = SDL_GetGamepadName(*slot);
    const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadType(*slot));
    SDL_PropertiesID props = SDL_GetGamepadProperties(*slot);
    LOGR("input: controller %d plugged in: %s (%04x:%04x, %s), rumble %s, trigger rumble %s", n, name ? name : "",
         SDL_GetGamepadVendor(*slot), SDL_GetGamepadProduct(*slot),
         type ? type : "unknown",
         SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no",
         SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false) ? "yes" : "no");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGR("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

}  // namespace

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGR("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    const uint16_t asked_left = left, asked_right = right;
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port < 0 || port >= 4) return;
    RumbleSeen& seen = g_seen[port];
    SDL_Gamepad* pad = g_pads[port];
    const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
    if (!name) name = pad ? "" : "no controller";
    bool ok = pad && SDL_RumbleGamepad(pad, left, right, 0);
    auto log = [&](const char* what) {
        if (pad && !ok) seen.failed = true;  // a failure is logged once
        LOGR("input: controller %d (%s) %s: game %u/%u, sent %u/%u, %s%s", port + 1, name, what,
             unsigned(asked_left), unsigned(asked_right), unsigned(left), unsigned(right), ok ? "ok" : "failed: ",
             ok ? "" : (pad ? SDL_GetError() : "not plugged in"));
    };
    if (!seen.any) {
        seen.any = true;
        log("first rumble");
    }
    if (!seen.nonzero && (asked_left || asked_right || left || right)) {
        seen.nonzero = true;
        log("first non-zero rumble");
    }
    if (pad && !ok && !seen.failed) {
        seen.failed = true;
        log("rumble");
    }
}

bool RumbleTriggers(int port, uint16_t left, uint16_t right, uint32_t ms) {
    std::lock_guard lock(g_lock);
    if (port < 0 || port >= 4 || !g_pads[port]) return false;
    bool ok = SDL_RumbleGamepadTriggers(g_pads[port], left, right, ms);
    if (!g_seen[port].trigger && (left || right)) {
        g_seen[port].trigger = true;
        LOGR("input: controller %d first non-zero trigger rumble: %u/%u for %u ms, %s%s", port + 1,
             unsigned(left), unsigned(right), unsigned(ms), ok ? "ok" : "failed: ", ok ? "" : SDL_GetError());
    }
    if (ok) return true;
    if (!g_trigger_failed[port]) {
        g_trigger_failed[port] = true;
        LOGR("input: controller %d trigger rumble failed: %s", port + 1, SDL_GetError());
    }
    return false;
}

}  // namespace input
