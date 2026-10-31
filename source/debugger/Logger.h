/**
 *
 * Copyright (c) 2014-2026 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#pragma once
// DEXED_HEADLESS: the engine library builds without JUCE, so no memory logger and no traces.
#ifndef DEXED_HEADLESS
#include <debugger/MemoryLogger.h>
#endif

void dexed_trace(const char *source, const char *fmt, ...);

#define __QUOTE(name) #name
#define __STR(macro) __QUOTE(macro)

#ifdef DEBUG
    #define DEXED_VERSION __STR(DEXED_ID) " DEBUG"
#else
    #define DEXED_VERSION __STR(DEXED_ID)
#endif

#if defined(DEBUG) && !defined(DEXED_HEADLESS)
    #ifdef _MSC_VER
        #define TRACE(fmt, ...) dexed_trace(__FUNCTION__,fmt,##__VA_ARGS__)
    #else
        #define TRACE(fmt, ...) dexed_trace(__PRETTY_FUNCTION__,fmt,##__VA_ARGS__)
    #endif
#else
    #define TRACE(fmt, ...)
#endif

#ifndef DEXED_HEADLESS
#define DXLOG(fmt, ...) MemoryLogger::getInstance()->logFormat(fmt,##__VA_ARGS__)
#endif