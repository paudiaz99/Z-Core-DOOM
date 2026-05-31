/*
 * i_main.c
 *
 * Main entry point for DOOM on Z-Core
 * Includes pre-flight peripheral checks and WAD validation.
 */

#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "d_main.h"

#include "config.h"
#include "console.h"


/* ================================================================
 * Pre-flight peripheral diagnostics
 *
 * Tests each peripheral DOOM depends on before handing off to the
 * game engine. Any failure is printed but does NOT halt — the user
 * can see exactly which subsystem is broken.
 * ================================================================ */

static int pf_errors;

static void pf_pass(const char *name)
{
	console_printf("  [OK]   %s\n", name);
}

static void pf_fail(const char *name, const char *detail)
{
	console_printf("  [FAIL] %s: %s\n", name, detail);
	pf_errors++;
}


/* --- UART ---
 * If we got this far, UART TX works (console_init already ran).
 * Test RX readiness bit to make sure STATUS register is readable. */
static void pf_uart(void)
{
	uint32_t stat = UART_STAT;
	/* TX_EMPTY should be set (we just finished printing) */
	/* Just verify the register reads something sane (not 0xFFFFFFFF / 0x00000000 bus error) */
	if (stat == 0xFFFFFFFF || stat == 0xDEADDEAD) {
		pf_fail("UART", "STATUS reads bus error");
		return;
	}
	if (!(stat & UART_STAT_TX_EMPTY)) {
		pf_fail("UART", "TX not empty after init");
		return;
	}
	pf_pass("UART");
}


/* --- Timer ---
 * Read twice with a small gap; verify it incremented. */
static void pf_timer(void)
{
	/* Enable timer first */
	TIMER_CTRL_REG = 0x03;

	uint32_t t0 = TIMER_LO;

	/* Small busy-wait (~1000 cycles) */
	for (volatile int i = 0; i < 200; i++)
		;

	uint32_t t1 = TIMER_LO;

	if (t1 == t0) {
		pf_fail("Timer", "counter did not increment");
		console_printf("         TIMER_LO = 0x%08x (both reads)\n", t0);
		return;
	}
	if (t0 == 0xFFFFFFFF || t1 == 0xFFFFFFFF) {
		pf_fail("Timer", "reads bus error (0xFFFFFFFF)");
		return;
	}

	uint32_t delta = t1 - t0;
	console_printf("  [OK]   Timer (delta=%u ticks)\n", delta);
}


/* --- VGA ---
 * Write and read back FB_ADDR register; check STATUS is readable. */
static void pf_vga(void)
{
	uint32_t status = VGA_FB_STATUS;
	if (status == 0xFFFFFFFF) {
		pf_fail("VGA", "STATUS reads bus error");
		return;
	}

	/* Write a known address, read it back */
	VGA_FB_ADDR = 42;
	uint32_t readback = VGA_FB_ADDR;
	if (readback != 42) {
		console_printf("  [WARN] VGA FB_ADDR: wrote 42, read %u\n", readback);
		/* Not fatal — some VGA implementations don't support readback */
	}

	pf_pass("VGA");
}


/* --- GPIO ---
 * Just verify the register is accessible (not bus error). */
static void pf_gpio(void)
{
	volatile uint32_t *gpio_dir  = (volatile uint32_t *)(GPIO_BASE + 0x04);
	uint32_t val = *gpio_dir;
	if (val == 0xFFFFFFFF && val == 0xDEADDEAD) {
		pf_fail("GPIO", "direction reg reads bus error");
		return;
	}
	pf_pass("GPIO");
}


/* --- SDRAM read-back ---
 * Read a few words from the loaded binary itself and verify they
 * match expected values. This tests that SDRAM reads work for
 * instruction fetches AND data loads. */
static void pf_sdram(void)
{
	/* Read the first word of our own code — should be a valid RV32 instruction.
	 * _start is at 0x10000000. The first instruction is "li t0, 0x04000000"
	 * which encodes as LUI t0, 0x04000. Let's just check it's not zero or FF. */
	volatile uint32_t *code_base = (volatile uint32_t *)SDRAM_BASE;
	uint32_t first_word = code_base[0];

	if (first_word == 0x00000000) {
		pf_fail("SDRAM", "first word is 0 (code not loaded?)");
		return;
	}
	if (first_word == 0xFFFFFFFF) {
		pf_fail("SDRAM", "first word is 0xFFFFFFFF (bus error?)");
		return;
	}

	/* Also read from further into the binary to check multiple SDRAM rows */
	uint32_t mid_word = code_base[0x1000];  /* offset 0x4000 */
	if (mid_word == 0x00000000 && code_base[0x1001] == 0x00000000) {
		pf_fail("SDRAM", "mid-region reads all zeros");
		return;
	}

	console_printf("  [OK]   SDRAM (code[0]=0x%08x)\n", first_word);
}


/* ================================================================
 * WAD Validation
 *
 * Checks:
 *   1. "IWAD" magic at WAD_BASE
 *   2. Header fields (numLumps, dirOffset) are sane
 *   3. 32-bit additive checksum of the entire WAD
 *      Expected: 0x1683486D for doom1.wad shareware v1.9
 * ================================================================ */

#define WAD_EXPECTED_MAGIC      0x44415749  /* "IWAD" little-endian */
#define WAD_EXPECTED_NUMLUMPS   1264
#define WAD_EXPECTED_DIROFS     0x003FB7B4
#define WAD_EXPECTED_BYTESUM    0x1683486D  /* sum of all bytes, truncated to 32 bits */

static void pf_wad(void)
{
	volatile uint32_t *wad = (volatile uint32_t *)WAD_BASE;

	/* Check magic only — full checksum already verified by bootloader. */
	uint32_t magic = wad[0];
	if (magic != WAD_EXPECTED_MAGIC) {
		pf_fail("WAD", "bad magic");
		console_printf("         got 0x%08x, expected 0x%08x\n",
			magic, WAD_EXPECTED_MAGIC);
		return;
	}

	uint32_t numLumps = wad[1];
	if (numLumps != WAD_EXPECTED_NUMLUMPS)
		console_printf("  [WARN] WAD numLumps=%u (expected %u)\n",
			numLumps, WAD_EXPECTED_NUMLUMPS);

	console_printf("  [OK]   WAD (%u lumps)\n", numLumps);
}


/* ================================================================
 * SDRAM read-consistency probes
 *
 * Diagnose whether DOOM's symptoms (guard misfire on valid pointers,
 * `numlumps` comparing wrong, garbled texture names) are caused by
 * SDRAM reads occasionally returning stale/wrong data when running
 * with mixed I-fetch + D-load traffic — a pattern the bootloader
 * BIST cannot exercise (BIST runs from BRAM).
 *
 * Test A: read the same global N times, expect identical values.
 *         A failure means SDRAM read inconsistency on .data.
 *
 * Test B: write to stack, immediately read back N times, expect
 *         identical values. A failure means stack write-then-read
 *         (write-buffer drain / forwarding) bug.
 * ================================================================ */

static volatile uint32_t g_canary = 0xDEADBEEF;

static void test_global_consistency(void)
{
	uint32_t errs = 0;
	const uint32_t N = 1000000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t a = g_canary;
		uint32_t b = g_canary;
		uint32_t c = g_canary;
		if (a != 0xDEADBEEF || b != 0xDEADBEEF || c != 0xDEADBEEF) {
			if (errs < 8)
				console_printf("[GLB] i=%u a=%08x b=%08x c=%08x\r\n",
					i, a, b, c);
			errs++;
		}
	}
	console_printf("[GLB] errs=%u/%u\r\n", errs, N);
}

static void test_stack_consistency(void)
{
	volatile uint32_t buf[16];
	uint32_t errs = 0;
	const uint32_t N = 1000000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xCAFE0000u | (i & 0xFFFFu);
		buf[3] = ref;
		uint32_t a = buf[3];
		uint32_t b = buf[3];
		uint32_t c = buf[3];
		if (a != b || b != c || a != ref) {
			if (errs < 8)
				console_printf("[STK] i=%u a=%08x b=%08x c=%08x ref=%08x\r\n",
					i, a, b, c, ref);
			errs++;
		}
	}
	console_printf("[STK] errs=%u/%u\r\n", errs, N);
}

/* Test C: write/read at a BRAM address (1-cycle latency, no CDC, no SDRAM
 * controller). If this passes while the SDRAM probe fails, the bug is
 * latency-dependent — pointing at CPU pipeline forwarding for long-latency
 * loads, not the SDRAM data path. Address 0x00000FF0 is at the top of the
 * 4 KB BRAM, well past the bootloader code. */
static void test_bram_consistency(void)
{
	volatile uint32_t *p = (volatile uint32_t *)0x00000FF0u;
	uint32_t errs = 0;
	const uint32_t N = 1000000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xBEEF0000u | (i & 0xFFFFu);
		*p = ref;
		uint32_t a = *p;
		uint32_t b = *p;
		uint32_t c = *p;
		if (a != b || b != c || a != ref) {
			if (errs < 8)
				console_printf("[BRM] i=%u a=%08x b=%08x c=%08x ref=%08x\r\n",
					i, a, b, c, ref);
			errs++;
		}
	}
	console_printf("[BRM] errs=%u/%u\r\n", errs, N);
}

/* Test D: Force back-to-back same-address SDRAM loads via inline asm.
 * The C-level probes above all pass under -O0 because the compiler
 * spills each load result to its own stack slot, breaking the
 * back-to-back pattern. Inline asm guarantees 3 consecutive `lw`
 * instructions to the same address with no intervening memory ops. */
static void test_b2b_sdram(void)
{
	volatile uint32_t buf;
	uint32_t errs = 0;
	const uint32_t N = 1000000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xA5A50000u | (i & 0xFFFFu);
		buf = ref;
		uint32_t a, b, c;
		asm volatile (
			"lw %0, 0(%3)\n\t"
			"lw %1, 0(%3)\n\t"
			"lw %2, 0(%3)\n\t"
			: "=&r"(a), "=&r"(b), "=&r"(c)
			: "r"(&buf)
			: "memory"
		);
		if (a != b || b != c || a != ref) {
			if (errs < 8)
				console_printf("[B2B] i=%u a=%08x b=%08x c=%08x ref=%08x\r\n",
					i, a, b, c, ref);
			errs++;
		}
	}
	console_printf("[B2B] errs=%u/%u\r\n", errs, N);
}

/* Test E: same as D but to BRAM. Discriminates: if D fails and E passes,
 * the bug is multi-cycle-load-specific (CPU pipeline or SDRAM path). */
static void test_b2b_bram(void)
{
	volatile uint32_t *p = (volatile uint32_t *)0x00000FF0u;
	uint32_t errs = 0;
	const uint32_t N = 1000000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0x5A5A0000u | (i & 0xFFFFu);
		*p = ref;
		uint32_t a, b, c;
		asm volatile (
			"lw %0, 0(%3)\n\t"
			"lw %1, 0(%3)\n\t"
			"lw %2, 0(%3)\n\t"
			: "=&r"(a), "=&r"(b), "=&r"(c)
			: "r"(p)
			: "memory"
		);
		if (a != b || b != c || a != ref) {
			if (errs < 8)
				console_printf("[B2B-BR] i=%u a=%08x b=%08x c=%08x ref=%08x\r\n",
					i, a, b, c, ref);
			errs++;
		}
	}
	console_printf("[B2B-BR] errs=%u/%u\r\n", errs, N);
}

/* Test F: write to stack, force loads to OTHER SDRAM banks/rows in
 * between, then re-read stack. Mimics DOOM's actual pattern: function
 * prolog (sw ra, sp+N) → many I-fetches and heap loads to different
 * banks → epilog (lw ra, sp+N). If the SDRAM controller drops the
 * pending write to bank A while serving banks B/C/D, the read-back
 * returns stale data.
 *
 * Address mapping for 64 MB SDRAM (4 banks × 16 MB):
 *   0x10000000 — bank 0
 *   0x10800000 — bank 1   (8 MB offset)
 *   0x11000000 — bank 2  (16 MB offset)
 *   0x11800000 — bank 3  (24 MB offset)
 * These are far enough apart to guarantee bank/row switches between
 * accesses. Reads are discarded into x0 so no register pollution. */
static void test_diversify_bank(void)
{
	volatile uint32_t buf;
	uint32_t errs = 0;
	const uint32_t N = 500000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xC0DE0000u | (i & 0xFFFFu);
		buf = ref;
		uint32_t a;
		asm volatile (
			"lui  t0, 0x10000\n\t"      /* bank 0 */
			"lw   zero, 0(t0)\n\t"
			"lui  t0, 0x10800\n\t"      /* bank 1 */
			"lw   zero, 0(t0)\n\t"
			"lui  t0, 0x11000\n\t"      /* bank 2 */
			"lw   zero, 0(t0)\n\t"
			"lui  t0, 0x11800\n\t"      /* bank 3 */
			"lw   zero, 0(t0)\n\t"
			"lw   %0, 0(%1)\n\t"        /* re-read buf (bank 0 stack) */
			: "=r"(a)
			: "r"(&buf)
			: "t0", "memory"
		);
		if (a != ref) {
			if (errs < 8)
				console_printf("[DIV] i=%u a=%08x ref=%08x\r\n",
					i, a, ref);
			errs++;
		}
	}
	console_printf("[DIV] errs=%u/%u\r\n", errs, N);
}

/* Test G: same idea but write/read pair surrounds many DIFFERENT-ADDRESS
 * stack writes, mimicking the deep-call-chain pattern where many nested
 * function prologs each do `sw ra, N(sp)` to varying offsets before the
 * outermost function reads its own saved ra. Pure SDRAM, no I-fetches
 * forced — exposes write-buffer drain issues independent of bank switches. */
static void test_write_storm(void)
{
	volatile uint32_t buf[32];
	uint32_t errs = 0;
	const uint32_t N = 500000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xDA7A0000u | (i & 0xFFFFu);
		buf[0] = ref;
		uint32_t a;
		asm volatile (
			/* Many writes to different stack offsets (buf[1..28]).
			 * If the SDRAM write FIFO can't keep up and an early
			 * write to buf[0] gets dropped/reordered, the final
			 * lw of buf[0] returns stale. */
			"sw   zero,  4(%1)\n\t"
			"sw   zero,  8(%1)\n\t"
			"sw   zero, 12(%1)\n\t"
			"sw   zero, 16(%1)\n\t"
			"sw   zero, 20(%1)\n\t"
			"sw   zero, 24(%1)\n\t"
			"sw   zero, 28(%1)\n\t"
			"sw   zero, 32(%1)\n\t"
			"sw   zero, 36(%1)\n\t"
			"sw   zero, 40(%1)\n\t"
			"sw   zero, 44(%1)\n\t"
			"sw   zero, 48(%1)\n\t"
			"sw   zero, 52(%1)\n\t"
			"sw   zero, 56(%1)\n\t"
			"sw   zero, 60(%1)\n\t"
			"sw   zero, 64(%1)\n\t"
			"lw   %0,    0(%1)\n\t"
			: "=r"(a)
			: "r"(&buf[0])
			: "memory"
		);
		if (a != ref) {
			if (errs < 8)
				console_printf("[WST] i=%u a=%08x ref=%08x\r\n",
					i, a, ref);
			errs++;
		}
	}
	console_printf("[WST] errs=%u/%u\r\n", errs, N);
}

/* Test H: I-cache thrashing.
 *
 * Z-Core I-cache: 256 entries × 1 word per line = 1 KB, direct-mapped.
 * Index = addr[9:2]. Functions placed 1024 bytes apart alias to the
 * SAME cache index. Calling N such functions in sequence forces N
 * back-to-back cache evictions + refills from SDRAM, while we have
 * a pending stack write to verify.
 *
 * Pattern: write buf=ref → call 16 aliased stub functions (heavy
 * I-cache thrashing + I-fetch traffic to SDRAM) → read buf back.
 * If I-fetch refills mess up data-load responses (interconnect /
 * CDC / SDRAM controller mixing channels), this catches it. */

#define THRASH_FN(n)                                                    \
__attribute__((noinline,aligned(1024),used,                             \
               section(".text.thrash" #n)))                             \
static void thrash##n(void) { asm volatile (""); }

THRASH_FN(0)  THRASH_FN(1)  THRASH_FN(2)  THRASH_FN(3)
THRASH_FN(4)  THRASH_FN(5)  THRASH_FN(6)  THRASH_FN(7)
THRASH_FN(8)  THRASH_FN(9)  THRASH_FN(10) THRASH_FN(11)
THRASH_FN(12) THRASH_FN(13) THRASH_FN(14) THRASH_FN(15)

static void (*const thrash_funcs[16])(void) = {
	thrash0,  thrash1,  thrash2,  thrash3,
	thrash4,  thrash5,  thrash6,  thrash7,
	thrash8,  thrash9,  thrash10, thrash11,
	thrash12, thrash13, thrash14, thrash15
};

static void test_icache_thrash(void)
{
	volatile uint32_t buf;
	uint32_t errs = 0;
	const uint32_t N = 200000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0x1CA70000u | (i & 0xFFFFu);
		buf = ref;
		/* 16 indirect calls — each forces I-cache miss+refill at
		 * the same aliased cache index, plus AXI-Lite read traffic
		 * to SDRAM for instruction fetch. */
		for (int j = 0; j < 16; j++)
			thrash_funcs[j]();
		uint32_t a = buf;
		if (a != ref) {
			if (errs < 8)
				console_printf("[ITH] i=%u a=%08x ref=%08x &buf=%08x\r\n",
					i, a, ref, (unsigned)(uintptr_t)&buf);
			errs++;
		}
	}
	console_printf("[ITH] errs=%u/%u\r\n", errs, N);
}

/* Test I: I-cache thrash + diversified-bank reads + stack write/read.
 * Combines all three stressors that together approximate DOOM's
 * pattern at the moment of crash: code spread across SDRAM (I-fetch
 * misses), data scattered across banks, stack write that must survive
 * the full storm before being read back. */
static void test_icache_plus_bank(void)
{
	volatile uint32_t buf;
	uint32_t errs = 0;
	const uint32_t N = 100000;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t ref = 0xCABA0000u | (i & 0xFFFFu);
		buf = ref;
		/* Bank-diversified reads */
		asm volatile (
			"lui  t0, 0x10000\n\t" "lw zero, 0(t0)\n\t"
			"lui  t0, 0x10800\n\t" "lw zero, 0(t0)\n\t"
			"lui  t0, 0x11000\n\t" "lw zero, 0(t0)\n\t"
			"lui  t0, 0x11800\n\t" "lw zero, 0(t0)\n\t"
			::: "t0", "memory"
		);
		/* I-cache thrashing via indirect calls */
		for (int j = 0; j < 8; j++)
			thrash_funcs[j]();
		uint32_t a = buf;
		if (a != ref) {
			if (errs < 8)
				console_printf("[IPB] i=%u a=%08x ref=%08x\r\n",
					i, a, ref);
			errs++;
		}
	}
	console_printf("[IPB] errs=%u/%u\r\n", errs, N);
}

/* Full SDRAM walk: every word in the free region, multiple patterns.
 * Region:  0x12500000 .. 0x13F00000   (~26 MB, 6.8M words)
 *   - Above WAD end (0x1241112C) with ~1 MB safety margin
 *   - Below SDRAM top (0x14000000) with 1 MB safety margin
 * Patterns: address-as-data, 0xAAAAAAAA, 0x55555555
 * Reports first 32 mismatches and OR of all bit-diffs (stuck-bit fingerprint).
 */
static void test_sdram_full(void)
{
	volatile uint32_t * const base = (volatile uint32_t *)0x12500000u;
	const uint32_t words = (0x13F00000u - 0x12500000u) / 4u;  /* 6,815,744 */

	uint32_t errs = 0;
	uint32_t reported = 0;
	uint32_t bad_bits_or = 0;

	/* Pattern 1: address-as-data (catches stuck-at + address-line faults) */
	for (uint32_t i = 0; i < words; i++)
		base[i] = (uint32_t)(uintptr_t)&base[i];
	for (uint32_t i = 0; i < words; i++) {
		uint32_t exp = (uint32_t)(uintptr_t)&base[i];
		uint32_t got = base[i];
		if (got != exp) {
			errs++;
			bad_bits_or |= (got ^ exp);
			if (reported < 32) {
				console_printf("[SDR-A] @%08x got=%08x exp=%08x diff=%08x\r\n",
					(uint32_t)&base[i], got, exp, got ^ exp);
				reported++;
			}
		}
	}

	/* Pattern 2: 0xAAAAAAAA (catches stuck-low data bits) */
	for (uint32_t i = 0; i < words; i++)
		base[i] = 0xAAAAAAAAu;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t got = base[i];
		if (got != 0xAAAAAAAAu) {
			errs++;
			bad_bits_or |= (got ^ 0xAAAAAAAAu);
			if (reported < 32) {
				console_printf("[SDR-AA] @%08x got=%08x diff=%08x\r\n",
					(uint32_t)&base[i], got, got ^ 0xAAAAAAAAu);
				reported++;
			}
		}
	}

	/* Pattern 3: 0x55555555 (catches stuck-high data bits) */
	for (uint32_t i = 0; i < words; i++)
		base[i] = 0x55555555u;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t got = base[i];
		if (got != 0x55555555u) {
			errs++;
			bad_bits_or |= (got ^ 0x55555555u);
			if (reported < 32) {
				console_printf("[SDR-55] @%08x got=%08x diff=%08x\r\n",
					(uint32_t)&base[i], got, got ^ 0x55555555u);
				reported++;
			}
		}
	}

	console_printf("[SDR] full-walk: words=%u patterns=3 errs=%u bad_bits_OR=%08x\r\n",
		words, errs, bad_bits_or);
}

static void run_consistency_probes(void)
{
	console_puts("[PROBE] global read consistency (SDRAM .data)...\r\n");
	test_global_consistency();
	console_puts("[PROBE] stack write/read consistency (SDRAM stack)...\r\n");
	test_stack_consistency();
	console_puts("[PROBE] BRAM write/read consistency (1-cycle slave)...\r\n");
	test_bram_consistency();
	console_puts("[PROBE] back-to-back lw SDRAM (forced asm)...\r\n");
	test_b2b_sdram();
	console_puts("[PROBE] back-to-back lw BRAM (forced asm)...\r\n");
	test_b2b_bram();
	console_puts("[PROBE] diversify-bank (write/cross-bank reads/re-read)...\r\n");
	test_diversify_bank();
	console_puts("[PROBE] write storm (many stack writes then re-read)...\r\n");
	test_write_storm();
	console_puts("[PROBE] I-cache thrash (16 aliased stubs)...\r\n");
	test_icache_thrash();
	console_puts("[PROBE] I-cache thrash + bank diversify...\r\n");
	test_icache_plus_bank();
	console_puts("[PROBE] SDRAM full walk (3 patterns, ~26 MB)...\r\n");
	console_puts("[PROBE] SDRAM full walk skipped\r\n");
	//test_sdram_full();
}


/* ================================================================ */

static void preflight(void)
{
	pf_errors = 0;

	console_puts("\n--- Z-Core DOOM Pre-flight ---\n");
	console_printf("  Code : 0x%08x\n", (uint32_t)SDRAM_BASE);
	console_printf("  Stack: 0x%08x\n", (uint32_t)0x10F00000);
	console_printf("  WAD  : 0x%08x (%u bytes)\n", (uint32_t)WAD_BASE, (uint32_t)WAD_SIZE);
	console_puts("  Checking peripherals...\n");

	pf_uart();
	pf_timer();
	pf_vga();
	pf_gpio();
	pf_sdram();
	pf_wad();

	if (pf_errors == 0)
		console_puts("  All checks passed.\n");
	else
		console_printf("  *** %d check(s) FAILED ***\n", pf_errors);

	console_printf("[UART] SDRAM code @ 0x10000000, WAD @ 0x%08x\r\n",
	               (uint32_t)WAD_BASE);
	console_puts("--- Starting DOOM ---\n\n");
}


int main(void)
{
	console_init();
	console_puts("[UART] console_init OK\r\n");

	preflight();

	run_consistency_probes();

	console_puts("[UART] preflight done, entering D_DoomMain\r\n");
	D_DoomMain();
	return 0;
}
