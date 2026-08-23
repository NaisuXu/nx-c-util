# UDS Design Notes

**Status:** Design phase; not yet implemented.

**Date:** 2026-08-20

## Context

This document records the design direction for a UDS (ISO 14229) module family in `nx-c-util`. The design serves two transport paths (CAN via `nx_can_isotp`, Modbus RTU via a future `nx_modbus_rtu_tp`), supports both client (tester) and server (ECU) roles, and must accommodate all of ISO 14229-1 as an extensible framework—not just flashing.

## Core Principle

**UDS is a pure upper-layer protocol and must not know anything about the link under it.**

This principle, applied rigorously through adversarial review, eliminates the originally proposed `nx_tp_port_t` seam struct entirely. What UDS genuinely needs is already satisfied by:

1. Fields already on `nx_tp_sdu_t` (payload, `ta_type`, `link`, `result`)—these are genuine ISO 14229-2 A_PDU service interface parameters.
2. Two application-chosen capacity numbers in the UDS server configuration.
3. Transport mechanics (pool, queues, ref-msg protocol) confined to per-path binding shims below the UDS layer.

## Architecture

### Layers

```
Application
    |
    | indicate(bytes, len, ta_type, link)
    | output_callback(user, link, rsp, len, ta_type)
    v
nx_uds_server / nx_uds_client
    |
    | (no direct transport coupling)
    |
    v
Binding shim (per path, ~30 lines each)
  - nx_uds_port_isotp.c
  - nx_uds_port_modbus.c
    |
    | nx_tp_sdu_t via nx_queue_t of nx_ref_msg_t*
    v
nx_can_isotp / nx_modbus_rtu_tp
```

The binding shim owns:
- Pulling `nx_tp_sdu_t` from the transport's TX queue
- Calling `nx_uds_server_indicate()` or `nx_uds_client_confirm()`
- Allocating from pool + publishing responses to the transport's RX queue
- All ref-msg and queue mechanics

The UDS layer sees only:
- `indicate()` calls carrying request bytes + addressing + link identity
- An output callback to emit responses
- `confirm()` calls carrying transmission outcomes

### Modules

| Module | Type | Purpose |
|--------|------|---------|
| `nx_tp_sdu.h` | Header-only (structs) | Transport SDU: payload + `ta_type` + `link` + `kind` + `result` + `len` |
| `nx_uds.h` | Header-only | Vocabulary: SIDs, NRCs, session masks, `nx_uds_xfer_t` state |
| `nx_uds_server.{h,c}` | Core | Table-driven server: service dispatch, P2/P2*/P4 timers, 0x78 pump, NRC ownership |
| `nx_uds_transfer.{h,c}` | Service family | 0x34/0x35/0x36/0x37 handlers (deliberately not "download"—0x38 shares 0x36/0x37) |
| `nx_modbus_rtu_tp.{h,c}` | Transport | Modbus carrier: ADU ↔ `nx_tp_sdu_t`, polled master-slave, confirms at collect |
| `nx_uds_client.{h,c}` | Core | Transactor: one-transaction engine, P2 from confirm, 0x78 absorption, no table |
| `nx_uds_port_isotp.c` | Shim (example) | Wires `nx_can_isotp` ↔ `nx_uds_server` |
| `nx_uds_port_modbus.c` | Shim (example) | Wires `nx_modbus_rtu_tp` ↔ `nx_uds_server` |

Client and server share header-only vocabulary but zero compiled artifacts.

### Service Extension Point

The service set is **data, not code**: a caller-held `const nx_uds_service_t[]` table with no privileged core. The library's own services (0x10 DiagnosticSessionControl, 0x11 ECUReset, 0x3E TesterPresent, 0x27 SecurityAccess) are just exported function pointers occupying rows.

```c
typedef struct {
    uint8_t  sid;               /**< Service identifier */
    uint8_t  flags;             /**< NX_UDS_SVC_* flags */
    uint8_t  sec_level;         /**< Minimum unlocked security level (0 = no restriction) */
    const uint8_t *subs;        /**< Allowed sub-functions (NULL = any) */
    uint8_t  subs_count;        /**< Length of subs array */
    uint8_t  session_mask;      /**< Bitmask of sessions where this service is available */
    uint16_t min_len;           /**< Minimum request length (SID inclusive) */
    uint16_t max_len;           /**< Maximum request length (0 = no limit) */
    uint32_t p4_us;             /**< Per-service P4 cap (0 = use global cfg.p4_us) */
    nx_uds_handler_fn handler;  /**< Service handler */
    void    *user;              /**< Opaque user pointer passed to handler */
} nx_uds_service_t;
```

One handler signature for all services:

```c
typedef nx_uds_disposition_t (*nx_uds_handler_fn)(nx_uds_ctx_t *ctx, void *user);
```

Phases: `REQUEST` (first call), `RESUME` (handler returned `PENDING` last time), `RESPONSE` (response about to transmit), `CONFIRM` (transmission completed), `LINK_ERROR` (transport reported failure), `SILENCE` (functionally addressed request will not be answered per ISO 14229-1 suppression), `ABORT` (transaction cancelled: S3 expiry, new request arrived mid-PENDING).

Dispositions: `DONE`, `PENDING` (layer re-enters next `process()`, emits NRC 0x78 on its own timer), `DEFER` (optional; see open question 2), `NEGATIVE` (handler wrote NRC to `ctx->out`), `NO_RESPONSE`.

**Adding a service** (e.g. 0x19 ReadDTCInformation):
1. Write the handler function: `nx_uds_disposition_t nx_uds_handle_read_dtc(nx_uds_ctx_t *ctx, void *user)`.
2. Export it in a header if it will be reused: `extern nx_uds_handler_fn nx_uds_svc_read_dtc;`.
3. Add one row to the application's service table with `sid=0x19`, appropriate flags/masks/lengths, and `handler=nx_uds_handle_read_dtc`.
4. No file under `src/middleware/` is opened.

### Capacity Numbers (relocated from the port struct)

UDS needs to know two upper-layer quantities to implement three ISO 14229-1 behaviors:

1. **0x34 RequestDownload / 0x35 RequestUpload** publish `maxNumberOfBlockLength` in the positive response—a number the server COMPUTES and the client obeys when sizing 0x36 blocks. There is no "build it and see if it fails"; the server must announce up front.
2. **NRC 0x14 responseTooLong** is the defined answer when a response exceeds what can be conveyed. On a small link (the owner's Modbus collect window), "let the link fail" yields a timeout, not the correct observable 0x14.
3. **0x22 ReadDataByIdentifier with many DIDs / 0x19 enumeration**: the server must stop enumerating at capacity or emit 0x14, requiring the number before the SDU is finished.

These are **application-chosen** capacities, not raw link MTUs. The server's receive capacity for 0x34 is `min(buffer, flash block constraint, link MTU)`; if UDS read only the link MTU it would publish a `maxNumberOfBlockLength` larger than its own buffer.

```c
typedef struct {
    /* ... other cfg fields ... */

    uint32_t max_req_apdu;   /**< Largest request A_PDU this server accepts, in bytes.
                                  Derived by the application from min(its own receive
                                  buffer, flash block constraint, transport rx limit).
                                  The 0x34 RequestDownload handler computes
                                  maxNumberOfBlockLength from this less SID overhead. */
    uint32_t max_resp_apdu;  /**< Largest response A_PDU this server produces, in bytes.
                                  Derived from min(its own transmit buffer, transport
                                  tx limit). Clamps ctx->out_cap; triggers NRC 0x14
                                  responseTooLong on overflow; feeds the 0x35
                                  RequestUpload maxNumberOfBlockLength. */

    uint32_t p2_us;          /**< P2_server: time from request to first response or
                                  pending notification, in microseconds */
    uint32_t p2_star_us;     /**< P2*_server: time from one 0x78 to the next or to
                                  the final response, in microseconds */
    uint32_t p4_us;          /**< P4_server: hard cap on the whole transaction from
                                  indication to final response inclusive of every 0x78
                                  extension, in microseconds */

    /* ... service table, get_us, output callback, etc. ... */
} nx_uds_server_cfg_t;
```

**Two numbers, not one:** the owner's Modbus path is asymmetric—the deposit window (what the server receives on 0x36) and the collect window (what it can hand back on 0x35) differ. `max_req_apdu` and `max_resp_apdu` feed different UDS mechanisms and must be independent.

**Wiring discipline:** the application computes one constant from `min(buffer, segment, link)` and wires it into both `nx_can_isotp_cfg.rx_max_len` (for the transport's own overflow check) and `nx_uds_server_cfg.max_req_apdu`. If they drift, the server publishes a `maxNumberOfBlockLength` the transport will then refuse—manual discipline replaces the compile-time guarantee the port struct would have given. Accepted cost for correct layering.

### Transport Contract (documented requirement, not a struct)

A carrier serving UDS:

- **Publishes one `NX_TP_SDU_CONFIRM` per message** accepted from `tx_queue`, carrying `result` (`NX_TP_N_OK` or a defined failure), never omitted. A carrier that cannot observe transmission completing confirms at acceptance; the confirmation is not optional.
- **Publishes `NX_TP_SDU_INDICATION`** for each complete message received, with `ta_type` naming how it was addressed (physical vs functional).
- **Refuses messages beyond its capacity** with `result != NX_TP_N_OK` on the confirm (e.g. `NX_TP_N_BUFFER_OVFLW`), never silently truncates.
- **On functional transmission:** either honors it (single-frame on a 1:n ID) or refuses at publish/confirm time; never transmits it on the wrong ID.

`nx_can_isotp` satisfies this when `confirm_tx = true` and `func_tx_id` is set (or left zero if functional TX is not needed). A Modbus carrier satisfies it by confirming when the master reads the last chunk.

### Behavioral Consequences of Eliminating the Port Struct

1. **0x78 pump runs unconditionally** on every server instance. When `P2` elapses with no answer ready, the layer emits one pending notification, offers it to the link until accepted (coalescing via "at most one pending notification outstanding per transaction"), then restarts the `P2*` window. No conditional logic for polled vs free-running carriers; the same code is correct on both. Queue back-pressure naturally throttles the pump on a slow drain.

2. **P4 is an unconditional transaction cap** applying to every link. Its expiry NRC is a per-service choice (table row field), defaulting to 0x21 `busyRepeatRequest`, not a hardcoded 0x22.

3. **Client P2 always starts at the confirm**. The confirm-wait is capped by a client-side timeout so a missing confirm degrades to a named timeout instead of a hang.

4. **0x11 ECUReset response-then-reset ordering** moves entirely into the handler. Every handler receives a `CONFIRM` phase (delivered after the response transmits); the 0x11 handler arms a caller-supplied actuator there, which carries its own settle delay. The server never special-cases the SID.

5. **Functional-addressing response suppression** (drop NRC 0x11/0x12/0x31/0x7E/0x7F on functionally addressed requests) reads `sdu->ta_type`, never a config bit. The transport stamps `ta_type` based on the received address; UDS applies ISO 14229-1 suppression rules unconditionally.

6. **Multi-link demux** (when one application wants one shared `rx_queue` serving multiple transports) is 8 lines of application code dispatching on `sdu->link`. Each UDS instance keeps independent session/security state. The alternative (`serve_any_link = true` in one server) is a security defect: unlocking over one path unlocks the other.

### NRC Ownership Split

- **Core** (the layer emits these itself): 0x11 `serviceNotSupported`, 0x12 `sub-functionNotSupported`, 0x7E `sub-functionNotSupportedInActiveSession`, 0x13 `incorrectMessageLengthOrInvalidFormat`, 0x14 `responseTooLong`, 0x21 `busyRepeatRequest` (default on P4 expiry), 0x33 `securityAccessDenied`, 0x7F `serviceNotSupportedInActiveSession`, 0x78 `requestCorrectlyReceived-ResponsePending`, and 0x22 `conditionsNotCorrect` on P4 expiry (if the service table row specifies it).
- **Handler** (all others, including 0x22/0x24/0x31/0x35/0x36/0x72 in their domain-specific uses).

### Binding Shim Example (CAN path, illustrative)

```c
/**
 * @file    nx_uds_port_isotp.c
 * @brief   Wires nx_can_isotp to nx_uds_server.
 *
 * One shim per transport path, written once. ~30 lines on the CAN path because
 * nx_can_isotp already speaks the ref-msg + queue idiom.
 */

void app_uds_pump_can(nx_uds_server_t *srv, nx_can_isotp_t *iso)
{
    nx_ref_msg_t *m;
    while (nx_queue_pop(iso->cfg.sdu_tx_queue, &m) == NX_QUEUE_OK) {
        const nx_tp_sdu_t *s = (const nx_tp_sdu_t *)nx_ref_msg_data(m);
        if (s->kind == NX_TP_SDU_INDICATION) {
            nx_uds_server_indicate(srv, s->data, s->len, s->ta_type, s->link);
        } else {  /* NX_TP_SDU_CONFIRM */
            nx_uds_server_confirm(srv, s->link, s->result);
        }
        nx_ref_msg_release(m);
    }
    nx_uds_server_process(srv);   /* drives 0x78 pump, calls out_fn when ready */
}

/* The server's output callback, wired at init, allocs + publishes: */
void app_uds_out_can(void *user, uint8_t link, const uint8_t *rsp,
                     uint32_t len, nx_tp_ta_type_t ta_type)
{
    app_ctx_t *app = (app_ctx_t *)user;
    nx_can_isotp_t *iso = app->iso_can;   /* app knows which link is which */
    
    nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool,
                                       sizeof(nx_tp_sdu_t) + len);
    if (m == NULL) { return; }   /* drop on alloc failure */
    
    nx_tp_sdu_t *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
    s->len     = len;
    s->link    = link;
    s->kind    = NX_TP_SDU_INDICATION;   /* request going down */
    s->ta_type = ta_type;
    s->result  = NX_TP_N_OK;
    memcpy(s->data, rsp, len);
    
    nx_ref_msg_publish(m, iso->cfg.sdu_rx_queue);
    nx_ref_msg_release(m);   /* producer reference */
}
```

The Modbus shim is similar length; the Modbus transport queues SDUs the same way ISO-TP does.

## Open Questions (genuine UDS-layer forks)

These are the only three design forks that survived adversarial review. Each is a UDS-layer question (not a transport or product question), cannot be settled by thinking harder, and leads to materially different headers or state.

### 1. Session and Security Scope

When one product terminates two independent UDS server instances (CAN path and Modbus path), what is the scope of the active diagnostic session and the unlocked security level?

**Options:**

- **Per instance** (default): Each `nx_uds_server_t` owns `session`, `sec_level`, `s3_deadline` in its `.run` struct. Unlocking over one path does not unlock the other; two flashing conversations can be open simultaneously and the UDS layer does not know it. If the product must prevent concurrent flash, the interlock belongs in the flash driver (the resource owner), not in UDS.

- **Shared ECU state**: A caller-owned `nx_uds_diag_state_t` that both instances' cfgs point at. Both see one session and one security level. Requires rules for whose S3 expiry drops the shared session, what the second instance answers while the first holds a programming session (NRC 0x22 / 0x21 / proceed), and whether an ownership field is needed.

- **Split**: Session and S3 per instance (property of a conversation); security level shared via pointer (property of the ECU). Unlocking over CAN unlocks Modbus; sessions stay independent.

**Recommendation:** Per instance. ISO 14229-1 is written for one connection and gives no rule for two; the standard's own session/security/S3 definitions are stated for a single server. The shared arrangement creates a security surface (one tester's unlock reaches another's path) and forces three arbitration rules into the layer that the per-instance design never needs.

### 2. DEFER Disposition

Does the server keep the `DEFER` disposition (a handler takes the request and answers later through a standalone `nx_uds_reply(ticket, ...)` with the business module never holding the server handle), or does it ship `PENDING`-only (handler is re-invoked each `process()` until it returns `DONE`)?

**Options:**

- **PENDING-only**: No ticket type, no generation counter, no second response-emission path. A handler polls its own completion (`st->flash_busy()` in the `RESUME` phase) and returns `DONE` when ready. One embedded transaction slot, one response emitter (`process()`), and the handler contract's `RESUME` phase carries all continuation. Additive later if a real async completer appears.

- **DEFER**: Define `nx_uds_ticket_t` (with generation counter), caller-provided transaction slot storage addressable by ticket, a standalone `nx_uds_reply()` that publishes the late answer without the server handle, and a response-emission model (likely a sink) that lets late replies and the 0x78 pump fire between `indicate()` calls. Justified when the thing that finishes a UDS operation is decoupled in time/context from the server's main loop (RTOS task, ISR completing a flash write).

**Recommendation:** PENDING-only for the first version. `RESUME` already lets a handler poll a business module across cycles (`st->flash_busy()` is the whole implementation), so `DEFER` buys not "slow answers" but specifically "answers from a module that will not be polled"—a narrower need. The ticket machinery is surface that only a genuinely asynchronous completer justifies. Reassess when a service cannot be written under `PENDING`; the addition is purely additive and breaks no existing handler.

### 3. Client Architecture

Is the client driven by the same `const` service table as the server (table-driven and symmetric: `nx_uds_service_t` grows response-side fields, and `nx_uds_client` takes the same table the server does), or does it stay a transactor (no table: `nx_uds_client_request(c, req, len, ta_type)` submits opaque bytes, the layer runs P2 from the transmit confirm, absorbs 0x78, and hands back the final response bytes plus NRC if negative; the caller parses)?

**Options:**

- **Table-driven and symmetric**: `nx_uds_service_t` grows expected response-length bounds, a response handler with phases, etc. One row describes a service for both roles. The client becomes a validator.

- **Transactor (no table)**: The client's entire state is one transaction, one deadline, one submitted-message reference. It runs the P2/P2*/0x78 state machine (the genuinely hard part) and the transmit-failure case (`result != NX_TP_N_OK`), and hands back bytes. The caller interprets. Optional: a small set of exported request builders / response parsers for the services the library already ships rows for, sitting beside the client as helpers rather than inside it.

**Recommendation:** Transactor. A table-driven client has to encode response formats to be worth anything, and encoding the response formats of ISO 14229-1 is exactly the closed-set commitment the whole data-driven design exists to avoid—it would put the library back in the business of knowing services, on the client side, having just gotten out of it on the server side. Keep the client to what is genuinely generic and genuinely hard: confirm-anchored P2, 0x78/P2* absorption, transmit-failure case, functional-request multi-responder case. The caller parses what it expects. Add optional helpers (row companions) later if the shipped rows want them.

## Implementation Increments

1. **`nx_tp_sdu.h` unchanged** (already shipped).
2. **`nx_uds.h`** — header-only vocabulary: SID/NRC enums, session masks, `nx_uds_xfer_t`.
3. **`nx_uds_server.{h,c}`** — core server with assert fixture including a **hostile port** (confirms late, drops messages, emits garbage SDUs) proving the layer never trusts the transport.
4. **CAN path runs** — `nx_can_isotp` unchanged, one binding shim example in `examples/`, owner's CAN flashing works.
5. **`nx_uds_transfer.{h,c}`** — 0x34/0x35/0x36/0x37 handlers.
6. **`nx_modbus_rtu_tp.{h,c}`** — Modbus carrier (ADU ↔ `nx_tp_sdu_t`).
7. **`nx_uds_client.{h,c}`** — transactor.
8. **`nx_modbus_rtu_master.{h,c}`** — optional (if the owner's Modbus path currently has no master and needs one).

The Modbus carrier blocks nothing; CAN flashing is working at increment 4.

## Key Traps

These are load-bearing correctness constraints surfaced during design review:

1. **The `- 2` for `maxNumberOfBlockLength`** (subtracting SID + blockSequenceCounter from `max_req_apdu`) must exist in exactly one place. Legacy code has four copies; the fourth is already inconsistent.

2. **S3 restarts on accepting an indication**, not on assembling a response. A slow handler cannot let S3 expire.

3. **`nx_uds_server_process()` must accept exactly one indication per call**, not drain the queue. Unlike `nx_can_isotp_process()` (which correctly drains), the UDS server holds one transaction at a time, and accepting a second request aborts the first.

4. **Separate pools per path**. One shared pool serving two transports is a budget-exhaustion attack: a flood on the CAN path starves the Modbus path of message slots.

5. **No `nx_lock` hook in the server**. The layer is single-threaded (one `process()` per instance); concurrency is the application's job. Document the `nx_queue` SPSC caveat: both sides read-modify-write the shared count, so an ISR producer preempting a main-loop consumer can lose an update. On bare metal, wrap `indicate()` in a critical section if the transport ISR-driven.

6. **`blockSequenceCounter` wraps 0xFF → 0x00**, not 0xFF → 0x01. The counter has a period of 256; `0x01` is special only as a fresh transfer's first value, not as a wrap target. (Behaviour confirmed against multiple independent implementations; the clause citation previously given here as "Table 407" could not be verified and has been removed rather than carried into code.)

7. **No `ctx->srv` back-pointer**. The handler receives `ctx` (the transaction state) and `user` (from the table row), never the server handle. This keeps the transaction self-contained and the handler testable without a full server instance.

8. **Zero-copy request, caller-buffer response**. The handler reads `ctx->req` (points into the received SDU, valid until `DONE`/`NEGATIVE`/`DEFER`) and writes into `ctx->out` (a fixed caller-supplied buffer in the transaction slot, sized by `ctx->out_cap` which is pre-clamped to `min(buffer size, cfg.max_resp_apdu)`). No intermediate copy; no dynamic allocation.

## References

- ISO 14229-1:2020 — Unified diagnostic services (UDS) — Part 1: Application layer
- ISO 14229-2:2013 — Part 2: Session layer services
- ISO 14229-3:2012 — Part 3: UDSonCAN
- ISO 15765-2:2016 — Road vehicles — Diagnostic communication over Controller Area Network (DoCAN) — Part 2: Transport protocol and network layer services

## Revision History

- 2026-08-20: Initial design notes. Adversarial review eliminated `nx_tp_port_t`; capacity numbers relocated to `nx_uds_server_cfg_t`; transport mechanics confined to binding shims. Three open questions remain (session/security scope, DEFER, client architecture).
