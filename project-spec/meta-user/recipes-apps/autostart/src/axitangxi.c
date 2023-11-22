#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axitangxi.h"
#include "axitangxi_ioctl.h"

void *ps_mmap(int fd_dev, size_t size) {
  return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dev, 0);
}

static void init_trans(struct axitangxi_transaction *trans, void *ps_addr,
                       uint32_t pl_addr, uint32_t size) {
  trans->tx_data_size = size;
  trans->rx_data_size = size;
  trans->burst_size = BURST_SIZE;
  trans->burst_data = 16 * BURST_SIZE;
  // ceil(a / b) = floor((a - 1) / b) + 1
  trans->burst_count = (size - 1) / (BURST_SIZE * 16) + 1;
  trans->tx_data_ps_ptr = ps_addr;
  trans->rx_data_pl_ptr = pl_addr;
}

ssize_t pl_io(int fd_dev, void *ps_addr, uint32_t pl_addr, uint32_t size,
              unsigned long request) {
  if (ps_addr == NULL)
    ps_addr = ps_mmap(fd_dev, size);
  if (ps_addr == MAP_FAILED)
    return -1;
  struct axitangxi_transaction trans;
  init_trans(&trans, ps_addr, pl_addr, size);
  if (ioctl(fd_dev, request, &trans) == -1)
    return -1;
  return size;
}

ssize_t pl_write(int fd_dev, void *ps_addr, uint32_t pl_addr, uint32_t size) {
  return pl_io(fd_dev, ps_addr, pl_addr, size, PSDDR_TO_PLDDR);
}

ssize_t pl_read(int fd_dev, void *ps_addr, uint32_t pl_addr, uint32_t size) {
  return pl_io(fd_dev, ps_addr, pl_addr, size, PLDDR_TO_PSDDR);
}

ssize_t ps_read_file(int fd_dev, char *filename, void *addr) {
  int fd = open(filename, O_RDWR);
  if (fd == -1)
    return -1;
  struct stat status;
  if (fstat(fd, &status) == -1)
    return -1;
  addr = ps_mmap(fd_dev, status.st_size);
  if (addr == MAP_FAILED)
    return -1;
  ssize_t size = read(fd, addr, status.st_size);
  if (close(fd) == -1)
    return -1;
  return size;
}

ssize_t pl_config(int fd_dev, char *filename, uint32_t pl_addr,
                  uint32_t *p_size) {
  void *ps_addr = NULL;
  *p_size = ps_read_file(fd_dev, filename, ps_addr);
  if (*p_size == -1)
    return -1;
  return pl_write(fd_dev, ps_addr, pl_addr, *p_size);
}

void pl_run(int fd_dev, struct network_acc_reg *reg) {
  if (ioctl(fd_dev, NETWORK_ACC_CONFIG, &reg) == -1)
    err(errno, AXITX_DEV_PATH);
  if (ioctl(fd_dev, NETWORK_ACC_START) == -1)
    err(errno, AXITX_DEV_PATH);
  if (ioctl(fd_dev, NETWORK_ACC_GET, &reg) == -1)
    err(errno, AXITX_DEV_PATH);
}

ssize_t dump_mem(char *filename, void *ps_addr, size_t size) {
  int fd = open(filename, O_RDWR);
  if (fd == -1)
    return -1;
  ssize_t _size = write(fd, ps_addr, size);
  if (close(fd) == -1)
    return -1;
  return _size;
}