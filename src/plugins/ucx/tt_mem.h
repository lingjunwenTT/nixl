/*
 * THROWAWAY EXPERIMENT: map Tenstorrent Blackhole DRAM into the host address
 * space so the UCX backend can register it like host memory.
 */
#ifndef NIXL_SRC_PLUGINS_UCX_TT_MEM_H
#define NIXL_SRC_PLUGINS_UCX_TT_MEM_H

#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// tt-kmd's ioctl header ships with the driver source.
#if __has_include("/usr/src/tenstorrent-2.11.0/ioctl.h")
#include "/usr/src/tenstorrent-2.11.0/ioctl.h"
#else
#include "/usr/src/tenstorrent-2.10.0/ioctl.h"
#endif

// One whole DRAM bank of a card, mapped through a 4 GiB tt-kmd TLB window.
// A card has 8 such windows, shared by every process that uses it.
struct nixlUcxTtWindow {
    static constexpr uint64_t size = 4ull << 30; // of the window
    // Usable bytes in a bank: the top 16 MiB of the window is not DRAM
    // (dram_bank_size in tt-metal's blackhole_140_arch.yaml).
    static constexpr uint64_t bankSize = 0xFF000000ull;

    int fd = -1;
    char *map = nullptr; // host address of the bank's address 0

    nixlUcxTtWindow() = default;
    nixlUcxTtWindow(const nixlUcxTtWindow &) = delete;
    nixlUcxTtWindow &
    operator=(const nixlUcxTtWindow &) = delete;

    ~nixlUcxTtWindow() {
        if (map) munmap(map, size);
        if (fd >= 0) close(fd); // tt-kmd frees the window with the fd
    }

    // Maps DRAM bank `bank` (0-7) of /dev/tenstorrent/<dev_id>. Call once.
    bool
    open(uint64_t dev_id, uint64_t bank) {
        if (bank >= 8) return false;
        fd = ::open(("/dev/tenstorrent/" + std::to_string(dev_id)).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return false;

        tenstorrent_allocate_tlb alloc = {};
        alloc.in.size = size;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0) return false;

        // Point the window at the bank: the translated NoC coordinate of the bank's endpoint
        // that reaches DRAM. Each bank has three endpoints starting at x=17, y=12 (four banks
        // per column); on CMFW 19.12 the first two alias every 128 KiB. Only bank 0 was checked.
        tenstorrent_configure_tlb cfg = {};
        cfg.in.id = alloc.out.id;
        cfg.in.config.x_end = 17 + bank / 4;
        cfg.in.config.y_end = 12 + (bank % 4) * 3 + 2;
        cfg.in.config.static_vc = 1;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0) return false;

        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc.out.mmap_offset_wc);
        if (p == MAP_FAILED) return false;
        map = static_cast<char *>(p);
        return true;
    }
};

#endif
