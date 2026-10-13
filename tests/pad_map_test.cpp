#include <cstdio>
#include "../src/pad_map.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    using namespace pad;
    XboxState s{};
    CHECK(to_ps1(s) == 0);
    CHECK(to_wire(to_ps1(s)) == 0xFFFF);          // nothing pressed -> all high

    s = {}; s.a = true;
    CHECK(to_ps1(s) == CROSS);
    s = {}; s.y = true; s.b = true;
    CHECK(to_ps1(s) == (TRIANGLE | CIRCLE));
    s = {}; s.lb = true; s.rt = 30000;
    CHECK(to_ps1(s) == (L1 | R2));
    s = {}; s.lt = 5000;                            // below threshold
    CHECK(to_ps1(s) == 0);

    s = {}; s.lx = -20000;                          // stick left
    CHECK(to_ps1(s) == LEFT);
    s = {}; s.ly = -20000;                          // stick up (SDL: negative)
    CHECK(to_ps1(s) == UP);
    s = {}; s.lx = 8000;                            // inside deadzone
    CHECK(to_ps1(s) == 0);

    s = {}; s.dpad_up = true; s.ly = 30000;         // d-pad up + stick down cancel
    CHECK(to_ps1(s) == 0);

    s = {}; s.start = true; s.back = true;
    CHECK(to_ps1(s) == (START | SELECT));


    {   // TapFilter: a long D-pad hold becomes a 2-frame pulse; other buttons pass through; release re-arms
        TapFilter tf;
        int seen = 0;
        for (int i = 0; i < 10; ++i) seen += (tf.apply(DOWN | CROSS) & DOWN) ? 1 : 0;
        CHECK(seen == 2);
        CHECK((tf.apply(DOWN | CROSS) & CROSS) != 0);   // CROSS is never filtered
        CHECK(tf.apply(0) == 0);                          // released
        CHECK((tf.apply(DOWN) & DOWN) != 0);              // next press works again
        TapFilter off; off.enabled = false;
        int seen2 = 0; for (int i = 0; i < 10; ++i) seen2 += (off.apply(DOWN) & DOWN) ? 1 : 0;
        CHECK(seen2 == 10);
    }

    if (failures == 0) std::puts("pad_map: all tests passed");
    return failures ? 1 : 0;
}
