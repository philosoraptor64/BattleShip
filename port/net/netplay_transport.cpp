#include <cstdio>
/* Netplay transport when a host app owns the connection (Smash as an i64
 * module: port/i64_module.cpp). netpeer.c then sends and receives through
 * these instead of its own UDP socket, and skips the room server.
 *
 * The host's game channel carries datagrams up to max_datagram bytes;
 * Smash's start-up handshake sends ~1.6 KB (the host's save data), so
 * bigger packets are split: every datagram starts with one byte, 0 = whole
 * packet, 1 = fragment (then u16 id, u8 index, u8 count). Fragments of one
 * packet are reassembled; an incomplete packet is dropped when a newer one
 * starts (the handshake resends until answered). */
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
/* Set by the module while a run uses the host's connection. */
int (*gPortNetHostSend)(const void* data, uint32_t size) = nullptr;
int (*gPortNetHostRecv)(void* buf, uint32_t cap) = nullptr;
uint32_t gPortNetHostMaxDatagram = 1200;
}

namespace {
constexpr uint8_t kWhole = 0, kFragment = 1;
constexpr int kFragHeader = 1 + 2 + 1 + 1;
uint16_t sNextFragId = 1;

struct Reassembly {
    uint16_t id = 0;
    uint8_t count = 0;
    uint8_t received = 0;
    std::vector<std::vector<uint8_t>> parts;
};
Reassembly sAssembly;
} // namespace

// Inside i64: why this side ends the run ("" = it doesn't). The decomp sets it
// on a desync or when the session ends; the module returns EXIT and i64 shows it.
extern "C" int port_net_external_active(void);
static char sPortNetEndReason[200];
extern "C" void port_net_set_end_reason(const char *why) {
    if (port_net_external_active() && sPortNetEndReason[0] == 0 && why != nullptr) {
        snprintf(sPortNetEndReason, sizeof(sPortNetEndReason), "%s", why);
    }
}
extern "C" const char *port_net_end_reason(void) {
    return sPortNetEndReason;
}

extern "C" int port_net_external_active(void) {
    return gPortNetHostSend != nullptr && gPortNetHostRecv != nullptr;
}

/* Returns 1 if the whole packet was handed to the host. */
extern "C" int port_net_external_send(const uint8_t* data, uint32_t size) {
    if (!port_net_external_active()) return 0;
    uint8_t buf[2048];
    const uint32_t max = gPortNetHostMaxDatagram < sizeof buf ? gPortNetHostMaxDatagram : (uint32_t)sizeof buf;
    if (size + 1 <= max) {
        buf[0] = kWhole;
        std::memcpy(buf + 1, data, size);
        return gPortNetHostSend(buf, size + 1) >= 0;
    }
    const uint32_t chunk = max - kFragHeader;
    const uint32_t count = (size + chunk - 1) / chunk;
    if (count > 255) return 0;
    const uint16_t id = sNextFragId++;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t off = i * chunk;
        const uint32_t n = (size - off < chunk) ? size - off : chunk;
        buf[0] = kFragment;
        buf[1] = (uint8_t)(id >> 8);
        buf[2] = (uint8_t)(id & 0xFF);
        buf[3] = (uint8_t)i;
        buf[4] = (uint8_t)count;
        std::memcpy(buf + kFragHeader, data + off, n);
        if (gPortNetHostSend(buf, n + kFragHeader) < 0) return 0;
    }
    return 1;
}

/* Next whole packet into `out` (cap bytes): its size, or -1 if none waiting. */
extern "C" int port_net_external_recv(uint8_t* out, uint32_t cap) {
    if (!port_net_external_active()) return -1;
    uint8_t buf[2048];
    for (;;) {
        const int n = gPortNetHostRecv(buf, sizeof buf);
        if (n <= 0) return -1;
        if (buf[0] == kWhole) {
            const uint32_t size = (uint32_t)n - 1;
            if (size > cap) continue;
            std::memcpy(out, buf + 1, size);
            return (int)size;
        }
        if (buf[0] != kFragment || n < kFragHeader) continue;
        const uint16_t id = (uint16_t)((buf[1] << 8) | buf[2]);
        const uint8_t index = buf[3], count = buf[4];
        if (count == 0 || index >= count) continue;
        if (sAssembly.id != id || sAssembly.count != count) {
            sAssembly = Reassembly();
            sAssembly.id = id;
            sAssembly.count = count;
            sAssembly.parts.assign(count, {});
        }
        if (!sAssembly.parts[index].empty()) continue;  // duplicate
        sAssembly.parts[index].assign(buf + kFragHeader, buf + n);
        if (++sAssembly.received < count) continue;
        uint32_t total = 0;
        for (auto& p : sAssembly.parts) total += (uint32_t)p.size();
        if (total > cap) {
            sAssembly = Reassembly();
            continue;
        }
        uint32_t at = 0;
        for (auto& p : sAssembly.parts) {
            std::memcpy(out + at, p.data(), p.size());
            at += (uint32_t)p.size();
        }
        sAssembly = Reassembly();
        return (int)total;
    }
}
