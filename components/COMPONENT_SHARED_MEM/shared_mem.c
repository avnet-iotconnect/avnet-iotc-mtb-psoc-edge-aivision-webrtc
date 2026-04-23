#include "shared_mem.h"
#include "cybsp.h"
#include <stdio.h>
#include <stdint.h>

#define SRAM1_SHARED_DATA_ADDR     (CYMEM_CM33_0_m55_allocatable_shared_START + CYMEM_CM33_0_m55_allocatable_shared_SIZE - (sizeof(oob_shared_data_t)))


void shared_mem_write(oob_shared_data_t *cfg)
{
    char *src = (char *)cfg;
    char *dst = (char *)SRAM1_SHARED_DATA_ADDR;

    memcpy(dst, src, sizeof(oob_shared_data_t));
}

void shared_mem_read(oob_shared_data_t *cfg)
{
    char *src = (char *)SRAM1_SHARED_DATA_ADDR;
    char *dst = (char *)cfg;

    memcpy(dst, src, sizeof(oob_shared_data_t));
}
