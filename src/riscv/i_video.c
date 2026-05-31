/*
 * i_video.c
 *
 * Video system support for Z-Core
 *
 * DOOM renders internally at 320x200 (8-bit indexed palette).
 * Z-Core VGA is 320x200, RGB332; hardware Bresenham-stretches to fill 640x480.
 *
 * I_SetPalette builds a 256-entry RGB332 lookup table.
 * I_FinishUpdate converts each pixel through the palette LUT and writes via
 * the VGA_FB_DATA auto-increment register.
 *
 * Original ice40 version by Sylvain Munaut
 * Adapted for Z-Core RISC-V SoC
 */

#include <stdint.h>
#include <string.h>

#include "doomdef.h"

#include "i_system.h"
#include "v_video.h"
#include "i_video.h"

#include "config.h"
#include "console.h"


/* RGB332 palette lookup table (built by I_SetPalette) */
static uint8_t rgb332_palette[256];

static uint32_t cycle_count = 0;
static uint32_t instret_count = 0;
static uint32_t cycle_count_prev = 0;
static uint32_t instret_count_prev = 0;
static uint32_t data_cache_hits = 0; // CSR mhpmcounter4
static uint32_t data_cache_hits_prev = 0;
static uint32_t load_requests = 0; // CSR mhpmcounter5
static uint32_t load_requests_prev = 0;
static uint32_t instruction_cache_misses = 0; // CSR mhpmcounter9
static uint32_t instruction_cache_misses_prev = 0;
static uint32_t instruction_cache_hits = 0; // CSR mhpmcounter3
static uint32_t instruction_cache_hits_prev = 0;



void
I_InitGraphics(void)
{
	/* Default gamma */
	usegamma = 1;
}

void
I_ShutdownGraphics(void)
{
	/* Nothing to do */
}


void
I_SetPalette(byte* palette)
{
	/*
	 * Convert DOOM's RGB888 palette (with gamma) to RGB332.
	 * RGB332: bits [7:5]=R, [4:2]=G, [1:0]=B
	 */
	for (int i = 0; i < 256; i++) {
		uint8_t r = gammatable[usegamma][*palette++];
		uint8_t g = gammatable[usegamma][*palette++];
		uint8_t b = gammatable[usegamma][*palette++];
		rgb332_palette[i] = (r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6);
	}
}


void
I_UpdateNoBlit(void)
{
}

void
I_FinishUpdate(void)
{
	/* Reset framebuffer write pointer */
	VGA_FB_ADDR = 0;

	/* Convert palette -> RGB332 and blit 320x200 */
	for (int y = 0; y < VGA_HEIGHT; y++) {
		const byte *src_row = screens[0] + y * SCREENWIDTH;
		for (int x = 0; x < VGA_WIDTH; x++)
			VGA_FB_DATA = rgb332_palette[src_row[x]];
	}

	/* FPS: print every 100 frames to UART at a rate that won't flood input */
	{
		static int frame_cnt = 0;
		static int tick_prev = 0;

		if (++frame_cnt == 100) {
			int tick_now = I_GetTime();
			int ticks = tick_now - tick_prev;
			/* FPS = 100 frames * 35 tics/sec / elapsed_tics */
			int fps = (ticks > 0) ? (100 * 35 / ticks) : 99;
			console_printf("[FPS] %d\n", fps);
			tick_prev = tick_now;
			frame_cnt = 0;
			cycle_count_prev = cycle_count;
			instret_count_prev = instret_count;
			data_cache_hits_prev = data_cache_hits;
			load_requests_prev = load_requests;
			instruction_cache_misses_prev = instruction_cache_misses;
			instruction_cache_hits_prev = instruction_cache_hits;
			__asm__ volatile ("csrr %0, mcycle" : "=r" (cycle_count));
			__asm__ volatile ("csrr %0, minstret" : "=r" (instret_count));
			__asm__ volatile ("csrr %0, mhpmcounter4" : "=r" (data_cache_hits));
			__asm__ volatile ("csrr %0, mhpmcounter5" : "=r" (load_requests));
			__asm__ volatile ("csrr %0, mhpmcounter9" : "=r" (instruction_cache_misses));
			__asm__ volatile ("csrr %0, mhpmcounter3" : "=r" (instruction_cache_hits));
			int cycle_diff = cycle_count - cycle_count_prev;
			int instret_diff = instret_count - instret_count_prev;
			int data_cache_hits_diff = data_cache_hits - data_cache_hits_prev;
			int load_requests_diff = load_requests - load_requests_prev;
			int instruction_cache_misses_diff = instruction_cache_misses - instruction_cache_misses_prev;
			int instruction_cache_hits_diff = instruction_cache_hits - instruction_cache_hits_prev;
			console_printf("[Cycle Count] %d\n", cycle_diff);
			console_printf("[Instret Count] %d\n", instret_diff);
			console_printf("[CPI] %d\n", cycle_diff / instret_diff);
			console_printf("[Data Cache Hit Rate] %d\n", data_cache_hits_diff / load_requests_diff);
			console_printf("[Instruction Cache Hit Rate] %d\n", instruction_cache_hits_diff / instruction_cache_misses_diff);
		}
	}
}


void
I_WaitVBL(int count)
{
	/* Busy-wait for vertical blanking (bit 0 of FB_STATUS) */
	while (count-- > 0) {
		while (!(VGA_FB_STATUS & 1))
			;
	}
}


void
I_ReadScreen(byte* scr)
{
	memcpy(
		scr,
		screens[0],
		SCREENHEIGHT * SCREENWIDTH
	);
}
