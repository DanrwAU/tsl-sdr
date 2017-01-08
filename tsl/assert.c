/*
 *  assert.c - Assertion helpers/handlers for the TSL
 *
 *  Copyright (c)2017 Phil Vachon <phil@security-embedded.com>
 *
 *  This file is a part of The Standard Library (TSL)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <tsl/assert.h>

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <execinfo.h>

#define WARN_ON_BACKTRACE_LEN           6

void __tsl_do_warn(int line_no, const char *filename, const char *msg, ...)
{
    va_list ap;
    size_t bt_len = 0;
    void *bt_symbols[WARN_ON_BACKTRACE_LEN];

    TSL_BUG_ON(NULL == filename);
    TSL_BUG_ON(NULL == msg);

    bt_len = backtrace(bt_symbols, BL_ARRAY_ENTRIES(bt_symbols));

    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fprintf(stderr, " (%s:%d)\n", filename, line_no);

    backtrace_symbols_fd(bt_symbols, bt_len, STDERR_FILENO);

    fprintf(stderr, "-----8<----- Cut Here -----8<-----\n");
}

