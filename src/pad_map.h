// Maps a generic "Xbox-style" gamepad state to a PS1 digital pad word.
// Kept free of SDL so it can be unit-tested anywhere.
#pragma once
#include <cstdint>

namespace pad {

// PS1 digital pad button bits. The pad hardware reports buttons ACTIVE-LOW
// on the wire; we keep an active-HIGH word here and invert at the HLE edge.
enum Ps1Button : uint16_t {
    SELECT   = 1u << 0,
    START    = 1u << 3,
    UP       = 1u << 4,
    RIGHT    = 1u << 5,
    DOWN     = 1u << 6,
    LEFT     = 1u << 7,
    L2       = 1u << 8,
    R2       = 1u << 9,
    L1       = 1u << 10,
    R1       = 1u << 11,
    TRIANGLE = 1u << 12,
    CIRCLE   = 1u << 13,
    CROSS    = 1u << 14,
    SQUARE   = 1u << 15,
};

// Xbox 360 layout (what SDL_GameController exposes, A/B/X/Y by position).
struct XboxState {
    bool a, b, x, y;
    bool lb, rb;
    bool back, start;
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    int16_t lx, ly;   // left stick, -32768..32767 (up is negative, as in SDL)
    int16_t lt, rt;   // triggers, 0..32767
};

constexpr int16_t kStickDeadzone = 16000;   // ~49%: stick acts as a d-pad
constexpr int16_t kTriggerThreshold = 12000;

inline uint16_t to_ps1(const XboxState& s) {
    uint16_t w = 0;
    // Face buttons by position: A=bottom=Cross, B=right=Circle,
    // X=left=Square, Y=top=Triangle.
    if (s.a) w |= CROSS;
    if (s.b) w |= CIRCLE;
    if (s.x) w |= SQUARE;
    if (s.y) w |= TRIANGLE;
    if (s.lb) w |= L1;
    if (s.rb) w |= R1;
    if (s.lt > kTriggerThreshold) w |= L2;
    if (s.rt > kTriggerThreshold) w |= R2;
    if (s.back) w |= SELECT;
    if (s.start) w |= START;
    if (s.dpad_up || s.ly < -kStickDeadzone) w |= UP;
    if (s.dpad_down || s.ly > kStickDeadzone) w |= DOWN;
    if (s.dpad_left || s.lx < -kStickDeadzone) w |= LEFT;
    if (s.dpad_right || s.lx > kStickDeadzone) w |= RIGHT;
    // Opposing directions can't both be pressed on a real d-pad.
    if ((w & UP) && (w & DOWN)) w &= ~(UP | DOWN);
    if ((w & LEFT) && (w & RIGHT)) w &= ~(LEFT | RIGHT);
    return w;
}

// What a game polling the pad via the BIOS/libpad expects: active-low.
inline uint16_t to_wire(uint16_t active_high) { return static_cast<uint16_t>(~active_high); }

// The game's menus step TWICE for a D-pad press that lasts 3+ frames (a human tap is 5-8 frames), so the cursor
// skips items (GAME -> LANGUAGE, jumping over HOW TO PLAY). TapFilter passes each new D-pad press through for
// `frames` frames only, then masks that direction until it is released. Face/shoulder/Start buttons are untouched.
struct TapFilter {
    bool enabled = true;
    int frames = 2;
    uint16_t apply(uint16_t w) {
        static const uint16_t kBits[4] = {UP, DOWN, LEFT, RIGHT};
        if (!enabled) { for (int& h : held_) h = 0; return w; }
        uint16_t out = static_cast<uint16_t>(w & ~(UP | DOWN | LEFT | RIGHT));
        for (int i = 0; i < 4; ++i) {
            if (w & kBits[i]) { if (held_[i] < 1000000) ++held_[i]; if (held_[i] <= frames) out |= kBits[i]; }
            else held_[i] = 0;
        }
        return out;
    }
private:
    int held_[4] = {0, 0, 0, 0};
};

}  // namespace pad
