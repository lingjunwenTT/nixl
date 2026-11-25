# NIXL for Tenstorrent cards

This branch lets NIXL transfer data directly to and from the memory on a
Tenstorrent Blackhole card, through the UCX backend. It is NIXL 1.5.0 plus a
small change to the UCX plugin and a patch to UCX itself.

## How card memory is described

Card memory is registered as NIXL's existing device-memory type, `VRAM_SEG`
(`"VRAM"` in Python). A descriptor is:

| Field | Meaning |
|---|---|
| `devId` | The card: `N` of `/dev/tenstorrent/N` |
| `addr` | `(DRAM bank << 32) \| address inside the bank`. A card has 8 banks, 0 to 7 |
| `len` | Bytes. The range must end below `0xFF000000` inside its bank |

Each registration maps one whole bank. A card allows 8 such mappings at a time,
shared by every process using the card.

## Build

    contrib/tt/build-tt-wheel.sh

This downloads UCX 1.23.0, applies `contrib/tt/ucx-patches`, builds it, builds
NIXL against it with `-Dtt_device_memory=true`, and writes two Python packages
to `dist/`:

- `nixl_tt`: NIXL with the patched UCX inside it.
- `nixl`: a small package that makes `import nixl` load `nixl_tt`.

Set `PYTHON` to the Python version to build for, for example `PYTHON=3.12`.
Install both:

    pip install dist/nixl_tt-*.whl dist/nixl-*.whl

## Run

Nothing Tenstorrent-specific is needed over TCP. For RDMA, pick the network
card with UCX's usual variables, for example:

    UCX_TLS=rc UCX_NET_DEVICES=bnxt_re0:1

## Test

Both tests overwrite a 2 MiB scratch range at address `0x10000000` of bank 0
and put the old contents back. Make sure nothing else is using the cards.

- Python, from the installed packages: `pytest test/python/test_tt_vram.py`
- C++, from a NIXL build tree configured with `-Dtt_device_memory=true`:
  `ucx_tt_vram_test [MiB] [card_a] [card_b] [bank]`

## Limits

- A build with `tt_device_memory` treats every `VRAM_SEG` registration in the
  UCX plugin as Tenstorrent card memory, so it cannot also serve GPUs.
- It cannot exchange UCX transfers with an unmodified NIXL: the memory
  metadata the two sides exchange carries one extra field.
- Verified on card firmware 19.16. Each bank is reached through a fixed
  endpoint (`dramEndpoint` in `src/plugins/ucx/tt/tt_mem.h`), the one tt-metal
  uses on this firmware. Firmware that relocates the DRAM endpoints needs a
  different table.
- Tested with both agents on one machine. Transfers between two machines have
  not been tried.
- `import nixl` picks `nixl_tt` ahead of other NIXL builds installed in the
  same environment, except when a CUDA build of PyTorch is installed there.
