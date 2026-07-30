/**
 * @file    nx_modbus_rtu_example.c
 * @brief   Usage examples for the nx_modbus_rtu module.
 *
 * Demonstrates:
 *   1. Building a "read holding registers" request (function code 0x03), setting
 *      the CRC, and verifying it.
 *   2. Parsing a successful response with multi-byte register values (big-endian).
 *   3. Building an exception response (error reply).
 *
 * The module provides frame structs (all uint8_t fields, no padding) and CRC
 * helpers (table-driven CRC-16/MODBUS, self-contained). Multi-byte fields (u16)
 * are stored in network (big-endian) order on the wire; use nx_modbus_rtu_get_u16 /
 * nx_modbus_rtu_set_u16 to read/write them portably.
 */
#include "nx_basic_examples.h"
#include "middleware/nx_modbus_rtu.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Example 1: build a "read holding registers" request (FC 0x03)      */
/* ------------------------------------------------------------------ */
static void example_read_holding_registers_request(void)
{
    printf("Example 1: build read holding registers request (FC 0x03)\n");

    /* Request to slave 0x11: read 3 registers starting at address 0x0064. */
    nx_modbus_rtu_req_fix_t req;
    req.addr = 0x11;
    req.cmd  = NX_MODBUS_FC_READ_HOLDING_REGS;
    nx_modbus_rtu_set_u16(0x0064, &req.addr_h, &req.addr_l);   /* starting address */
    nx_modbus_rtu_set_u16(3, &req.qty_h, &req.qty_l);          /* quantity of registers */

    /* Compute and append the CRC (the last 2 bytes). */
    size_t req_len = sizeof(req);
    bool ok = nx_modbus_rtu_set_crc((uint8_t *)&req, req_len);
    printf("  slave=0x%02X fc=0x%02X start=0x%04X count=%u\n",
           req.addr, req.cmd,
           nx_modbus_rtu_get_u16(req.addr_h, req.addr_l),
           nx_modbus_rtu_get_u16(req.qty_h, req.qty_l));
    printf("  CRC set: %s, frame (%zu bytes): ", ok ? "OK" : "FAIL", req_len);
    for (size_t i = 0; i < req_len; i++) {
        printf("%02X ", ((uint8_t *)&req)[i]);
    }
    printf("\n");

    /* Verify the CRC we just wrote. */
    bool valid = nx_modbus_rtu_check_crc((uint8_t *)&req, req_len);
    printf("  CRC check: %s\n\n", valid ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
/* Example 2: parse a successful response with register values         */
/* ------------------------------------------------------------------ */
static void example_parse_response(void)
{
    printf("Example 2: parse a successful read holding registers response\n");

    /* Simulate a response from the slave: 3 registers (6 bytes of data). */
    uint8_t frame_buf[32];
    nx_modbus_rtu_rsp_var_t *rsp = (nx_modbus_rtu_rsp_var_t *)frame_buf;

    rsp->addr       = 0x11;
    rsp->cmd        = NX_MODBUS_FC_READ_HOLDING_REGS;
    rsp->byte_count = 6;   /* 3 registers * 2 bytes each */

    /* Fill the payload with register values (big-endian on the wire). */
    nx_modbus_rtu_set_u16(0x1234, &rsp->payload[0], &rsp->payload[1]);
    nx_modbus_rtu_set_u16(0x5678, &rsp->payload[2], &rsp->payload[3]);
    nx_modbus_rtu_set_u16(0xABCD, &rsp->payload[4], &rsp->payload[5]);

    /* Locate the CRC field (2 bytes after the payload) and set it. */
    uint8_t *crc_pos = nx_modbus_rtu_rsp_var_crc(rsp);
    size_t frame_len = (size_t)(crc_pos - frame_buf) + 2;   /* header + byte_count + payload + 2-byte CRC */
    nx_modbus_rtu_set_crc(frame_buf, frame_len);

    /* Now parse it back. */
    printf("  received frame (%zu bytes): ", frame_len);
    for (size_t i = 0; i < frame_len; i++) {
        printf("%02X ", frame_buf[i]);
    }
    printf("\n");

    bool crc_ok = nx_modbus_rtu_check_crc(frame_buf, frame_len);
    printf("  CRC check: %s\n", crc_ok ? "PASS" : "FAIL");

    if (!nx_modbus_rtu_is_exception(rsp->cmd)) {
        printf("  slave=0x%02X fc=0x%02X byte_count=%u\n",
               rsp->addr, rsp->cmd, rsp->byte_count);
        printf("  register values (big-endian decoded):\n");
        for (uint8_t i = 0; i < rsp->byte_count / 2; i++) {
            uint16_t val = nx_modbus_rtu_get_u16(rsp->payload[i * 2], rsp->payload[i * 2 + 1]);
            printf("    reg[%u] = 0x%04X\n", i, val);
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: build an exception response (error reply)                */
/* ------------------------------------------------------------------ */
static void example_exception_response(void)
{
    printf("Example 3: build an exception response (error reply)\n");

    /* Slave 0x11 replies with "illegal data address" for FC 0x03. */
    nx_modbus_rtu_rsp_exc_t exc;
    exc.addr = 0x11;
    exc.cmd  = NX_MODBUS_FC_READ_HOLDING_REGS | NX_MODBUS_RTU_EXCEPTION_FLAG;
    exc.exception_code = NX_MODBUS_EXC_ILLEGAL_DATA_ADDR;

    size_t exc_len = sizeof(exc);
    nx_modbus_rtu_set_crc((uint8_t *)&exc, exc_len);

    printf("  exception frame (%zu bytes): ", exc_len);
    for (size_t i = 0; i < exc_len; i++) {
        printf("%02X ", ((uint8_t *)&exc)[i]);
    }
    printf("\n");

    printf("  slave=0x%02X fc=0x%02X (exception bit set) code=0x%02X (%s)\n",
           exc.addr, exc.cmd & ~NX_MODBUS_RTU_EXCEPTION_FLAG, exc.exception_code,
           (exc.exception_code == NX_MODBUS_EXC_ILLEGAL_DATA_ADDR) ? "illegal data address" : "other");
    printf("  is_exception: %s\n\n", nx_modbus_rtu_is_exception(exc.cmd) ? "yes" : "no");
}

int nx_modbus_rtu_example_run(void)
{
    printf("########## nx_modbus_rtu examples ##########\n");
    example_read_holding_registers_request();
    example_parse_response();
    example_exception_response();
    return 0;
}
