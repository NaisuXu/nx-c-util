/**
 * @file    nx_can_bus_example.c
 * @brief   Usage examples for the nx_can_bus module.
 *
 * Demonstrates:
 *   1. Building a standard (11-bit ID) CAN frame with data.
 *   2. Building an extended (29-bit ID) CAN frame, setting flags via the union.
 *   3. Parsing a received frame: extracting the ID, checking flags, reading
 *      multi-byte fields from the data payload.
 *
 * The nx_can_msg_t struct uses a flexible array member for data, and a union for
 * flags (access individual flags via flags.bits.* or the whole word via flags.raw).
 * The ID field is a plain uint32_t (11-bit standard uses the low 11 bits; 29-bit
 * extended uses the low 29 bits).
 */
#include "nx_basic_examples.h"
#include "middleware/nx_can_bus.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Example 1: build a standard (11-bit ID) CAN frame                  */
/* ------------------------------------------------------------------ */
static void example_standard_frame(void)
{
    printf("Example 1: build a standard CAN frame (11-bit ID)\n");

    /* Allocate storage: struct + 4-byte payload. */
    uint8_t storage[sizeof(nx_can_msg_t) + 4];
    nx_can_msg_t *msg = (nx_can_msg_t *)storage;
    memset(msg, 0, sizeof(storage));

    msg->id             = 0x123;   /* 11-bit standard ID */
    msg->flags.bits.dlc = 4;       /* 4 bytes of data */
    msg->flags.bits.is_ext    = 0; /* standard (not extended) */
    msg->flags.bits.is_remote = 0; /* data frame (not remote) */
    msg->flags.bits.dir       = NX_CAN_DIR_TX;  /* transmit */

    /* Fill the data payload. */
    msg->data[0] = 0xAA;
    msg->data[1] = 0xBB;
    msg->data[2] = 0xCC;
    msg->data[3] = 0xDD;

    printf("  ID=0x%03X is_ext=%u is_remote=%u DLC=%u DIR=%u\n",
           msg->id, msg->flags.bits.is_ext, msg->flags.bits.is_remote,
           msg->flags.bits.dlc, msg->flags.bits.dir);
    printf("  data: ");
    uint32_t len = nx_can_dlc_to_len(msg->flags.bits.dlc);
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", msg->data[i]);
    }
    printf("\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: build an extended (29-bit ID) CAN frame                 */
/* ------------------------------------------------------------------ */
static void example_extended_frame(void)
{
    printf("Example 2: build an extended CAN frame (29-bit ID)\n");

    /* Allocate storage: struct + 8-byte payload. */
    uint8_t storage[sizeof(nx_can_msg_t) + 8];
    nx_can_msg_t *msg = (nx_can_msg_t *)storage;
    memset(msg, 0, sizeof(storage));

    msg->id = 0x12345678;          /* 29-bit extended ID (uses low 29 bits) */
    msg->flags.bits.dlc       = 8;
    msg->flags.bits.is_ext    = 1; /* extended frame */
    msg->flags.bits.is_remote = 0;
    msg->flags.bits.dir       = NX_CAN_DIR_RX;   /* received */

    /* Fill 8 bytes of payload. */
    for (unsigned i = 0; i < 8; i++) {
        msg->data[i] = (uint8_t)(0x10 + i);
    }

    printf("  ID=0x%08X is_ext=%u is_remote=%u DLC=%u DIR=%u\n",
           msg->id, msg->flags.bits.is_ext, msg->flags.bits.is_remote,
           msg->flags.bits.dlc, msg->flags.bits.dir);
    printf("  data: ");
    uint32_t len = nx_can_dlc_to_len(msg->flags.bits.dlc);
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", msg->data[i]);
    }
    printf("\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: parse a received frame and extract multi-byte fields    */
/* ------------------------------------------------------------------ */
static void example_parse_frame(void)
{
    printf("Example 3: parse a received CAN frame\n");

    /* Simulate a received message: standard ID 0x200, 6 bytes of data. */
    uint8_t storage[sizeof(nx_can_msg_t) + 6];
    nx_can_msg_t *msg = (nx_can_msg_t *)storage;
    memset(msg, 0, sizeof(storage));

    msg->id = 0x200;
    msg->flags.bits.is_ext    = 0;
    msg->flags.bits.is_remote = 0;
    msg->flags.bits.dlc       = 6;
    msg->flags.bits.dir       = NX_CAN_DIR_RX;

    /* Payload contains a 16-bit sensor value (big-endian) at offset 0,
     * and a 32-bit timestamp (little-endian) at offset 2. */
    msg->data[0] = 0x12;   /* sensor high byte */
    msg->data[1] = 0x34;   /* sensor low byte */
    msg->data[2] = 0x78;   /* timestamp byte 0 (LSB in LE) */
    msg->data[3] = 0x56;
    msg->data[4] = 0x34;
    msg->data[5] = 0x12;   /* timestamp byte 3 (MSB in LE) */

    printf("  received: ID=0x%03X is_ext=%u is_remote=%u DLC=%u\n",
           msg->id, msg->flags.bits.is_ext, msg->flags.bits.is_remote,
           msg->flags.bits.dlc);

    /* Decode the sensor value (big-endian 16-bit). */
    uint16_t sensor = ((uint16_t)msg->data[0] << 8) | msg->data[1];
    printf("  sensor value (BE u16 at offset 0) = 0x%04X\n", sensor);

    /* Decode the timestamp (little-endian 32-bit). */
    uint32_t ts = ((uint32_t)msg->data[2])
                | ((uint32_t)msg->data[3] << 8)
                | ((uint32_t)msg->data[4] << 16)
                | ((uint32_t)msg->data[5] << 24);
    printf("  timestamp (LE u32 at offset 2) = 0x%08X\n", ts);
    printf("\n");
}

int nx_can_bus_example_run(void)
{
    printf("########## nx_can_bus examples ##########\n");
    example_standard_frame();
    example_extended_frame();
    example_parse_frame();
    return 0;
}
