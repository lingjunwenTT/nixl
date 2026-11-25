# SPDX-FileCopyrightText: Copyright (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Host memory <-> Tenstorrent card memory through the Python API.

Needs a NIXL built with -Dtt_device_memory=true (contrib/tt/build-tt-wheel.sh)
and a Tenstorrent card; skipped without a card. The card memory the test
overwrites is read first and written back at the end.
"""

import os

import numpy as np
import pytest

from nixl._api import nixl_agent, nixl_agent_config

CARD = 0
CARD_ADDR = 0x10000000  # (DRAM bank << 32) | address inside the bank; this is bank 0
LENGTH = 2 << 20

pytestmark = pytest.mark.skipif(
    not os.path.exists(f"/dev/tenstorrent/{CARD}"),
    reason=f"no Tenstorrent card {CARD}",
)


def transfer(agent, operation, host_buffer, remote_agent_name):
    """Move LENGTH bytes between host_buffer and the card memory, and wait for it."""
    local = agent.get_xfer_descs([(host_buffer.ctypes.data, LENGTH, 0)], "DRAM")
    remote = agent.get_xfer_descs([(CARD_ADDR, LENGTH, CARD)], "VRAM")
    handle = agent.initialize_xfer(operation, local, remote, remote_agent_name)

    state = agent.transfer(handle)
    while state == "PROC":
        state = agent.check_xfer_state(handle)
    agent.release_xfer_handle(handle)
    assert state == "DONE"


def test_host_card_round_trip():
    card_agent = nixl_agent("card", nixl_agent_config(backends=["UCX"]))
    host_agent = nixl_agent("host", nixl_agent_config(backends=["UCX"]))

    saved = np.zeros(LENGTH, dtype=np.uint8)
    pattern = np.random.default_rng(0).integers(0, 256, LENGTH, dtype=np.uint8)
    readback = np.zeros(LENGTH, dtype=np.uint8)

    card_agent.register_memory([(CARD_ADDR, LENGTH, CARD, "")], "VRAM")
    host_agent.register_memory(
        [(buf.ctypes.data, LENGTH, 0, "") for buf in (saved, pattern, readback)],
        "DRAM",
    )
    host_agent.add_remote_agent(card_agent.get_agent_metadata())

    transfer(host_agent, "READ", saved, "card")
    try:
        transfer(host_agent, "WRITE", pattern, "card")
        transfer(host_agent, "READ", readback, "card")
        assert np.array_equal(readback, pattern)
    finally:
        transfer(host_agent, "WRITE", saved, "card")
