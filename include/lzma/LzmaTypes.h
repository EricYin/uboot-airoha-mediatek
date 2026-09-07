/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Fake include for Types.h
 *
 * Copyright (C) 2007-2009 Industrie Dial Face S.p.A.
 * Luigi 'Comio' Mantellini (luigi.mantellini@idf-hit.com)
 */

#ifndef __TYPES_H__FAKE__
#define __TYPES_H__FAKE__

/*
 * This avoids the collision with zlib.h Byte definition
 */
#define Byte LZByte

/*
 * Prevent system lzma headers (/usr/include/lzma/) from being pulled
 * in when compiling host tools. U-Boot's internal LZMA (lib/lzma/)
 * must take priority over any system-installed LZMA SDK.
 */
#define __LZMA_DEC_H	/* guard for system LzmaDec.h */

#include "../../lib/lzma/7zTypes.h"

#endif
