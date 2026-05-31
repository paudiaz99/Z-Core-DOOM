/*
 * i_system.c
 *
 * System support code for Z-Core
 *
 * Copyright (C) 1993-1996 by id Software, Inc.
 * Copyright (C) 2021 Sylvain Munaut
 * Adapted for Z-Core RISC-V SoC
 */


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"

#include "d_main.h"
#include "g_game.h"
#include "m_misc.h"
#include "i_sound.h"
#include "i_video.h"

#include "i_system.h"

#include "console.h"
#include "config.h"


/* 50 MHz / 35 Hz = 1,428,571 timer ticks per DOOM tic */
#define TICKS_PER_TIC  1428571


void
I_Init(void)
{
	/* Enable timer: bit 0 = enable, bit 1 = auto-reload */
	TIMER_CTRL_REG = 0x03;
}


byte *
I_ZoneBase(int *size)
{
	/* Give 12 MB to DOOM — plenty for shareware textures/sprites.
	 * Heap starts ~0x100E0000, zone ends ~0x10CE0000, stack at 0x10FFC000. */
	*size = 12 * 1024 * 1024;
	return (byte *) malloc (*size);
}


/*
 * I_GetTime — returns DOOM game tics (35 Hz).
 *
 * Uses subtraction-based accumulator to avoid 64-bit division
 * (libgcc's __udivdi3 contains compressed instructions that
 * Z-Core cannot execute).  The while-loop runs 0 or 1 times
 * per call at normal frame rates.
 */
static uint32_t timer_last = 0;
static uint32_t timer_accum = 0;
static int      doom_tics = 0;

int
I_GetTime(void)
{
	uint32_t now = TIMER_LO;
	timer_accum += (now - timer_last);   /* handles 32-bit wrap */
	timer_last = now;

	while (timer_accum >= TICKS_PER_TIC) {
		timer_accum -= TICKS_PER_TIC;
		doom_tics++;
	}

	return doom_tics;
}


static void
I_GetRemoteEvent(void)
{
	event_t event;

	const char map[] = {
		KEY_LEFTARROW,  // 0
		KEY_RIGHTARROW, // 1
		KEY_DOWNARROW,  // 2
		KEY_UPARROW,    // 3
		KEY_RSHIFT,     // 4
		KEY_RCTRL,      // 5
		KEY_RALT,       // 6
		KEY_ESCAPE,     // 7
		KEY_ENTER,      // 8
		KEY_TAB,        // 9
		KEY_BACKSPACE,  // 10
		KEY_PAUSE,      // 11
		KEY_EQUALS,     // 12
		KEY_MINUS,      // 13
		KEY_F1,         // 14
		KEY_F2,         // 15
		KEY_F3,         // 16
		KEY_F4,         // 17
		KEY_F5,         // 18
		KEY_F6,         // 19
		KEY_F7,         // 20
		KEY_F8,         // 21
		KEY_F9,         // 22
		KEY_F10,        // 23
		KEY_F11,        // 24
		KEY_F12,        // 25
	};

	static byte s_btn = 0;

	boolean mupd = false;
	int mdx = 0;
	int mdy = 0;

	while (1) {
		int ch = console_getchar_nowait();
		if (ch == -1)
			break;

		boolean msb = ch & 0x80;
		ch &= 0x7f;

		if (ch < 28) {
			/* Keyboard special */
			event.type = msb ? ev_keydown : ev_keyup;
			event.data1 = map[ch];
			D_PostEvent(&event);
		} else if (ch < 31) {
			/* Mouse buttons */
			if (msb)
				s_btn |= (1 << ((ch & 0x7f) - 28));
			else
				s_btn &= ~(1 << ((ch & 0x7f) - 28));
			mupd = true;
		} else if (ch == 0x1f) {
			/* Mouse movement */
			signed char x = console_getchar();
			signed char y = console_getchar();
			mdx += x;
			mdy += y;
			mupd = true;
		} else {
			/* Keyboard normal */
			event.type = msb ? ev_keydown : ev_keyup;
			event.data1 = ch;
			D_PostEvent(&event);
		}
	}

	if (mupd) {
		event.type = ev_mouse;
		event.data1 = s_btn;
		event.data2 =   mdx << 2;
		event.data3 = - mdy << 2;	/* Doom is sort of inverted ... */
		D_PostEvent(&event);
	}
}

void
I_StartFrame(void)
{
	/* Nothing to do */
}

void
I_StartTic(void)
{
	I_GetRemoteEvent();
}

ticcmd_t *
I_BaseTiccmd(void)
{
	static ticcmd_t emptycmd;
	return &emptycmd;
}


void
I_Quit(void)
{
	D_QuitNetGame();
	M_SaveDefaults();
	I_ShutdownGraphics();
	exit(0);
}


byte *
I_AllocLow(int length)
{
	byte*	mem;
	mem = (byte *)malloc (length);
	memset (mem,0,length);
	return mem;
}


void
I_Tactile
( int on,
  int off,
  int total )
{
	// UNUSED.
	on = off = total = 0;
}


void
I_Error(char *error, ...)
{
	va_list	argptr;

	// Message first.
	va_start (argptr,error);
	fprintf (stderr, "Error: ");
	vfprintf (stderr,error,argptr);
	fprintf (stderr, "\n");
	va_end (argptr);

	fflush( stderr );

	// Shutdown. Here might be other errors.
	if (demorecording)
		G_CheckDemoStatus();

	D_QuitNetGame ();
	I_ShutdownGraphics();

	exit(-1);
}
