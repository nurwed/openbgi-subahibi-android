/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/sjis.c - the CP932 table (src/os/posix/sjis_table.c) against the
 *                iconv code it replaces (src/os/posix/sjis.c)
 *
 * Both implementations are linked into this binary: the ordinary sjis.c
 * (iconv) and the generated table, whose SjisTable_* entry points are not
 * used by the iconv build.  Every two-byte Shift-JIS code is converted both
 * ways and every BMP code point is encoded both ways; the summary counts
 * the outcomes and the test fails when the two disagree about a mapping
 * that both know.
 *
 * The table comes from the Microsoft code page 932 and iconv's CP932 does
 * not have all of it (the NEC row 13 and the IBM extension rows), so
 * "table only" is the expected kind of difference: the table has a mapping,
 * iconv returns U+FFFD (decoding) or '*' (encoding).  "iconv only" and
 * "conflict" must be zero: those would mean the table lost or changed a
 * code the desktop build converts today.
 */
#include "bgi/os.h"
#include "bgi/os_posix.h"

#include <stdio.h>

// the generated table (sjis_table.c); declared here so that the iconv
// build's sjis.c and the table can be compared in one binary
uint32_t SjisTable_ToUnicode(uint16_t code);
int SjisTable_FromUnicode(uint32_t cp, char out[2]);
void OsPosix_SjisInit(void); // src/os/posix/posix_internal.h, internal

static int gFails;

// the Shift-JIS bytes of a conversion as one number: 0 = no code, 0x00NN =
// one byte, 0xNNNN = two bytes
static int CodeOf(int n, const char* b)
{
	if(n == 0)
		return 0;
	if(n == 1)
		return (unsigned char)b[0];
	return ((unsigned char)b[0] << 8) | (unsigned char)b[1];
}

int main(void)
{
	int lead, trail;
	uint32_t cp;
	int same = 0, tableOnly = 0, iconvOnly = 0, conflict = 0;

	OsPosix_SjisInit();

	// every two-byte code: CP932 -> Unicode
	for(lead = 0x81; lead <= 0xfc; lead++)
	{
		if(lead > 0x9f && lead < 0xe0)
			continue;
		for(trail = 0x40; trail <= 0xfc; trail++)
		{
			uint16_t code = (uint16_t)((lead << 8) | trail);
			uint32_t a, b;
			if(trail == 0x7f)
				continue; // not a trail byte
			a = OS_SjisToCodePoint(code); // iconv (U+FFFD when unmapped)
			b = SjisTable_ToUnicode(code); // the table (U+FFFD when unmapped)
			if(a == b)
				same++;
			else if(a == 0xfffd)
				tableOnly++;
			else if(b == 0xfffd)
			{
				if(iconvOnly < 20)
					printf("  iconv only: %04x -> U+%04x, table has none\n", code, a);
				iconvOnly++;
			}
			else
			{
				if(conflict < 20)
					printf("  conflict: %04x -> iconv U+%04x, table U+%04x\n", code, a, b);
				conflict++;
			}
		}
	}
	printf("sjis: %d two-byte codes: %d identical, %d table only, %d iconv only, %d conflict\n",
		same + tableOnly + iconvOnly + conflict, same, tableOnly, iconvOnly, conflict);
	if(iconvOnly || conflict)
		gFails++;

	// every BMP code point: Unicode -> CP932
	same = tableOnly = iconvOnly = conflict = 0;
	for(cp = 0; cp < 0x10000; cp++)
	{
		char a[2] = {0, 0}, b[2] = {0, 0};
		int na, nb, ca, cb;
		if(cp >= 0xd800 && cp <= 0xdfff)
			continue; // surrogates have no encoding
		na = OsPosix_UnicodeToSjis(cp, a);
		nb = SjisTable_FromUnicode(cp, b);
		// iconv substitutes '*' for an unencodable code point; that is
		// "no mapping", not a mapping to U+002A (which is ASCII and
		// handled before the converter)
		if(na == 1 && (unsigned char)a[0] == '*')
			na = 0;
		ca = CodeOf(na, a);
		cb = CodeOf(nb, b);
		if(ca == cb)
			same++;
		else if(ca == 0)
			tableOnly++;
		else if(cb == 0)
		{
			if(iconvOnly < 20)
				printf("  iconv only: U+%04x -> %04x, table has none\n", cp, ca);
			iconvOnly++;
		}
		else
		{
			if(conflict < 20)
				printf("  conflict: U+%04x -> iconv %04x, table %04x\n", cp, ca, cb);
			conflict++;
		}
	}
	printf("sjis: %d code points: %d identical, %d table only, %d iconv only, %d conflict\n",
		same + tableOnly + iconvOnly + conflict, same, tableOnly, iconvOnly, conflict);
	if(iconvOnly || conflict)
		gFails++;

	if(gFails)
	{
		printf("sjis: FAILED\n");
		return 1;
	}
	printf("sjis: OK (every mapping iconv knows is the same)\n");
	return 0;
}
