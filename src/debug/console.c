/*
 * Hatari - console.c
 * 
 * Copyright (C) 2012 by Eero Tamminen
 *
 * This file is distributed under the GNU Public License, version 2 or at
 * your option any later version. Read the file gpl.txt for details.
 *
 * console.c - catching of emulated console output with minimal VT52 emulation.
 */
const char Console_fileid[] = "Hatari console.c : " __DATE__ " " __TIME__;

#include <stdio.h>
#include <string.h>
#include <SDL.h>

#include "main.h"
#include "m68000.h"
#include "stMemory.h"
#include "hatari-glue.h"
#include "console.h"
#include "options.h"

/**
 * Maps Atari characters to their closest ASCII equivalents.
 */
static void map_character(Uint8 value)
{
	static const Uint8 map_0_31[32] = {
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0x00 */
		/* white space */
		'\b','\t','\n','.','.','\r', '.', '.',	/* 0x08 */
		/* LED numbers */
		'0', '1', '2', '3', '4', '5', '6', '7',	/* 0x10 */
		'8', '9', '.', '.', '.', '.', '.', '.' 	/* 0x18 */
	};
	static const Uint8 map_128_255[128] = {
		/* accented characters */
		'C', 'U', 'e', 'a', 'a', 'a', 'a', 'c',	/* 0x80 */
		'e', 'e', 'e', 'i', 'i', 'i', 'A', 'A',	/* 0x88 */
		'E', 'a', 'A', 'o', 'o', 'o', 'u', 'u',	/* 0x90 */
		'y', 'o', 'u', 'c', '.', 'Y', 'B', 'f',	/* 0x98 */
		'a', 'i', 'o', 'u', 'n', 'N', 'a', 'o',	/* 0xA0 */
		'?', '.', '.', '.', '.', 'i', '<', '>',	/* 0xA8 */
		'a', 'o', 'O', 'o', 'o', 'O', 'A', 'A',	/* 0xB0 */
		'O', '"','\'', '.', '.', 'C', 'R', '.',	/* 0xB8 */
		'j', 'J', '.', '.', '.', '.', '.', '.',	/* 0xC0 */
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0xC8 */
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0xD0 */
		'.', '.', '.', '.', '.', '.', '^', '.',	/* 0xD8 */
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0xE0 */
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0xE8 */
		'.', '.', '.', '.', '.', '.', '.', '.',	/* 0xF0 */
		'.', '.', '.', '.', '.', '.', '.', '.'	/* 0xF8 */
	};
	/* map normal characters to host console */
	if (value < 32) {
		fputc(map_0_31[value], stderr);
	} else if (value > 127) {
		fputc(map_128_255[value-128], stderr);
	} else {
		fputc(value, stderr);
	}
}


/**
 * Convert given console character output to ASCII.
 * Accepts one character at the time, parses VT52 escape codes
 * and outputs them on console.
 * 
 * On host, TOS cursor forwards movement is done with spaces,
 * backwards movement is delayed until next non-white character
 * at which point output switches to next line.  Other VT52
 * escape sequences than cursor movement are ignored.
 */
static void vt52_emu(Uint8 value)
{
	/* state machine to handle/ignore VT52 escape sequence */
	static int escape_index;
	static int escape_target;
	static int hpos_host, hpos_tos;
	static bool need_nl;
	static enum {
		ESCAPE_NONE, ESCAPE_POSITION
	} escape_type;

	if (escape_target) {
		if (++escape_index == 1) {
			/* VT52 escape sequences */
			switch(value) {
			case 'E':	/* clear screen+home -> newline */
				fputs("\n", stderr);
				hpos_host = 0;
				break;
			/* sequences with arguments */
			case 'b':	/* foreground color */
			case 'c':	/* background color */
				escape_target = 2;
				return;
			case 'Y':	/* cursor position */
				escape_type = ESCAPE_POSITION;
				escape_target = 3;
				return;
			}
		} else if (escape_index < escape_target) {
			return;
		}
		if (escape_type == ESCAPE_POSITION) {
			/* last item gives horizontal position */
			hpos_tos = value - ' ';
			if (hpos_tos > 79) {
				hpos_tos = 79;
			} else if (hpos_tos < 0) {
				hpos_tos = 0;
			}
			if (hpos_tos > hpos_host) {
				fprintf(stderr, "%*s", hpos_tos - hpos_host, "");
				hpos_host = hpos_tos;
			} else if (hpos_tos < hpos_host) {
				need_nl = true;
			}
		}
		/* escape sequence end */
		escape_target = 0;
		return;
	}
	if (value == 27) {
		/* escape sequence start */
		escape_type = ESCAPE_NONE;
		escape_target = 1;
		escape_index = 0;
		return;
	}

	/* do newline & indent for backwards movement only when necessary */
	if (need_nl) {
		/* TOS cursor horizontal movement until host output */
		switch (value) {
		case ' ':
			hpos_tos++;
			return;
		case '\b':
			hpos_tos--;
			return;
		case '\t':
			hpos_tos = (hpos_tos + 8) & 0xfff0;
			return;
		case '\r':
		case '\n':
			hpos_tos = 0;
			break;
		}
		fputs("\n", stderr);
		if (hpos_tos > 0 && hpos_tos < 80) {
			fprintf(stderr, "%*s", hpos_tos, "");
			hpos_host = hpos_tos;
		} else {
			hpos_host = 0;
		}
		need_nl = false;
	}

	/* host cursor horizontal movement */
	switch (value) {
	case '\b':
		hpos_host--;
		break;
	case '\t':
		hpos_host = (hpos_host + 8) & 0xfff0;
		break;
	case '\r':
	case '\n':
		hpos_host = 0;
		break;
	default:
		hpos_host++;
		break;
	}
	map_character(value);
}


/**
 * Catch requested xconout vector calls and show their output on console
 */
void Console_Check(void)
{
	Uint32 pc, xconout, stack, stackbeg, stackend;
	int increment;
	Uint16 chr;

	/* xconout vector for requested device? */
	xconout = STMemory_ReadLong(0x57e + ConOutDevice * SIZE_LONG);
	pc = M68000_GetPC();
	if (pc != xconout) {
		return;
	}

	/* assumptions about xconout function:
	 * - c declaration: leftmost item on top of stackframe
	 * - args: WORD device, WORD character to output
	 * - can find the correct stackframe arguments by skipping
	 *   wrong looking stack content from intermediate functions
	 *   (bsr/jsr return addresses are > 0xff, local stack args
	 *   could be an issue but hopefully don't match device number
	 *   in any of the TOSes nor in MiNT or its conout devices)
	 */
	stackbeg = stack = Regs[REG_A7];
	stackend = stack + 16;
	increment = SIZE_LONG;
	while (STMemory_ReadWord(stack) != ConOutDevice) {
		stack += increment;
		if (stack > stackend) {
			if (increment == SIZE_LONG) {
				/* skipping return addresses not enough,
				 * try skipping potential local args too
				 */
				fprintf(stderr, "WARNING: xconout stack args not found by skipping return addresses, trying short skipping.\n");
				increment = SIZE_WORD;
				stack = stackbeg;
				continue;
			}
			/* failed */
			fprintf(stderr, "WARNING: xconout args not found from stack.\n");
			return;
		}
	}
	chr = STMemory_ReadWord(stack + SIZE_WORD);
	if (chr & 0xff00) {
		fprintf(stderr, "WARNING: xconout character has high bits: 0x%x '%c'.\n", chr, chr&0xff);
		/* higher bits, assume not correct arg */
		return;
	}
	switch(ConOutDevice) {
	case 2:	/* EmuTOS/TOS/MiNT/etc console */
		vt52_emu(chr);
		break;
	case 1: /* EmuTOS RS232 debug console */
	case 3: /* EmuTOS MIDI debug console */
	case 5: /* raw screen device (no escape sequence / control char processing) */
		map_character(chr);
		break;
	}
}
