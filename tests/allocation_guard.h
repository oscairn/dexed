/*
 * Allocation guard for tests: counts any operator new on a thread while forbidAllocation is
 * set. Include it in exactly one translation unit of a test executable (it replaces the
 * global operators new and delete).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * SPDX-License-Identifier: MIT
 * Test helper, MIT licensed so any project can reuse it (docs/oscairn-plan.md, section 10).
 */
#pragma once

#include <atomic>
#include <cstdlib>
#include <new>
#include <unistd.h>

static thread_local bool forbidAllocation = false;
static std::atomic<int> forbiddenAllocations { 0 };

static void *allocate(size_t size) {
    if (forbidAllocation) {
        forbiddenAllocations++;
        static const char message[] = "allocation while processing\n";
        (void)!write(2, message, sizeof message - 1);
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new(size_t size) { return allocate(size); }
void *operator new[](size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

