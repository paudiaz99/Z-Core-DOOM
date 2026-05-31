/*
 * start.S
 *
 * Z-Core startup code for DOOM
 *
 * The bootloader has already loaded code+data into SDRAM,
 * so there is no ROM-to-RAM copy needed.
 *
 * Debug: immediately writes "Z>" to UART before any C code runs.
 * If you see "Z>" on the serial console, the CPU successfully
 * fetched and executed instructions from SDRAM.
 */

	.equ UART_BASE, 0x04000000
	.equ UART_TX,   0x00
	.equ UART_STAT, 0x08

	.section .text.start
	.global _start
_start:

	// ============================================================
	// Layer 1 debug: raw UART write, no stack, no C runtime
	// If you see "Z>" the CPU is executing from SDRAM.
	// ============================================================
	li   t0, UART_BASE

	// Send 'Z'
1:	lw   t1, UART_STAT(t0)
	andi t1, t1, 1            // bit 0 = TX_EMPTY
	beqz t1, 1b
	li   t1, 'Z'
	sw   t1, UART_TX(t0)

	// Send '>'
2:	lw   t1, UART_STAT(t0)
	andi t1, t1, 1
	beqz t1, 2b
	li   t1, '>'
	sw   t1, UART_TX(t0)

	// Send '\r' '\n'
3:	lw   t1, UART_STAT(t0)
	andi t1, t1, 1
	beqz t1, 3b
	li   t1, '\r'
	sw   t1, UART_TX(t0)
4:	lw   t1, UART_STAT(t0)
	andi t1, t1, 1
	beqz t1, 4b
	li   t1, '\n'
	sw   t1, UART_TX(t0)

	// ============================================================
	// Normal startup
	// ============================================================

	// Set stack pointer
	la sp, __stacktop

	// Install trap handler so exceptions print info instead of
	// silently jumping to 0x0 (bootloader).
	la   t0, _trap_handler
	csrw mtvec, t0

	// Clear .bss section
	la a0, _sbss
	la a1, _ebss
	bge a0, a1, end_init_bss
loop_init_bss:
	sw zero, 0(a0)
	addi a0, a0, 4
	blt a0, a1, loop_init_bss
end_init_bss:

	// call main
	call main

.global	_exit
_exit:
	j _exit


// ================================================================
// Trap handler — prints mcause / mepc / mtval via raw UART writes.
//
// Completely self-contained: no call/ret, no .rodata strings,
// CSRs read immediately so they are captured at the fault point.
// Uses a0-a5 and t0 freely (we hang at the end, no restore needed).
// Output format: "\nTRAP C=XXXXXXXX E=YYYYYYYY V=ZZZZZZZZ\n"
// ================================================================

	.section .text
	.global _trap_handler
	.balign 4
_trap_handler:
	// Save ra FIRST, before anything clobbers it
	mv   a2, ra             // a2 = ra (return address at fault)

	li   a0, UART_BASE      // a0 = UART base throughout

	// Capture CSRs — before any other activity
	csrr a3, mcause         // a3 = mcause
	csrr a4, mepc           // a4 = mepc
	csrr a5, mtval          // a5 = mtval

	// Stash ra into a saved register (a2 is overwritten by hex printer)
	// We'll use t1 to hold ra across the print loop — save it now
	mv   t1, a2             // t1 = ra (preserved across loops)

	// ---- Print '\r' '\n' ----
_th_nl1:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_nl1
	li   t0, '\r'
	sw   t0, UART_TX(a0)
_th_nl1b:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_nl1b
	li   t0, '\n'
	sw   t0, UART_TX(a0)

	// ---- Print 'T' ----
_th_T:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_T
	li   t0, 'T'
	sw   t0, UART_TX(a0)

	// ---- Print 'R' ----
_th_R:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_R
	li   t0, 'R'
	sw   t0, UART_TX(a0)

	// ---- Print 'A' ----
_th_A:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_A
	li   t0, 'A'
	sw   t0, UART_TX(a0)

	// ---- Print 'P' ----
_th_P:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_P
	li   t0, 'P'
	sw   t0, UART_TX(a0)

	// ---- Print ' ' ----
_th_sp1:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_sp1
	li   t0, ' '
	sw   t0, UART_TX(a0)

	// ---- Print 'C' ----
_th_C:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_C
	li   t0, 'C'
	sw   t0, UART_TX(a0)

	// ---- Print '=' ----
_th_eq1:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_eq1
	li   t0, '='
	sw   t0, UART_TX(a0)

	// ---- Print a3 (mcause) as 8 hex digits, MSB first ----
	// a1 = digit counter (8..1), a2 = digit char, t0 = scratch
	li   a1, 8
_th_mc_loop:
	addi a2, a1, -1
	slli a2, a2, 2          // shift = (a1-1)*4
	srl  a2, a3, a2
	andi a2, a2, 0xF
	addi a2, a2, '0'
	li   t0, '9'
	ble  a2, t0, _th_mc_digit
	addi a2, a2, 7          // 'A'-'0'-10 = 7
_th_mc_digit:
_th_mc_tx:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_mc_tx
	sw   a2, UART_TX(a0)
	addi a1, a1, -1
	bnez a1, _th_mc_loop

	// ---- Print ' E=' ----
_th_sp2:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_sp2
	li   t0, ' '
	sw   t0, UART_TX(a0)
_th_E:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_E
	li   t0, 'E'
	sw   t0, UART_TX(a0)
_th_eq2:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_eq2
	li   t0, '='
	sw   t0, UART_TX(a0)

	// ---- Print a4 (mepc) as 8 hex digits ----
	li   a1, 8
_th_ep_loop:
	addi a2, a1, -1
	slli a2, a2, 2
	srl  a2, a4, a2
	andi a2, a2, 0xF
	addi a2, a2, '0'
	li   t0, '9'
	ble  a2, t0, _th_ep_digit
	addi a2, a2, 7
_th_ep_digit:
_th_ep_tx:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_ep_tx
	sw   a2, UART_TX(a0)
	addi a1, a1, -1
	bnez a1, _th_ep_loop

	// ---- Print ' V=' ----
_th_sp3:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_sp3
	li   t0, ' '
	sw   t0, UART_TX(a0)
_th_V:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_V
	li   t0, 'V'
	sw   t0, UART_TX(a0)
_th_eq3:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_eq3
	li   t0, '='
	sw   t0, UART_TX(a0)

	// ---- Print a5 (mtval) as 8 hex digits ----
	li   a1, 8
_th_mt_loop:
	addi a2, a1, -1
	slli a2, a2, 2
	srl  a2, a5, a2
	andi a2, a2, 0xF
	addi a2, a2, '0'
	li   t0, '9'
	ble  a2, t0, _th_mt_digit
	addi a2, a2, 7
_th_mt_digit:
_th_mt_tx:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_mt_tx
	sw   a2, UART_TX(a0)
	addi a1, a1, -1
	bnez a1, _th_mt_loop

	// ---- Print ' R=' ----
_th_sp4:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_sp4
	li   t0, ' '
	sw   t0, UART_TX(a0)
_th_Ra:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_Ra
	li   t0, 'R'
	sw   t0, UART_TX(a0)
_th_eq4:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_eq4
	li   t0, '='
	sw   t0, UART_TX(a0)

	// ---- Print t1 (ra) as 8 hex digits ----
	li   a1, 8
_th_ra_loop:
	addi a2, a1, -1
	slli a2, a2, 2
	srl  a2, t1, a2
	andi a2, a2, 0xF
	addi a2, a2, '0'
	li   t0, '9'
	ble  a2, t0, _th_ra_digit
	addi a2, a2, 7
_th_ra_digit:
_th_ra_tx:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_ra_tx
	sw   a2, UART_TX(a0)
	addi a1, a1, -1
	bnez a1, _th_ra_loop

	// ---- Print '\r' '\n' and hang ----
_th_nl2:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_nl2
	li   t0, '\r'
	sw   t0, UART_TX(a0)
_th_nl2b:
	lw   t0, UART_STAT(a0)
	andi t0, t0, 1
	beqz t0, _th_nl2b
	li   t0, '\n'
	sw   t0, UART_TX(a0)

_trap_hang:
	j    _trap_hang
