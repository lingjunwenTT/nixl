/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Tenstorrent USA, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef NIXL_SRC_PLUGINS_UCX_TT_TT_MEM_H
#define NIXL_SRC_PLUGINS_UCX_TT_TT_MEM_H

#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ioctl.h"

/**
 * One DRAM bank of a Tenstorrent Blackhole card, mapped into this process.
 *
 * A card has 8 DRAM banks. The card's driver (tt-kmd) can map a bank into a
 * process through a "TLB window": a range of host addresses whose reads and
 * writes go to the card. A card has 8 windows of this size, shared by every
 * process that uses it.
 */
struct nixlUcxTtWindow {
    static constexpr uint64_t numBanks = 8;
    static constexpr uint64_t windowSize = 4ull << 30;
    // The top 16 MiB of the window is not DRAM (dram_bank_size in tt-metal's
    // blackhole_140_arch.yaml).
    static constexpr uint64_t bankSize = 0xFF000000ull;
    // Which of a bank's three endpoints reaches its DRAM: worker_endpoint (first
    // value) of each dram_view in tt-metal's blackhole_140_arch.yaml. Firmware that
    // reports relocated DRAM endpoints needs relocated_worker_endpoint instead.
    static constexpr int dramEndpoint[numBanks] = {2, 0, 0, 0, 2, 2, 2, 2};

    int fd = -1;
    char *map = nullptr; // host address of the bank's address 0

    nixlUcxTtWindow() = default;
    nixlUcxTtWindow(const nixlUcxTtWindow &) = delete;
    nixlUcxTtWindow &
    operator=(const nixlUcxTtWindow &) = delete;

    ~nixlUcxTtWindow() {
        if (map) {
            munmap(map, windowSize);
        }
        if (fd >= 0) {
            close(fd); // the driver frees the window with the fd
        }
    }

    /**
     * Map DRAM bank @a bank of the card /dev/tenstorrent/<dev_id>.
     * Call at most once per object. Returns false on failure.
     */
    [[nodiscard]] bool
    open(uint64_t dev_id, uint64_t bank) {
        if (bank >= numBanks) {
            return false;
        }

        const std::string path = "/dev/tenstorrent/" + std::to_string(dev_id);
        fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }

        tenstorrent_allocate_tlb alloc = {};
        alloc.in.size = windowSize;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0) {
            return false;
        }

        // Point the window at the bank. The banks sit in two columns of four on the
        // card's internal network, and each has three endpoints there. Once the card
        // is initialized, only one of the three reaches the bank's DRAM.
        tenstorrent_configure_tlb config = {};
        config.in.id = alloc.out.id;
        config.in.config.x_end = 17 + bank / 4;
        config.in.config.y_end = 12 + (bank % 4) * 3 + dramEndpoint[bank];
        config.in.config.static_vc = 1;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &config) != 0) {
            return false;
        }

        void *addr = mmap(
            nullptr, windowSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc.out.mmap_offset_wc);
        if (addr == MAP_FAILED) {
            return false;
        }
        map = static_cast<char *>(addr);
        return true;
    }
};

#endif
