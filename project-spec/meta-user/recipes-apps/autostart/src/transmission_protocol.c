#include <endian.h>
#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <syslog.h>
#include <unistd.h>

#include "crc.h"
#include "transmission_protocol.h"

const uint8_t tp_header[] = {0x3A, 0x62, 0x04, 0x3F};

char *bin_to_str(uint8_t const *bin, size_t size) {
  char *str = malloc(size * 3 + 1);
  char *p = str;
  for (int i = 0; i < size; i++) {
    sprintf(p, "%02x ", bin[i]);
    p += 3;
  }
  return str;
}

n_frame_t id_to_n_frame(n_frame_t id, n_frame_t len) {
  if (len == 1)
    return id;
  // count n_frame from 1 not 0
  return id++;
}

n_frame_t n_frame_to_id(n_frame_t n_frame, n_frame_t len) {
  if (len == 1)
    return n_frame;
  // count id from 0 not 1
  return n_frame--;
}

ssize_t send_frame(int fd, frame_t *frame, int timeout) {
  frame->check_sum = crc16((uint8_t *)frame, sizeof(*frame) - sizeof(uint16_t));

  struct epoll_event event;
  int num = epoll_wait(fd, &event, 1, timeout);
  if (num < 1)
    return -1;

  frame->n_file = htole32(frame->n_file);
  frame->n_frame = htole16(frame->n_frame);
  frame->status = htole16(frame->status);
  return write(event.data.fd, frame, sizeof(*frame));
}

ssize_t send_data_frame(int fd, data_frame_t *frame, int timeout) {
  frame->check_sum = crc16((uint8_t *)frame, sizeof(*frame) - sizeof(uint16_t));

  struct epoll_event event;
  int num = epoll_wait(fd, &event, 1, timeout);
  if (num < 1)
    return -1;

  frame->n_total_frame.uint24 = htole32(frame->n_total_frame.uint24) >> 8;
  frame->n_file = htole32(frame->n_file);
  frame->n_frame = htole16(frame->n_frame);
  frame->total_data_len = htole32(frame->total_data_len);
  frame->data_len = htole16(frame->data_len);
  return write(event.data.fd, frame, sizeof(*frame));
}

/**
 * Slave waits master's request. If not received, just wait forever.
 * However, master sends a request and waits slave's response. If not received,
 * it shouldn't wait forever but resend a request. So a timeout is necessary.
 *
 * https://stackoverflow.com/questions/32537792/why-i-only-get-one-character-at-one-time-when-i-use-read
 */
ssize_t receive_frame(int fd, frame_t *frame, int timeout) {
  __typeof__(frame) temp = malloc(sizeof(*frame));

  struct epoll_event event;
  int num = epoll_wait(fd, &event, 1, timeout);
  if (num < 1)
    return -1;
  ssize_t n = read(event.data.fd, temp, sizeof(*frame));
  char *str = bin_to_str((uint8_t *)temp, sizeof(*frame));
  if (n < sizeof(*frame) ||
      crc16((uint8_t *)temp, sizeof(*frame) - sizeof(uint16_t)) !=
          temp->check_sum) {
    syslog(LOG_INFO, "receive incorrectly: %s", str);
    free(str);
    free(temp);
    return -1;
  }
  syslog(LOG_INFO, "receive correctly: %s", str);
  free(str);
  memcpy(frame, temp, sizeof(*frame));
  free(temp);

  frame->n_file = le32toh(frame->n_file);
  frame->n_frame = le16toh(frame->n_frame);
  frame->status = le16toh(frame->status);
  return n;
}

ssize_t receive_data_frame(int fd, data_frame_t *frame, int timeout) {
  __typeof__(frame) temp = malloc(sizeof(*frame));

  struct epoll_event event;
  int num = epoll_wait(fd, &event, 1, timeout);
  if (num < 1)
    return -1;
  ssize_t n = read(event.data.fd, temp, sizeof(*frame));
  char *str = bin_to_str((uint8_t *)temp, sizeof(*frame));
  if (n < sizeof(*frame) ||
      crc16((uint8_t *)temp, sizeof(*frame) - sizeof(uint16_t)) !=
          temp->check_sum) {
    syslog(LOG_INFO, "receive incorrectly: %s", str);
    free(str);
    free(temp);
    return -1;
  }
  syslog(LOG_INFO, "receive correctly: %s", str);
  free(str);
  memcpy(frame, temp, sizeof(*frame));
  free(temp);

  frame->n_total_frame.uint24 = le32toh(frame->n_total_frame.uint24) >> 8;
  frame->n_file = le32toh(frame->n_file);
  frame->n_frame = le16toh(frame->n_frame);
  frame->total_data_len = le32toh(frame->total_data_len);
  frame->data_len = le16toh(frame->data_len);
  return n;
}

size_t data_frame_to_data_len(data_frame_t *data_frames, n_frame_t n_frame) {
  size_t len = 0;
  for (int i = 0; i < n_frame; i++)
    len += data_frames[i].data_len;
  return len;
};

void data_frames_to_data(data_frame_t *data_frames, n_frame_t n_frame,
                         uint8_t *addr) {
  uint8_t *p = addr;
  for (int i = 0; i < n_frame; i++) {
    memcpy(p, data_frames[i].data, data_frames[i].data_len);
    p += data_frames[i].data_len;
  }
};

void data_to_data_frames(uint8_t *addr, size_t len, data_frame_t *data_frames) {
  n_frame_t i = 0;
  if (addr == NULL) {
    ssize_t size;
    while (true) {
      size = read(len, data_frames[i].data, TP_FRAME_DATA_LEN_MAX);
      if (size != TP_FRAME_DATA_LEN_MAX)
        break;
      data_frames[i].data_len = TP_FRAME_DATA_LEN_MAX;
      data_frames[i].check_sum = crc16((uint8_t *)&data_frames[i],
                                       sizeof(data_frame_t) - sizeof(uint16_t));
      i++;
    }
    if (size == -1) {
      perror(NULL);
      return;
    }
    data_frames[i].data_len = size;
    memset(data_frames[i].data + size, 0, TP_FRAME_DATA_LEN_MAX - size);
    data_frames[i].check_sum = crc16((uint8_t *)&data_frames[i],
                                     sizeof(data_frame_t) - sizeof(uint16_t));
    return;
  }
  uint8_t *p = addr;
  size_t n_frame = (len - 1) / TP_FRAME_DATA_LEN_MAX + 1;
  for (; i < n_frame - 1; i++) {
    memcpy(data_frames[i].data, p, TP_FRAME_DATA_LEN_MAX);
    data_frames[i].data_len = TP_FRAME_DATA_LEN_MAX;
    data_frames[i].check_sum = crc16((uint8_t *)&data_frames[i],
                                     sizeof(data_frame_t) - sizeof(uint16_t));
    p += TP_FRAME_DATA_LEN_MAX;
  }
  data_frames[i].data_len = len - i * TP_FRAME_DATA_LEN_MAX;
  memcpy(data_frames[i].data, p, data_frames[i].data_len);
  memset(data_frames[i].data + data_frames[i].data_len, 0,
         TP_FRAME_DATA_LEN_MAX - data_frames[i].data_len);
  data_frames[i].check_sum = crc16((uint8_t *)&data_frames[i],
                                   sizeof(data_frame_t) - sizeof(uint16_t));
};

ssize_t data_to_yuv420(uint8_t *y, uint8_t **u, uint8_t **v, size_t yuv_len) {
  size_t v_len = yuv_len / (4 + 1 + 1);
  *u = y + v_len * 4;
  *v = *u + v_len;
  return v_len;
};

/**
 * dequantize:
 *  output = input * scale
 *  bias = 0
 */
void entropy_to_gmm(uint16_t *entropy_addr, gmm_t *gmm, size_t len) {
  // TODO: 9 channels in 13 subbands use same scale
#define LL 0.00940390583127737
#define HL0 0.00012030187644995749
#define HL1 0.0006082353065721691
#define HL2 0.0009562921477481723
#define HL3 0.001880464842543006
#define LH0 0.00022100130445323884
#define LH1 0.00040055729914456606
#define LH2 0.0015174609143286943
#define LH3 0.002205430995672941
#define HH0 0.0002125250466633588
#define HH1 0.0003605725651141256
#define HH2 0.000580432009883225
#define HH3 0.0031505702063441277
  double scale = 1.0;
  for (gmm_t *p = gmm; gmm + len - p >= 0; p++) {
    p->mean1 = entropy_addr[1] * scale;
    p->mean2 = entropy_addr[2] * scale;
    p->mean3 = entropy_addr[3] * scale;
    p->std1 = entropy_addr[4] * scale;
    p->std2 = entropy_addr[5] * scale;
    p->std3 = entropy_addr[6] * scale;
    p->prob1 = entropy_addr[7] * scale;
    p->prob2 = entropy_addr[8] * scale;
    p->prob3 = entropy_addr[9] * scale;
  }
}

/**
 * data, data_len and check_sum should be set in data_to_data_frames()
 */
void init_data_frames(data_frame_t *data_frames, n_frame_t n_frame,
                      n_file_t n_file, flag_t flag) {
  for (int i = 0; i < n_frame; i++) {
    memcpy(data_frames[i].header, tp_header, sizeof(tp_header));
    data_frames[i].n_file = n_file;
    data_frames[i].n_frame = id_to_n_frame(i, n_frame);
    data_frames[i].flag = flag;
  }
}

/**
 * if addr is NULL, len is fd
 */
data_frame_t *alloc_data_frames(n_frame_t n_frame, n_file_t n_file,
                                uint8_t *addr, size_t len, flag_t flag) {
  data_frame_t *data_frames = malloc(n_frame * sizeof(data_frame_t));
  if (data_frames == NULL)
    return NULL;
  init_data_frames(data_frames, n_frame, n_file, flag);
  data_to_data_frames(addr, len, data_frames);
  return data_frames;
}
