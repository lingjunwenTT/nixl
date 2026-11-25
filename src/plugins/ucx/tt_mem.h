/*
 * THROWAWAY EXPERIMENT: map Tenstorrent Blackhole DRAM into the host address
 * space so the UCX backend can register it like host memory.
 *
 * A TT_SEG descriptor is {addr = (DRAM bank << 32) | address inside the bank,
 * len, devId = N of /dev/tenstorrent/N}.
 */
#ifndef NIXL_SRC_PLUGINS_UCX_TT_MEM_H
#define NIXL_SRC_PLUGINS_UCX_TT_MEM_H

#include <cstdint>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "/usr/src/tenstorrent-2.10.0/ioctl.h"

struct nixlUcxTtWindow {
    int fd = -1;
    uint32_t tlbId = 0;
    void *map = nullptr;
    size_t mapSize = 0;
    void *va = nullptr; // host address of the descriptor's first byte
};

inline void
nixlUcxTtUnmap(nixlUcxTtWindow &w) {
    if (w.map) munmap(w.map, w.mapSize);
    if (w.fd >= 0) {
        tenstorrent_free_tlb f = {};
        f.in.id = w.tlbId;
        ioctl(w.fd, TENSTORRENT_IOCTL_FREE_TLB, &f);
        close(w.fd);
    }
    w = nixlUcxTtWindow();
}

// Returns 0 on success. The window is 2 MiB if the range fits in one, else 4 GiB.
inline int
nixlUcxTtMap(uint64_t dev_id, uint64_t addr, size_t len, nixlUcxTtWindow &w) {
    const uint64_t bank = addr >> 32;
    const uint64_t local = addr & 0xffffffffull;
    if (bank >= 8) return -1;

    const uint64_t small = 2ull << 20;
    const uint64_t win = (local % small + len <= small) ? small : (4ull << 30);
    const uint64_t win_base = local - local % win;
    if (local + len > win_base + win) return -1;

    w.fd = open(("/dev/tenstorrent/" + std::to_string(dev_id)).c_str(), O_RDWR | O_CLOEXEC);
    if (w.fd < 0) return -1;

    tenstorrent_allocate_tlb t = {};
    t.in.size = win;
    if (ioctl(w.fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &t) != 0) {
        close(w.fd);
        w.fd = -1;
        return -1;
    }
    w.tlbId = t.out.id;

    const char *ordering = getenv("NIXL_TT_ORDERING"); // 0 relaxed, 1 strict, 2 posted
    tenstorrent_configure_tlb c = {};
    c.in.id = t.out.id;
    c.in.config.addr = win_base;
    // Translated NoC coordinate of the bank's endpoint that reaches DRAM. Each bank has three
    // endpoints starting at x=17, y=12 (four banks per column); on CMFW 19.12 the other two
    // alias every 128 KiB. Only bank 0 (endpoint index 2) was checked.
    c.in.config.x_end = 17 + bank / 4;
    c.in.config.y_end = 12 + (bank % 4) * 3 + 2;
    c.in.config.ordering = ordering ? atoi(ordering) : 0;
    c.in.config.static_vc = getenv("NIXL_TT_NO_STATIC_VC") ? 0 : 1;
    if (ioctl(w.fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &c) != 0) {
        nixlUcxTtUnmap(w);
        return -1;
    }

    const char *uc = getenv("NIXL_TT_UNCACHED");
    void *p = mmap(nullptr, win, PROT_READ | PROT_WRITE, MAP_SHARED, w.fd,
                   uc ? t.out.mmap_offset_uc : t.out.mmap_offset_wc);
    if (p == MAP_FAILED) {
        nixlUcxTtUnmap(w);
        return -1;
    }
    w.map = p;
    w.mapSize = win;
    w.va = static_cast<char *>(p) + (local - win_base);
    return 0;
}

#endif
