// PS1 GTE (COP2). Implements RTPS/RTPT/NCLIP/AVSZ3/AVSZ4/MVMVA/SQR/OP/GPF/GPL.
// Lighting / colour commands are not implemented yet and are counted in `unsupported`.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hw {

inline int clz32(uint32_t x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; ++n; } return n; }

class Gte {
public:
    int32_t d[32] = {};   // data registers
    int32_t c[32] = {};   // control registers
    uint64_t unsupported = 0;
    uint32_t last_unsupported = 0;

    uint32_t read(int r) const {
        switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)(int16_t)d[r];
        case 7: case 16: case 17: case 18: case 19: return (uint32_t)(uint16_t)d[r];
        case 15: return (uint32_t)d[14];                       // SXYP reads SXY2
        case 28: case 29: {                                   // IRGB / ORGB
            auto cv = [](int32_t v) { int x = v / 0x80; return (uint32_t)std::clamp(x, 0, 0x1F); };
            return cv(d[9]) | cv(d[10]) << 5 | cv(d[11]) << 10;
        }
        default: return (uint32_t)d[r];
        }
    }
    void write(int r, uint32_t v) {
        switch (r) {
        case 15: d[12] = d[13]; d[13] = d[14]; d[14] = (int32_t)v; return;   // SXYP push
        case 28: d[9] = (v & 0x1F) * 0x80; d[10] = ((v >> 5) & 0x1F) * 0x80; d[11] = ((v >> 10) & 0x1F) * 0x80; return;
        case 29: case 31: return;
        case 30: { d[30] = (int32_t)v; uint32_t x = v & 0x80000000u ? ~v : v; d[31] = x ? clz32(x) : 32; return; }
        default: d[r] = (int32_t)v;
        }
    }
    uint32_t read_ctl(int r) const {
        if (r == 4 || r == 12 || r == 20 || r == 26 || r == 27 || r == 29 || r == 30) return (uint32_t)(int32_t)(int16_t)c[r];  // 16-bit regs sign-extend (H is u16 but reads back sign-extended)
        return (uint32_t)c[r];
    }
    void write_ctl(int r, uint32_t v) { if (r == 31) { c[31] = (int32_t)(v & 0x7FFFF000u); return; } c[r] = (int32_t)v; }

    void exec(uint32_t cmd) {
        sf_ = (cmd >> 19) & 1; lm_ = (cmd >> 10) & 1;
        int mx = (cmd >> 17) & 3, vx = (cmd >> 15) & 3, tx = (cmd >> 13) & 3;
        c[31] = 0;
        switch (cmd & 0x3F) {
        case 0x01: rtp(0, true); break;
        case 0x30: rtp(0, false); rtp(1, false); rtp(2, true); break;
        case 0x06: {
            int64_t m = (int64_t)sx(0) * (sy(1) - sy(2)) + (int64_t)sx(1) * (sy(2) - sy(0)) + (int64_t)sx(2) * (sy(0) - sy(1));
            d[24] = (int32_t)m; break; }
        case 0x2D: { int64_t m = (int64_t)(int16_t)c[29] * (u16(17) + u16(18) + u16(19)); d[24] = (int32_t)m; d[7] = std::clamp<int64_t>(m >> 12, 0, 0xFFFF); break; }
        case 0x2E: { int64_t m = (int64_t)(int16_t)c[30] * (u16(16) + u16(17) + u16(18) + u16(19)); d[24] = (int32_t)m; d[7] = std::clamp<int64_t>(m >> 12, 0, 0xFFFF); break; }
        case 0x12: mvmva(mx, vx, tx); break;
        case 0x28: {  // SQR
            for (int i = 0; i < 3; ++i) { int64_t v = (int64_t)(int16_t)d[9 + i] * (int16_t)d[9 + i]; set_mac(i, v); }
            ir_from_mac(); break; }
        case 0x0C: {  // OP: cross product of IR with the RT diagonal
            int64_t r1 = (int16_t)(c[0] & 0xFFFF), r2 = (int16_t)(c[2] & 0xFFFF), r3 = (int16_t)(c[4] & 0xFFFF);  // RT11, RT22, RT33
            int64_t i1 = (int16_t)d[9], i2 = (int16_t)d[10], i3 = (int16_t)d[11];
            set_mac(0, i3 * r2 - i2 * r3); set_mac(1, i1 * r3 - i3 * r1); set_mac(2, i2 * r1 - i1 * r2);
            ir_from_mac(); break; }
        case 0x3D: {  // GPF
            int64_t ir0 = (int16_t)d[8];
            for (int i = 0; i < 3; ++i) set_mac(i, (int64_t)(int16_t)d[9 + i] * ir0);
            ir_from_mac(); push_color(); break; }
        case 0x3E: {  // GPL
            int64_t ir0 = (int16_t)d[8];
            for (int i = 0; i < 3; ++i) {
                int64_t base = (int64_t)d[25 + i] << (sf_ ? 12 : 0);
                set_mac(i, base + (int64_t)(int16_t)d[9 + i] * ir0, false);
            }
            ir_from_mac(); push_color(); break; }
        default: ++unsupported; last_unsupported = cmd; break;
        }
    }

private:
    int sf_ = 0, lm_ = 0;
    int32_t sx(int n) const { return (int16_t)(d[12 + n] & 0xFFFF); }
    int32_t sy(int n) const { return (int16_t)((uint32_t)d[12 + n] >> 16); }
    uint32_t u16(int r) const { return (uint16_t)d[r]; }

    void set_mac(int i, int64_t v, bool shift = true) { d[25 + i] = (int32_t)(shift && sf_ ? v >> 12 : v); }
    void ir_from_mac() {
        for (int i = 0; i < 3; ++i) d[9 + i] = std::clamp<int32_t>(d[25 + i], lm_ ? 0 : -0x8000, 0x7FFF);
    }
    void push_color() {
        d[20] = d[21]; d[21] = d[22];
        auto cl = [](int32_t v) { return (uint32_t)std::clamp(v >> 4, 0, 255); };
        d[22] = (int32_t)(cl(d[25]) | cl(d[26]) << 8 | cl(d[27]) << 16 | ((uint32_t)d[6] & 0xFF000000u));
    }
    void push_sz(int32_t z) { d[16] = d[17]; d[17] = d[18]; d[18] = d[19]; d[19] = std::clamp(z, 0, 0xFFFF); }
    void push_sxy(int32_t x, int32_t y) {
        d[12] = d[13]; d[13] = d[14];
        d[14] = (int32_t)((uint32_t)(uint16_t)std::clamp(x, -0x400, 0x3FF) | (uint32_t)std::clamp(y, -0x400, 0x3FF) << 16);
    }
    void vec(int v, int64_t out[3]) const {
        if (v == 3) { out[0] = (int16_t)d[9]; out[1] = (int16_t)d[10]; out[2] = (int16_t)d[11]; return; }
        out[0] = (int16_t)(d[v * 2] & 0xFFFF); out[1] = (int16_t)((uint32_t)d[v * 2] >> 16); out[2] = (int16_t)d[v * 2 + 1];
    }
    void matrix(int m, int64_t out[3][3]) const {
        if (m == 3) { std::memset(out, 0, sizeof(int64_t) * 9); return; }  // reserved selector (hardware quirk not modelled)
        int base = m * 8;
        int32_t w0 = c[base], w1 = c[base + 1], w2 = c[base + 2], w3 = c[base + 3], w4 = c[base + 4];
        out[0][0] = (int16_t)(w0 & 0xFFFF); out[0][1] = (int16_t)((uint32_t)w0 >> 16); out[0][2] = (int16_t)(w1 & 0xFFFF);
        out[1][0] = (int16_t)((uint32_t)w1 >> 16); out[1][1] = (int16_t)(w2 & 0xFFFF); out[1][2] = (int16_t)((uint32_t)w2 >> 16);
        out[2][0] = (int16_t)(w3 & 0xFFFF); out[2][1] = (int16_t)((uint32_t)w3 >> 16); out[2][2] = (int16_t)(w4 & 0xFFFF);
    }
    void mvmva(int mx, int vx, int tx) {
        int64_t M[3][3], V[3];
        matrix(mx, M); vec(vx, V);
        int64_t T[3] = {0, 0, 0};
        if (tx != 3) { int b = tx == 0 ? 5 : tx == 1 ? 13 : 21; for (int i = 0; i < 3; ++i) T[i] = c[b + i]; }
        for (int i = 0; i < 3; ++i) set_mac(i, (T[i] << 12) + M[i][0] * V[0] + M[i][1] * V[1] + M[i][2] * V[2]);
        ir_from_mac();
    }
    // Reciprocal-based divide as performed by RTPS/RTPT.
    uint32_t divide(uint32_t h, uint32_t sz3) const {
        if (sz3 * 2 <= h) return 0x1FFFF;
        static int table[257]; static bool init = false;
        if (!init) { for (int i = 0; i <= 0x100; ++i) table[i] = std::max(0, ((0x40000 / (i + 0x100)) + 1) / 2 - 0x101); init = true; }
        uint32_t shift = sz3 ? (uint32_t)clz32(sz3 & 0xFFFF) - 16 : 16;
        uint64_t lhs = (uint64_t)h << shift; uint32_t rhs = sz3 << shift;
        uint32_t divisor = rhs | 0x8000;
        int32_t x = 0x101 + table[((divisor & 0x7FFF) + 0x40) >> 7];
        int32_t dd = (int32_t)(((int64_t)divisor * -x + 0x80) >> 8);
        int64_t res = ((int64_t)x * (0x20000 + dd) + 0x80) >> 8;
        return (uint32_t)std::min<int64_t>(0x1FFFF, ((int64_t)lhs * res + 0x8000) >> 16);
    }
    void rtp(int v, bool last) {
        int64_t M[3][3], V[3]; matrix(0, M); vec(v, V);
        int64_t mac[3];
        for (int i = 0; i < 3; ++i) mac[i] = ((int64_t)c[5 + i] << 12) + M[i][0] * V[0] + M[i][1] * V[1] + M[i][2] * V[2];
        for (int i = 0; i < 3; ++i) d[25 + i] = (int32_t)(sf_ ? mac[i] >> 12 : mac[i]);
        ir_from_mac();
        int32_t z = (int32_t)(mac[2] >> 12);  // SZ3 = unshifted MAC3 >> 12
        push_sz(z);
        uint32_t q = divide((uint32_t)(uint16_t)c[26], (uint32_t)(uint16_t)d[19]);
        int64_t X = ((int64_t)c[24] + (int64_t)(int16_t)d[9] * q) >> 16;
        int64_t Y = ((int64_t)c[25] + (int64_t)(int16_t)d[10] * q) >> 16;
        push_sxy((int32_t)std::clamp<int64_t>(X, -0x8000, 0x7FFF), (int32_t)std::clamp<int64_t>(Y, -0x8000, 0x7FFF));
        if (last) {
            int64_t m0 = (int64_t)q * (int16_t)c[27] + c[28];
            d[24] = (int32_t)m0; d[8] = (int32_t)std::clamp<int64_t>(m0 >> 12, 0, 0x1000);
        }
    }
};

}  // namespace hw
