# UCX patches for Tenstorrent card memory

Apply to the UCX 1.23.0 release (`patch -p1` in the UCX source folder) before
building UCX. `contrib/tt/build-tt-wheel.sh` does this.

## 0001-tt-dmabuf.patch

An RDMA network card cannot register mapped Tenstorrent card memory by its
address (`ibv_reg_mr` fails). It can register it through a dma-buf, a kernel
handle that lets one device share its memory with another, which the
Tenstorrent driver can export. UCX has no public way for a program to supply a
dma-buf for a memory range, so this patch adds one:

- `ucs_tt_dmabuf_set(va, length, fd)` tells UCX that an address range is backed
  by a dma-buf, and `ucs_tt_dmabuf_unset(va)` removes it.
- UCX's InfiniBand registration looks the range up and, if found, uses
  `ibv_reg_dmabuf_mr` instead of `ibv_reg_mr`.
