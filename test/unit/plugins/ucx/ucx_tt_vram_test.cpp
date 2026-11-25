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

// Transfers between host memory and Tenstorrent card memory through the UCX
// backend, with both agents in this process:
//   host -> card a, card a -> host, card a -> card b.
// Every transfer is checked byte for byte, and the card memory the test
// overwrites is saved first and restored at the end.
//
// Usage: ucx_tt_vram_test [MiB] [card_a] [card_b] [bank]    (default: 2 0 1 0)
// Exits with 77 (skipped) if a card is not present.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <nixl.h>

#include "tt/tt_mem.h"

namespace {

const int exit_skipped = 77;
const uint64_t scratch_addr_in_bank = 0x10000000ull;

void
require(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(EXIT_FAILURE);
    }
}

double
now() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
}

struct region {
    nixl_mem_t type;
    uint64_t addr;
    uint64_t devId;
};

// Runs one transfer of len bytes on the initiator and returns the seconds it took.
double
transfer(nixlAgent &initiator,
         nixlBackendH *backend,
         nixl_xfer_op_t op,
         const region &local,
         const region &remote,
         size_t len) {
    nixl_xfer_dlist_t local_descs(local.type), remote_descs(remote.type);
    local_descs.addDesc(nixlBasicDesc(local.addr, len, local.devId));
    remote_descs.addDesc(nixlBasicDesc(remote.addr, len, remote.devId));

    nixl_opt_args_t args;
    args.backends.push_back(backend);

    nixlXferReqH *req;
    require(initiator.createXferReq(op, local_descs, remote_descs, "target", req, &args) ==
                NIXL_SUCCESS,
            "create transfer request");

    const double start = now();
    nixl_status_t status = initiator.postXferReq(req);
    while (status == NIXL_IN_PROG) {
        status = initiator.getXferStatus(req);
    }
    const double seconds = now() - start;

    initiator.releaseXferReq(req);
    require(status == NIXL_SUCCESS, "transfer");
    return seconds;
}

// Returns false if the data is wrong.
bool
report(const char *name, double seconds, const char *got, const std::vector<char> &want) {
    const bool ok = memcmp(got, want.data(), want.size()) == 0;
    printf("%-22s %9.1f MB/s  data %s\n",
           name,
           want.size() / seconds / 1e6,
           ok ? "verified" : "MISMATCH");
    return ok;
}

} // namespace

int
main(int argc, char **argv) {
    const size_t len = (argc > 1 ? strtoul(argv[1], nullptr, 10) : 2) << 20;
    const uint64_t card_a = argc > 2 ? strtoul(argv[2], nullptr, 10) : 0;
    const uint64_t card_b = argc > 3 ? strtoul(argv[3], nullptr, 10) : 1;
    const uint64_t bank = argc > 4 ? strtoul(argv[4], nullptr, 10) : 0;
    const uint64_t card_addr = (bank << 32) | scratch_addr_in_bank;

    for (uint64_t card : {card_a, card_b}) {
        if (!std::filesystem::exists("/dev/tenstorrent/" + std::to_string(card))) {
            printf("SKIPPED: no Tenstorrent card %lu\n", card);
            return exit_skipped;
        }
    }

    // The test's own view of the card memory, to save, check and restore it.
    nixlUcxTtWindow window_a, window_b;
    require(window_a.open(card_a, bank), "map card a");
    require(window_b.open(card_b, bank), "map card b");
    char *mem_a = window_a.map + scratch_addr_in_bank;
    char *mem_b = window_b.map + scratch_addr_in_bank;
    const std::vector<char> saved_a(mem_a, mem_a + len), saved_b(mem_b, mem_b + len);

    std::vector<char> pattern(len), readback(len);
    for (size_t i = 0; i < len; i++) {
        pattern[i] = static_cast<char>(i * 2654435761u >> 24);
    }

    nixlAgentConfig config;
    config.useProgThread = true;
    nixlAgent target("target", config), initiator("initiator", config);

    nixl_b_params_t params;
    nixl_mem_list_t mems;
    require(target.getPluginParams("UCX", mems, params) == NIXL_SUCCESS, "UCX plugin");
    nixlBackendH *target_backend, *initiator_backend;
    require(target.createBackend("UCX", params, target_backend) == NIXL_SUCCESS, "backend");
    require(initiator.createBackend("UCX", params, initiator_backend) == NIXL_SUCCESS, "backend");
    nixl_opt_args_t target_args, initiator_args;
    target_args.backends.push_back(target_backend);
    initiator_args.backends.push_back(initiator_backend);

    // The target owns card a. The initiator owns the host buffers and card b.
    const region host_pattern = {DRAM_SEG, (uintptr_t)pattern.data(), 0};
    const region host_readback = {DRAM_SEG, (uintptr_t)readback.data(), 0};
    const region on_card_a = {VRAM_SEG, card_addr, card_a};
    const region on_card_b = {VRAM_SEG, card_addr, card_b};

    nixl_reg_dlist_t target_vram(VRAM_SEG), initiator_vram(VRAM_SEG), initiator_dram(DRAM_SEG);
    target_vram.addDesc(nixlBlobDesc(on_card_a.addr, len, on_card_a.devId, ""));
    initiator_vram.addDesc(nixlBlobDesc(on_card_b.addr, len, on_card_b.devId, ""));
    initiator_dram.addDesc(nixlBlobDesc(host_pattern.addr, len, 0, ""));
    initiator_dram.addDesc(nixlBlobDesc(host_readback.addr, len, 0, ""));
    require(target.registerMem(target_vram, &target_args) == NIXL_SUCCESS, "register card a");
    require(initiator.registerMem(initiator_vram, &initiator_args) == NIXL_SUCCESS,
            "register card b");
    require(initiator.registerMem(initiator_dram, &initiator_args) == NIXL_SUCCESS,
            "register host memory");

    std::string metadata, name;
    require(target.getLocalMD(metadata) == NIXL_SUCCESS, "get target metadata");
    require(initiator.loadRemoteMD(metadata, name) == NIXL_SUCCESS, "load target metadata");

    bool ok = true;
    double seconds;

    seconds = transfer(initiator, initiator_backend, NIXL_WRITE, host_pattern, on_card_a, len);
    ok &= report("host -> card a", seconds, mem_a, pattern);

    seconds = transfer(initiator, initiator_backend, NIXL_READ, host_readback, on_card_a, len);
    ok &= report("card a -> host", seconds, readback.data(), pattern);

    seconds = transfer(initiator, initiator_backend, NIXL_READ, on_card_b, on_card_a, len);
    ok &= report("card a -> card b", seconds, mem_b, pattern);

    initiator.deregisterMem(initiator_dram, &initiator_args);
    initiator.deregisterMem(initiator_vram, &initiator_args);
    target.deregisterMem(target_vram, &target_args);

    memcpy(mem_a, saved_a.data(), len);
    memcpy(mem_b, saved_b.data(), len);
    const bool restored =
        memcmp(mem_a, saved_a.data(), len) == 0 && memcmp(mem_b, saved_b.data(), len) == 0;
    printf("card memory restored: %s\n", restored ? "yes" : "NO");

    return ok && restored ? EXIT_SUCCESS : EXIT_FAILURE;
}
