// clang -Wall -ggdb3 -O0 -pedantic -Wundef -Wextra -fno-inline
// -fno-omit-frame-pointer
// -fsanitize=address,undefined,signed-integer-overflow,alignment -lpthread -lm
// -ffunction-sections -Wwrite-strings

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define KB_TO_B_UNIT (1024)
#define MB_TO_KB_UNIT (1024)
#define MB_TO_B_UNIT (MB_TO_KB_UNIT * KB_TO_B_UNIT)
#define KB_TO_B(_x) ((_x) * KB_TO_B_UNIT)
#define MB_TO_B(_x) ((_x) * MB_TO_B_UNIT)

#define SECTOR_SZ (256UL)
#define PAGE_SZ (1UL << 12)
#define SECTORS_PER_PAGE (PAGE_SZ / SECTOR_SZ)

#define SZ_START (0)
#define SZ_END (MB_TO_B(10))
#define SEED_SIZE (3)

#define N_PAGES_MEM(_x) ((_x) * PAGE_SZ)
#define TEST_PAGES (32)

#undef DEBUG

#define FREE_MEM(_z) do { free((_z)); } while(0)
#define ALLOC_MEM(_z, _type_ptr, _size )                                       \
  ({                                                                           \
    size_t _s = (_size);                                                       \
    _type_ptr _t = malloc(_s);                                                 \
    assert(_t);                                                                \
    memset(_t, '\0', _s);                                                      \
    _t;                                                                        \
  })

// #define RAND_FILL(_x, _sz)                                                     \
//   do {                                                                         \
//     size_t _l;                                                                 \
//     char *_y = _x;                                                             \
//     for (_l = 0; _l < _sz; _l++) {                                             \
//       _y[_l] = (char)(drand48() * (UINT8_MAX + 1));                            \
//       printf("%x\n", _y[_l]);                                                  \
//       assert(0);                                                               \
//     }                                                                          \
//   } while (0)

typedef union seedval {
  time_t tseed;
  uint16_t u48seed[SEED_SIZE];
} seedval_t;

typedef struct page_sector_def {
  size_t current_page;
  size_t current_sector;
} page_sector_def_t;

typedef struct memdef {
  void *start;
  void *current;
  size_t size;
  size_t read_bytes;
  size_t remain_bytes;
  page_sector_def_t ps_def;
} memdef_t;

typedef struct rd_memchunk {
  char buf[SECTOR_SZ];
  size_t offset;
  size_t avail_bytes;
} memchunk_t;

void init_page_sector_def(page_sector_def_t *sp) {
  assert(sp);
  sp->current_page = 0;
  sp->current_sector = 0;
}

void reset_memdef(memdef_t *mp) {
  mp->read_bytes = 0;
  mp->remain_bytes = mp->size;
}

void randfill_memdef(memdef_t *mp) {
  unsigned char *dst = mp->start;
  for (size_t s = 0; s < mp->size; s++) {
    dst[s] = floor(drand48() * (UINT8_MAX));
  }
}

void init_memdef(memdef_t **mpp, size_t sz) {
  void *mem = NULL;
  memdef_t *mp = NULL;
  assert(mpp && sz);
  mp = ALLOC_MEM(mp, typeof(mp), sizeof(*mp));
  mem = ALLOC_MEM(mem, char *, sz);
  mp->start = mem;
  mp->size = sz;
  reset_memdef(mp);
  init_page_sector_def(&(mp->ps_def));

#if defined(DEBUG)
  printf("%s: start :%p limit: %p\n", __func__, mp->start,
         (char *)mp->start + sz);
#endif
  randfill_memdef(mp);
  *mpp = mp;
}

void free_memdef(memdef_t **mpp) {
  memdef_t *mp = *mpp;
  FREE_MEM(mp->start);
  FREE_MEM(mp);
  *mpp = (memdef_t *)NULL;
}

void increment_sector_page_idx(memdef_t *md) {
  md->ps_def.current_sector++;
  if (md->ps_def.current_sector == SECTORS_PER_PAGE) {
    md->ps_def.current_sector = 0;
    md->ps_def.current_page++;
  }

#if defined(DEBUG)
  printf("Now Page id: %zu sector id: %zu\n", md->ps_def.current_page,
         md->ps_def.current_sector);
#endif
}

int read_sector_page(void *buf, size_t page_id, size_t sector_id,
                     memdef_t *md) {
  assert(buf);
  assert(sector_id < SECTORS_PER_PAGE);

  if (page_id == TEST_PAGES) {
#if defined(DEBUG)
    printf("Discarding read beyond page !!!!\n");
#endif
    return -1;
  }
  void *src =
      ((char *)md->start) + (PAGE_SZ * page_id) + (SECTOR_SZ * sector_id);
#if defined(DEBUG)
  printf("Reading page: %zu sector id: %zu\n", page_id, sector_id);
#endif
  memcpy(buf, src, SECTOR_SZ);
  increment_sector_page_idx(md);
  return 0;
}

int read_bytes(void *buf, size_t sz, memdef_t *md) {
  static memchunk_t m;
  char *dst = buf;
  size_t remainder_sector_bytes, n_sectors;
  remainder_sector_bytes = n_sectors = 0;
  assert(buf);
#if defined(DEBUG)
  printf("%s: buf :%p sz: %zu\n", __func__, buf, sz);
#endif
  if (m.avail_bytes) {
    size_t min = m.avail_bytes < sz ? m.avail_bytes : sz;
    memcpy(dst, &m.buf[m.offset], min);
    sz -= min;
    dst += min;
#if defined(DEBUG)
    printf("%s: read %zu out of avail_bytes :%zu \n", __func__, min,
           m.avail_bytes);
#endif
    m.offset += min;
    m.avail_bytes -= min;
  }

  n_sectors = sz / SECTOR_SZ;
  remainder_sector_bytes = sz - n_sectors * SECTOR_SZ;

  for (size_t i = 0; i < n_sectors; i++) {
    if (read_sector_page(dst, md->ps_def.current_page,
                         md->ps_def.current_sector, md) == -1) {
      return -1;
    }
    dst += SECTOR_SZ;
  }

  if (remainder_sector_bytes) {
    assert(remainder_sector_bytes < SECTOR_SZ);

    if (read_sector_page(m.buf, md->ps_def.current_page,
                         md->ps_def.current_sector, md) == -1) {
      return -1;
    }

    memcpy(dst, m.buf, remainder_sector_bytes);
    m.avail_bytes = SECTOR_SZ - remainder_sector_bytes;
    m.offset = remainder_sector_bytes;
  }

  return 0;
}

void cmp(char *a, char *b, size_t sz) {
  size_t i = 0;
  for (i = 0; i < sz; i++) {
    assert(a[i] == b[i]);
  }
#if defined(DEBUG)
  printf("Pass: %zu\n", sz);
#endif
}

int main() {

  memdef_t *md = NULL;
  seedval_t s = {0};
  size_t rd_bytes;

  time(&s.tseed);
  seed48(s.u48seed);
  printf("Seed: %zu\n", s.tseed);
  rd_bytes = 0;

  for (size_t stride = 1; stride <= N_PAGES_MEM(TEST_PAGES); stride++) {
    init_memdef(&md, N_PAGES_MEM(TEST_PAGES));
    char *dst = ALLOC_MEM(dst, typeof(dst) , N_PAGES_MEM(TEST_PAGES));
    char *src = (char *)md->start;
    size_t n_strides = 0;
    size_t n_discards = 0;
    for (rd_bytes = 0; rd_bytes < N_PAGES_MEM(TEST_PAGES); rd_bytes += stride) {
      src = (char *)md->start + rd_bytes;
#if defined (DEBUG)
      printf("%s: src :%p start :%p dst :%p\n", __func__, src, md->start, dst);
#endif
      if (!read_bytes(dst, stride, md)) {
        cmp(dst, src, stride);
        n_strides++;
      } else {
        n_discards++;
      }
    }
    free_memdef(&md);
    FREE_MEM(dst);
    printf("Pass for stride_size %zu nstrides: %zu discarded reads: %zu\n", stride, n_strides, n_discards);
  }
  return 0;
}
