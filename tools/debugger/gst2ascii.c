/*
 * Hatari - gst2ascii.c
 * 
 * Copyright (C) 2013-2017 by Eero Tamminen
 * 
 * This file is distributed under the GNU General Public License, version 2
 * or at your option any later version. Read the file gpl.txt for details.
 * 
 * Convert DRI/GST and a.out format symbol table in a binary into ASCII symbols
 * file accepted by Hatari debugger and its profiler data post-processor.
 * This will also allow manual editing of the symbol table (removing irrelevant
 * labels or adding missing symbols for functions).
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#if defined(__MINT__)	/* assume MiNT/lib is always big-endian */
# define SDL_SwapBE16(x) x
# define SDL_SwapBE32(x) x
#else
# include <SDL_endian.h>
#endif
#include <assert.h>
#include "../../src/debug/a.out.h"

typedef enum {
	SYMTYPE_TEXT = 1,
	SYMTYPE_DATA = 2,
	SYMTYPE_BSS  = 4,
	SYMTYPE_ABS  = 8
} symtype_t;

typedef struct {
	char *name;
	uint32_t address;
	symtype_t type;
} symbol_t;

typedef struct {
	int count;		/* final symbol count */
	int symbols;		/* initial symbol count */
	symbol_t *addresses;	/* items sorted by address */
	symbol_t *names;	/* items sorted by symbol name */
	char *strtab;
} symbol_list_t;

typedef struct {
	uint32_t offset;
	uint32_t end;
} prg_section_t;

/* Magic used to denote different symbol table formats */
#define SYMBOL_FORMAT_GNU  0x474E555f	/* "MiNT" */
#define SYMBOL_FORMAT_MINT 0x4D694E54	/* "GNU_" */
#define SYMBOL_FORMAT_DRI  0x0

/* Magic identifying Atari programs */
#define ATARI_PROGRAM_MAGIC 0x601A


/* ------------------ options & usage ------------------ */

#ifdef WIN32
#define PATHSEP '\\'
#else
#define PATHSEP '/'
#endif

#define ARRAY_SIZE(x) (int)(sizeof(x)/sizeof(x[0]))

static const char *PrgPath;

static struct {
	symtype_t notypes;
	bool no_local;
	bool no_obj;
	bool sort_name;
} Options;

/*
 * Show program usage, given error message, and exit
 */
static void usage(const char *msg)
{
	const struct {
		const char opt;
		const char *desc;
	} OptInfo[] = {
		{ 'a', "no absolute symbols (are values, not addresses)" },
		{ 'b', "no BSS symbols" },
		{ 'd', "no DATA symbols" },
		{ 't', "no TEXT symbols" },
		{ 'l', "no local (.L*) symbols" },
		{ 'o', "no object symbols (filenames or GCC internals)" },
		{ 'n', "sort by name (not address)" },
	};
	const char *name;
	int i;

	if ((name = strrchr(PrgPath, PATHSEP))) {
		name++;
	} else {
		name = PrgPath;
	}
	fprintf(stderr,
		"\n"
		"Usage: %s [options] <Atari program>\n"
		"\n"
		"Outputs given program (DRI/GST or a.out format) symbol table\n"
		"content in ASCII format accepted by Hatari debugger and its\n"
		"profiler data post-processor.\n"
		"\n"
		"All symbol addresses are output as TEXT relative, i.e. you need\n"
		"to give only that as section address for the Hatari debugger:\n"
		"\tsymbols <filename> TEXT\n"
		"\n"
		"Options:\n", name);
	for (i = 0; i < ARRAY_SIZE(OptInfo); i++) {
		fprintf(stderr, "\t-%c\t%s\n", OptInfo[i].opt, OptInfo[i].desc);
	}
	if (msg) {
		fprintf(stderr, "\nERROR: %s!\n", msg);
	}

	exit(msg != NULL);
}

/* ------------------ load and free functions ------------------ */

/**
 * compare function for qsort() to sort according to symbol address
 */
static int symbols_by_address(const void *s1, const void *s2)
{
	uint32_t addr1 = ((const symbol_t*)s1)->address;
	uint32_t addr2 = ((const symbol_t*)s2)->address;

	if (addr1 < addr2) {
		return -1;
	}
	if (addr1 > addr2) {
		return 1;
	}
	return 0;
}

/**
 * compare function for qsort() to sort according to symbol name
 */
static int symbols_by_name(const void *s1, const void *s2)
{
	const char* name1 = ((const symbol_t*)s1)->name;
	const char* name2 = ((const symbol_t*)s2)->name;
	int ret;

	ret = strcmp(name1, name2);
	return ret;
}

/**
 * check for duplicate addresses in symbol list
 */
static void symbols_check_addresses(const symbol_t *syms, int count)
{
	int i, j;

	for (i = 0; i < (count - 1); i++)
	{
		/* absolute symbols have values, not addresses */
		if (syms[i].type == SYMTYPE_ABS) {
			continue;
		}
		for (j = i + 1; j < count && syms[i].address == syms[j].address; j++) {
			if (syms[j].type == SYMTYPE_ABS) {
				continue;
			}
			fprintf(stderr, "WARNING: symbols '%s' & '%s' have the same 0x%x address.\n",
				syms[i].name, syms[j].name, syms[i].address);
			i = j;
		}
	}
}

/**
 * check for duplicate names in symbol list
 */
static void symbols_check_names(const symbol_t *syms, int count)
{
	int i, j;

	for (i = 0; i < (count - 1); i++)
	{
		for (j = i + 1; j < count && strcmp(syms[i].name, syms[j].name) == 0; j++) {
			fprintf(stderr, "WARNING: addresses 0x%x & 0x%x have the same '%s' name.\n",
				syms[i].address, syms[j].address, syms[i].name);
			i = j;
		}
	}
}

/**
 * Allocate symbol list & names for given number of items.
 * Return allocated list or NULL on failure.
 */
static symbol_list_t* symbol_list_alloc(int symbols)
{
	symbol_list_t *list;

	if (!symbols) {
		return NULL;
	}
	list = calloc(1, sizeof(symbol_list_t));
	if (list) {
		list->names = malloc(symbols * sizeof(symbol_t));
		if (!list->names) {
			free(list);
			list = NULL;
		}
	}
	return list;
}

/**
 * Free symbol list & names.
 */
static void symbol_list_free(symbol_list_t *list)
{
	if (list) {
		if (list->names) {
			free(list->names);
		}
		free(list);
	}
}

/**
 * Return symbol type identifier char
 */
static char symbol_char(int type)
{
	switch (type) {
	case SYMTYPE_TEXT: return 'T';
	case SYMTYPE_DATA: return 'D';
	case SYMTYPE_BSS:  return 'B';
	case SYMTYPE_ABS:  return 'A';
	default: return '?';
	}
}

/**
 * Return true if symbol name matches internal GCC symbol name,
 * or is object / file name.
 */
static bool symbol_remove_obj(const char *name)
{
	static const char *gcc_sym[] = {
		"___gnu_compiled_c",
		"gcc2_compiled."
	};
	int i, len = strlen(name);
	/* object (.a or .o) / file name? */
	if (len > 2 && ((name[len-2] == '.' && (name[len-1] == 'a' || name[len-1] == 'o')) || strchr(name, '/'))) {
		    return true;
	}
	/* useless symbols GCC (v2) seems to add to every object? */
	for (i = 0; i < ARRAY_SIZE(gcc_sym); i++) {
		if (strcmp(name, gcc_sym[i]) == 0) {
			return true;
		}
	}
	return false;
}


/**
 * Load symbols of given type and the symbol address addresses from
 * DRI/GST format symbol table, and add given offsets to the addresses:
 *	http://toshyp.atari.org/en/005005.html
 * Return symbols list or NULL for failure.
 */
static symbol_list_t* symbols_load_dri(FILE *fp, prg_section_t *sections, uint32_t tablesize)
{
	int i, count, symbols, invalid;
	int notypes, dtypes, locals, ofiles;
	prg_section_t *section;
	symbol_list_t *list;
	symtype_t symtype;
#define DRI_ENTRY_SIZE	14
	char name[23];
	uint16_t symid;
	uint32_t address;

	if (tablesize % DRI_ENTRY_SIZE) {
		fprintf(stderr, "ERROR: invalid DRI/GST symbol table size %d!\n", tablesize);
		return NULL;
	}
	symbols = tablesize / DRI_ENTRY_SIZE;
	if (!(list = symbol_list_alloc(symbols))) {
		return NULL;
	}

	invalid = dtypes = notypes = ofiles = locals = count = 0;
	for (i = 1; i <= symbols; i++) {
		/* read DRI symbol table slot */
		if (fread(name, 8, 1, fp) != 1 ||
		    fread(&symid, sizeof(symid), 1, fp) != 1 ||
		    fread(&address, sizeof(address), 1, fp) != 1) {
			break;
		}
		address = SDL_SwapBE32(address);
		symid = SDL_SwapBE16(symid);

		/* GST extended DRI symbol format? */
		if ((symid & 0x0048)) {
			/* next slot is rest of name */
			i += 1;
			if (fread(name+8, 14, 1, fp) != 1) {
				break;
			}
			name[22] = '\0';
		} else {
			name[8] = '\0';
		}

		/* check section */
		switch (symid & 0xf00) {
		case 0x0200:
			symtype = SYMTYPE_TEXT;
			section = &(sections[0]);
			break;
		case 0x0400:
			symtype = SYMTYPE_DATA;
			section = &(sections[1]);
			break;
		case 0x0100:
			symtype = SYMTYPE_BSS;
			section = &(sections[2]);
			break;
		default:
			if ((symid & 0xe000) == 0xe000) {
				dtypes++;
				continue;
			}
			if ((symid & 0x4000) == 0x4000) {
				symtype = SYMTYPE_ABS;
				section = NULL;
				break;
			}
			fprintf(stderr, "WARNING: ignoring symbol '%s' in slot %d of unknown type 0x%x.\n", name, i, symid);
			invalid++;
			continue;
		}
		if (Options.notypes & symtype) {
			notypes++;
			continue;
		}
		if (Options.no_local) {
			if (name[0] == '.' && name[1] == 'L') {
				locals++;
				continue;
			}
		}
		if (Options.no_obj) {
			if (symbol_remove_obj(name)) {
				ofiles++;
				continue;
			}
		}
		if (section) {
			address += section->offset;
			if (address > section->end) {
				fprintf(stderr, "WARNING: ignoring symbol '%s' of type %c in slot %d with invalid offset 0x%x (>= 0x%x).\n",
					name, symbol_char(symtype), i, address, section->end);
				invalid++;
				continue;
			}
		}
		list->names[count].address = address;
		list->names[count].type = symtype;
		list->names[count].name = strdup(name);
		assert(list->names[count].name);
		count++;
	}
	if (i <= symbols) {
		perror("ERROR: reading symbol failed");
		symbol_list_free(list);
		return NULL;
	}
	if (invalid) {
		fprintf(stderr, "NOTE: ignored %d invalid symbols.\n", invalid);
	}
	if (dtypes) {
		fprintf(stderr, "NOTE: ignored %d debugging symbols.\n", dtypes);
	}
	if (notypes) {
		fprintf(stderr, "NOTE: ignored %d other unwanted symbol types.\n", notypes);
	}
	if (locals) {
		fprintf(stderr, "NOTE: ignored %d unnamed / local symbols (= name starts with '.L').\n", locals);
	}
	if (ofiles) {
		/* object file path names most likely get truncated and
		 * as result cause unnecessary symbol name conflicts in
		 * addition to object file addresses conflicting with
		 * first symbol in the object file.
		 */
		fprintf(stderr, "NOTE: ignored %d object symbols (= name has '/', ends in '.[ao]' or is GCC internal).\n", ofiles);
	}
	list->symbols = symbols;
	list->count = count;
	return list;
}


/**
 * Load symbols of given type and the symbol address addresses from
 * a.out format symbol table, and add given offsets to the addresses:
 * Return symbols list or NULL for failure.
 */
static symbol_list_t* symbols_load_gnu(FILE *fp, prg_section_t *sections, Uint32 tablesize, Uint32 stroff, Uint32 strsize)
{
	size_t slots = tablesize / SIZEOF_STRUCT_NLIST;
	size_t i;
	size_t strx;
	unsigned char *p;
	char *name;
	symbol_t *sym;
	symtype_t symtype;
	uint32_t address;
	uint32_t nread;
	symbol_list_t *list;
	unsigned char n_type;
	unsigned char n_other;
	unsigned short n_desc;
	static char dummy[] = "<invalid>";
	int dtypes, locals, ofiles, count, notypes, invalid, weak;
	prg_section_t *section;

	if (!(list = symbol_list_alloc(slots))) {
		return NULL;
	}

	list->strtab = (char *)malloc(tablesize + strsize);

	if (list->strtab == NULL)
	{
		symbol_list_free(list);
		return NULL;
	}

	nread = fread(list->strtab, tablesize + strsize, 1, fp);
	if (nread != 1)
	{
		perror("ERROR: reading symbols failed");
		symbol_list_free(list);
		return NULL;
	}

	p = (unsigned char *)list->strtab;
	sym = list->names;

	weak = invalid = dtypes = notypes = ofiles = locals = count = 0;
	for (i = 0; i < slots; i++)
	{
		strx = SDL_SwapBE32(*(Uint32*)p);
		p += 4;
		n_type = *p++;
		n_other = *p++;
		n_desc = SDL_SwapBE16(*(Uint16*)p);
		p += 2;
		address = SDL_SwapBE32(*(Uint32*)p);
		p += 4;
		name = dummy;
		if (!strx) {
			invalid++;
			continue;
		}
		if (strx >= strsize) {
			fprintf(stderr, "symbol name index %x out of range\n", (unsigned int)strx);
			invalid++;
			continue;
		}
		name = list->strtab + strx + stroff;

		if (n_type & N_STAB)
		{
			dtypes++;
			continue;
		}
		section = NULL;
		switch (n_type & (N_TYPE|N_EXT))
		{
		case N_UNDF:
		case N_UNDF|N_EXT:
			/* shouldn't happen here */
			weak++;
			continue;
		case N_ABS:
		case N_ABS|N_EXT:
			symtype = SYMTYPE_ABS;
			break;
		case N_TEXT:
		case N_TEXT|N_EXT:
			symtype = SYMTYPE_TEXT;
			section = &(sections[0]);
			break;
		case N_DATA:
		case N_DATA|N_EXT:
			symtype = SYMTYPE_DATA;
			section = &(sections[1]);
			break;
		case N_BSS:
		case N_BSS|N_EXT:
		case N_COMM:
		case N_COMM|N_EXT:
			symtype = SYMTYPE_BSS;
			section = &(sections[2]);
			break;
		case N_FN: /* filenames, not object addresses? */
			dtypes++;
			continue;
		case N_SIZE:
		case N_WARNING:
		case N_SETA:
		case N_SETT:
		case N_SETD:
		case N_SETB:
		case N_SETV:
			dtypes++;
			continue;
		case N_WEAKU:
		case N_WEAKT:
		case N_WEAKD:
		case N_WEAKB:
			weak++;
			continue;
		default:
			fprintf(stderr, "WARNING: ignoring symbol '%s' in slot %u of unknown type 0x%x.\n", name, (unsigned int)i, n_type);
			invalid++;
			continue;
		}
		/*
		 * the value of a common symbol is its size, not its address:
		 */
		if (((n_type & N_TYPE) == N_COMM) ||
			(((n_type & N_EXT) && (n_type & N_TYPE) == N_UNDF && address != 0)))
		{
			/* if we ever want to know a symbols size, get that here */
			fprintf(stderr, "WARNING: ignoring common symbol '%s' in slot %u.\n", sym->name, (unsigned int)i);
			dtypes++;
			continue;
		}
		if (Options.notypes & symtype) {
			notypes++;
			continue;
		}
		if (Options.no_local) {
			if (name[0] == '.' && name[1] == 'L') {
				locals++;
				continue;
			}
		}
		if (Options.no_obj) {
			if (symbol_remove_obj(name)) {
				ofiles++;
				continue;
			}
		}
		if (section) {
			address += sections[0].offset;	/* all GNU symbol addresses are TEXT relative */
			if (address > section->end) {
				fprintf(stderr, "WARNING: ignoring symbol '%s' of type %c in slot %u with invalid offset 0x%x (>= 0x%x).\n",
					name, symbol_char(symtype), (unsigned int)i, address, section->end);
				invalid++;
				continue;
			}
		}
		sym->address = address;
		sym->type = symtype;
		sym->name = name;
		sym++;
		count++;
		(void) n_desc;
		(void) n_other;
	}

	if (invalid) {
		fprintf(stderr, "NOTE: ignored %d invalid symbols.\n", invalid);
	}
	if (dtypes) {
		fprintf(stderr, "NOTE: ignored %d debugging symbols.\n", dtypes);
	}
	if (weak) {
		fprintf(stderr, "NOTE: ignored %d weak / undefined symbols.\n", weak);
	}
	if (notypes) {
		fprintf(stderr, "NOTE: ignored %d other unwanted symbol types.\n", notypes);
	}
	if (locals) {
		fprintf(stderr, "NOTE: ignored %d unnamed / local symbols (= name starts with '.L').\n", locals);
	}
	if (ofiles) {
		/* object file path names most likely get truncated and
		 * as result cause unnecessary symbol name conflicts in
		 * addition to object file addresses conflicting with
		 * first symbol in the object file.
		 */
		fprintf(stderr, "NOTE: ignored %d object symbols (= name has '/', ends in '.[ao]' or is GCC internal).\n", ofiles);
	}

	list->symbols = slots;
	list->count = count;
	return list;
}


/**
 * Print program header information.
 * Return false for unrecognized symbol table type.
 */
static bool symbols_print_prg_info(Uint32 tabletype, Uint32 prgflags, Uint16 relocflag)
{
	static const struct {
		Uint32 flag;
		const char *name;
	} flags[] = {
		{ 0x0001, "FASTLOAD"   },
		{ 0x0002, "TTRAMLOAD"  },
		{ 0x0004, "TTRAMMEM"   },
		{ 0x0008, "MINIMUM"    }, /* MagiC */
		{ 0x1000, "SHAREDTEXT" }
	};
	const char *info;
	int i;

	switch (tabletype) {
	case SYMBOL_FORMAT_MINT: /* "MiNT" */
		info = "GCC/MiNT executable, GST symbol table";
		break;
	case SYMBOL_FORMAT_GNU:	 /* "GNU_" */
		info = "GCC/MiNT executable, a.out symbol table";
		break;
	case SYMBOL_FORMAT_DRI:
		info = "TOS executable, DRI / GST symbol table";
		break;
	default:
		fprintf(stderr, "ERROR: unknown executable type 0x%x!\n", tabletype);
		return false;
	}
	fprintf(stderr, "%s, reloc=%d, program flags:", info, relocflag);
	/* bit flags */
	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		if (prgflags & flags[i].flag) {
			fprintf(stderr, " %s", flags[i].name);
		}
	}
	/* memory protection flags */
	switch((prgflags >> 4) & 3) {
		case 0:	info = "PRIVATE";  break;
		case 1: info = "GLOBAL";   break;
		case 2: info = "SUPER";    break;
		case 3: info = "READONLY"; break;
	}
	fprintf(stderr, " %s (0x%x)\n", info, prgflags);
	return true;
}

/**
 * Parse program header and use symbol table format specific loader
 * loader function to load the symbols.
 * Return symbols list or NULL for failure.
 */
static symbol_list_t* symbols_load_binary(FILE *fp)
{
	Uint32 textlen, datalen, bsslen, tablesize, tabletype, prgflags;
	prg_section_t sections[3];
	int offset, reads = 0;
	Uint16 relocflag;
	symbol_list_t* symbols;
	Uint32 symoff = 0;
	Uint32 stroff = 0;
	Uint32 strsize = 0;

	/* get TEXT, DATA & BSS section sizes */
	reads += fread(&textlen, sizeof(textlen), 1, fp);
	textlen = SDL_SwapBE32(textlen);
	reads += fread(&datalen, sizeof(datalen), 1, fp);
	datalen = SDL_SwapBE32(datalen);
	reads += fread(&bsslen, sizeof(bsslen), 1, fp);
	bsslen = SDL_SwapBE32(bsslen);

	/* get symbol table size & type and check that all reads succeeded */
	reads += fread(&tablesize, sizeof(tablesize), 1, fp);
	tablesize = SDL_SwapBE32(tablesize);
	reads += fread(&tabletype, sizeof(tabletype), 1, fp);
	tabletype = SDL_SwapBE32(tabletype);

	/* get program header and whether there's reloc table */
	reads += fread(&prgflags, sizeof(prgflags), 1, fp);
	prgflags = SDL_SwapBE32(prgflags);
	reads += fread(&relocflag, sizeof(relocflag), 1, fp);
	relocflag = SDL_SwapBE32(relocflag);
	
	if (reads != 7) {
		fprintf(stderr, "ERROR: program header reading failed!\n");
		return NULL;
	}
	/*
	 * check for GNU-style symbol table in aexec header
	 */
	if (tabletype == SYMBOL_FORMAT_MINT) { /* MiNT */
		Uint32 magic1, magic2;
		Uint32 dummy;
		Uint32 a_text, a_data, a_bss, a_syms, a_entry, a_trsize, a_drsize;
		Uint32 g_tparel_pos, g_tparel_size, g_stkpos, g_symbol_format;

		reads  = fread(&magic1, sizeof(magic1), 1, fp);
		magic1 = SDL_SwapBE32(magic1);
		reads += fread(&magic2, sizeof(magic2), 1, fp);
		magic2 = SDL_SwapBE32(magic2);
		if (reads == 2 &&
			((magic1 == 0x283a001a && magic2 == 0x4efb48fa) || 	/* Original binutils: move.l 28(pc),d4; jmp 0(pc,d4.l) */
			 (magic1 == 0x203a001a && magic2 == 0x4efb08fa))) {	/* binutils >= 2.18-mint-20080209: move.l 28(pc),d0; jmp 0(pc,d0.l) */
			reads += fread(&dummy, sizeof(dummy), 1, fp);	/* skip a_info */
			reads += fread(&a_text, sizeof(a_text), 1, fp);
			a_text = SDL_SwapBE32(a_text);
			reads += fread(&a_data, sizeof(a_data), 1, fp);
			a_data = SDL_SwapBE32(a_data);
			reads += fread(&a_bss, sizeof(a_bss), 1, fp);
			a_bss = SDL_SwapBE32(a_bss);
			reads += fread(&a_syms, sizeof(a_syms), 1, fp);
			a_syms = SDL_SwapBE32(a_syms);
			reads += fread(&a_entry, sizeof(a_entry), 1, fp);
			a_entry = SDL_SwapBE32(a_entry);
			reads += fread(&a_trsize, sizeof(a_trsize), 1, fp);
			a_trsize = SDL_SwapBE32(a_trsize);
			reads += fread(&a_drsize, sizeof(a_drsize), 1, fp);
			a_drsize = SDL_SwapBE32(a_drsize);
			reads += fread(&g_tparel_pos, sizeof(g_tparel_pos), 1, fp);
			g_tparel_pos = SDL_SwapBE32(g_tparel_pos);
			reads += fread(&g_tparel_size, sizeof(g_tparel_size), 1, fp);
			g_tparel_size = SDL_SwapBE32(g_tparel_size);
			reads += fread(&g_stkpos, sizeof(g_stkpos), 1, fp);
			g_stkpos = SDL_SwapBE32(g_stkpos);
			reads += fread(&g_symbol_format, sizeof(g_symbol_format), 1, fp);
			g_symbol_format = SDL_SwapBE32(g_symbol_format);
			if (g_symbol_format == 0)
			{
				tabletype = SYMBOL_FORMAT_GNU;
			}
			if ((a_text + (256 - 28)) != textlen)
				fprintf(stderr, "warning: inconsistent text segment size %08x != %08x\n", textlen, a_text + (256 - 28));
			if (a_data != datalen)
				fprintf(stderr, "warning: inconsistent data segment size %08x != %08x\n", datalen, a_data);
			if (a_bss != bsslen)
				fprintf(stderr, "warning: inconsistent bss segment size %08x != %08x\n", bsslen, a_bss);
			/*
			 * the symbol table size in the GEMDOS header includes the string table,
			 * the symbol table size in the exec header does not.
			 */
			if (tabletype == SYMBOL_FORMAT_GNU)
			{
				strsize = tablesize - a_syms;
				tablesize = a_syms;
				stroff = a_syms;
			}

			textlen = a_text + (256 - 28);
			datalen = a_data;
			bsslen = a_bss;
			symoff = 0x100 + /* sizeof(extended exec header) */
				a_text +
				a_data +
				a_trsize +
				a_drsize;
		}
	}
	if (!symbols_print_prg_info(tabletype, prgflags, relocflag)) {
		return NULL;
	}
	fprintf(stderr, "Program section sizes:\n- text: %d\n- data: %d\n- bss:  %d\n",
		textlen, datalen, bsslen);

	if (!tablesize) {
		fprintf(stderr, "ERROR: symbol table missing from the program!\n");
		return NULL;
	}
	fprintf(stderr, "- syms: %d\n", tablesize);

	/* symbols already have suitable offsets, so only acceptable end position needs to be calculated */
	sections[0].offset = 0;
	sections[0].end = textlen;
	sections[1].offset = textlen;
	sections[1].end = textlen + datalen;
	sections[2].offset = textlen + datalen;
	sections[2].end = textlen + datalen + bsslen;

	if (tabletype == SYMBOL_FORMAT_GNU) {
		/* go to start of symbol table */
		offset = symoff;
		if (fseek(fp, offset, SEEK_SET) < 0) {
			perror("ERROR: seeking to symbol table failed");
			return NULL;
		}
		fprintf(stderr, "Trying to load symbol table at offset 0x%x...\n", offset);
		symbols = symbols_load_gnu(fp, sections, tablesize, stroff, strsize);
	} else {
		/* go to start of symbol table */
		offset = 0x1C + textlen + datalen;
		if (fseek(fp, offset, SEEK_SET) < 0) {
			perror("ERROR: seeking to symbol table failed");
			return NULL;
		}
		fprintf(stderr, "Trying to load symbol table at offset 0x%x...\n", offset);
		symbols = symbols_load_dri(fp, sections, tablesize);
	}
	if (!symbols) {
		fprintf(stderr, "\n\n*** Try with 'nm -n <program>' (Atari/cross-compiler tool) instead ***\n\n");
		return NULL;
	}
	fprintf(stderr, "Load the listed symbols to Hatari debugger with 'symbols <filename> TEXT'.\n");
	return symbols;
}

/**
 * Load symbols of given type and the symbol address addresses from
 * the given file and add given offsets to the addresses.
 * Return symbols list or NULL for failure.
 */
static symbol_list_t* symbols_load(const char *filename)
{
	symbol_list_t *list;
	uint16_t magic;
	FILE *fp;

	fprintf(stderr, "Reading symbols from program '%s' symbol table...\n", filename);
	if (!(fp = fopen(filename, "rb"))) {
		usage("opening program file failed");
	}
	if (fread(&magic, sizeof(magic), 1, fp) != 1) {
		usage("reading program file failed");
	}

	if (SDL_SwapBE16(magic) != ATARI_PROGRAM_MAGIC) {
		usage("file isn't an Atari program file");
	}
	list = symbols_load_binary(fp);
	fclose(fp);

	if (!list) {
		usage("no symbols, or reading them failed");
	}

	if (list->count < list->symbols) {
		if (!list->count) {
			usage("no valid symbols in program, symbol table loading failed");
		}
		/* parsed less than there were "content" lines */
		list->names = realloc(list->names, list->count * sizeof(symbol_t));
		assert(list->names);
	}

	/* copy name list to address list */
	list->addresses = malloc(list->count * sizeof(symbol_t));
	assert(list->addresses);
	memcpy(list->addresses, list->names, list->count * sizeof(symbol_t));

	/* sort both lists, with different criteria */
	qsort(list->addresses, list->count, sizeof(symbol_t), symbols_by_address);
	qsort(list->names, list->count, sizeof(symbol_t), symbols_by_name);

	/* check for duplicate addresses */
	symbols_check_addresses(list->addresses, list->count);

	/* check for duplicate names */
	symbols_check_names(list->names, list->count);

	return list;
}


/* ---------------- symbol showing & option parsing ------------------ */

/**
 * Show symbols sorted by selected option
 */
static int symbols_show(symbol_list_t* list)
{
	symbol_t *entry, *entries;
	char symchar;
	int i;
	
	if (!list) {
		fprintf(stderr, "No symbols!\n");
		return 1;
	}
	if (Options.sort_name) {
		entries = list->names;
	} else {
		entries = list->addresses;
	}
	for (entry = entries, i = 0; i < list->count; i++, entry++) {
		symchar = symbol_char(entry->type);
		fprintf(stdout, "0x%08x %c %s\n",
			entry->address, symchar, entry->name);
	}

	fprintf(stderr, "%d (unignored) symbols processed.\n", list->count);
	return 0;
}

/**
 * parse program options and then call symbol load+save
 */
int main(int argc, const char *argv[])
{
	int i;

	PrgPath = *argv;
	for (i = 1; i+1 < argc; i++) {
		if (argv[i][0] != '-') {
			break;
		}
		switch(tolower((unsigned char)argv[i][1])) {
		case 'a':
			Options.notypes |= SYMTYPE_ABS;
			break;
		case 'b':
			Options.notypes |= SYMTYPE_BSS;
			break;
		case 'd':
			Options.notypes |= SYMTYPE_DATA;
			break;
		case 't':
			Options.notypes |= SYMTYPE_TEXT;
			break;
		case 'l':
			Options.no_local = true;
			break;
		case 'o':
			Options.no_obj = true;
			break;
		case 'n':
			Options.sort_name = true;
			break;
		default:
			usage("unknown option");
		}
	}
	if (i+1 != argc) {
		usage("incorrect number of arguments");
	}
	return symbols_show(symbols_load(argv[i]));
}
