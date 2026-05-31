/*
 * config.h
 *
 * Z-Core platform hardware addresses and configuration
 *
 * Original ice40 config by Sylvain Munaut
 * Adapted for Z-Core RISC-V SoC on DE10-Lite
 */

#pragma once

#include <stdint.h>

/* ---- Peripheral base addresses (from Z-Core memory map) ---- */
#define UART_BASE       0x04000000
#define GPIO_BASE       0x04001000
#define TIMER_BASE      0x04002000
#define VGA_BASE        0x04003000

/* ---- UART registers ---- */
#define UART_TX         (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_RX         (*(volatile uint32_t *)(UART_BASE + 0x04))
#define UART_STAT       (*(volatile uint32_t *)(UART_BASE + 0x08))
#define UART_CTRL       (*(volatile uint32_t *)(UART_BASE + 0x0C))
#define UART_BAUD_DIV   (*(volatile uint32_t *)(UART_BASE + 0x10))

#define UART_STAT_TX_EMPTY  0x01
#define UART_STAT_TX_BUSY   0x02
#define UART_STAT_RX_VALID  0x04

/* ---- Timer registers (64-bit free-running @ 50 MHz) ---- */
#define TIMER_LO        (*(volatile uint32_t *)(TIMER_BASE + 0x00))
#define TIMER_HI        (*(volatile uint32_t *)(TIMER_BASE + 0x04))
#define TIMER_CTRL_REG  (*(volatile uint32_t *)(TIMER_BASE + 0x08))

/* ---- VGA registers (320x200 FB, RGB332; HW Bresenham-stretched to 640x480) ---- */
#define VGA_FB_ADDR     (*(volatile uint32_t *)(VGA_BASE + 0x00))
#define VGA_FB_DATA     (*(volatile uint32_t *)(VGA_BASE + 0x04))
#define VGA_FB_STATUS   (*(volatile uint32_t *)(VGA_BASE + 0x08))

#define VGA_WIDTH       320
#define VGA_HEIGHT      200

/* ---- WAD in SDRAM (loaded by bootloader) ----
 * Moved up 64 KB (was 0x12000000) so that the stack (top 0x11FF0000,
 * grows down to 0x11FE0000) and the WAD no longer share an address:
 * 0x12000000–0x1200FFFF is now a guard region.
 */
#define WAD_BASE        0x12010000
#define WAD_SIZE        4196020     /* doom1.wad (shareware v1.9) */

/* ---- SDRAM layout ---- */
#define SDRAM_BASE      0x10000000
