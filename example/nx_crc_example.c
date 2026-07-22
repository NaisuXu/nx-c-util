/**
 * @file    nx_crc_example.c
 * @brief   Usage examples for nx_crc, focused on CRC-16/MODBUS.
 *
 * CRC-16/MODBUS is the frame check used by Modbus RTU. This example shows the
 * three ways to compute it and that they all agree:
 *   1. The named wrapper nx_crc16_modbus().
 *   2. The generic nx_crc16_compute() with the MODBUS parameters spelled out.
 *   3. The incremental API, feeding the data in chunks.
 *
 * ----------------------------------------------------------------------------
 * To verify your own data: change INPUT_BYTES below to the bytes you want to
 * check, then rebuild and run. The program prints the CRC as a 16-bit hex value.
 * Modbus RTU transmits it low byte first (little-endian) on the wire.
 * ----------------------------------------------------------------------------
 */
#include "nx_c_util_examples.h"
#include "nx_crc.h"

#include <stdio.h>

/* >>> Change this to the bytes you want to check. <<< */
static const uint8_t INPUT_BYTES[] = { 0x01, 0x04, 0x02, 0xFF, 0xFF };

int nx_crc_example_run(void)
{
    const uint8_t *data = INPUT_BYTES;
    const size_t   len  = sizeof(INPUT_BYTES);
    int            rc   = 0;

    printf("########## nx_crc examples (CRC-16/MODBUS) ##########\n");
    printf("input =");
    for (size_t i = 0; i < len; i++) {
        printf(" %02X", data[i]);
    }
    printf(" (%zu bytes)\n\n", len);

    /* ---- 1. named wrapper ---- */
    printf("Example 1: named wrapper nx_crc16_modbus()\n");
    uint16_t crc_named = nx_crc16_modbus(data, len);
    printf("  crc = 0x%04X\n\n", crc_named);

    /* ---- 2. generic function with MODBUS parameters ---- */
    printf("Example 2: generic nx_crc16_compute() with MODBUS parameters\n");
    uint16_t crc_generic = nx_crc16_compute(data, len,
                                            0x8005,      /* poly   */
                                            0xFFFF,      /* init   */
                                            true, true,  /* refin, refout */
                                            0x0000);     /* xorout */
    printf("  crc = 0x%04X\n\n", crc_generic);

    /* ---- 3. incremental: feed the data in two chunks ---- */
    printf("Example 3: incremental hash (fed in chunks)\n");
    nx_crc_ctx_t ctx;
    nx_crc_init(&ctx, 16, 0x8005, 0xFFFF, true, true, 0x0000);
    size_t first = len / 2u;
    nx_crc_update(&ctx, data, first);
    nx_crc_update(&ctx, data + first, len - first);
    uint16_t crc_stream = (uint16_t)nx_crc_final(&ctx);
    printf("  crc = 0x%04X\n\n", crc_stream);

    /* ---- 4. all three must agree; show the on-wire byte order ---- */
    printf("Example 4: the three results match\n");
    if (crc_named == crc_generic && crc_named == crc_stream) {
        printf("  match: yes\n");
        printf("  on the wire (Modbus RTU, low byte first): %02X %02X\n",
               crc_named & 0xFFu, (crc_named >> 8) & 0xFFu);
    } else {
        printf("  match: NO (this should never happen)\n");
        rc = 1;
    }
    printf("\n");

    return rc;
}
