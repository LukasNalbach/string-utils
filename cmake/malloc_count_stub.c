/**
 * part of LukasNalbach/string-utils
 *
 * MIT License
 *
 * Copyright (c) Lukas Nalbach
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// replacement of external/malloc_count/malloc_count.c that does not count (<prefix>_USE_MALLOC_COUNT=OFF, or Windows
// with the dynamic C runtime): it does not override malloc and free, and all functions return 0

#include <malloc_count.h>

extern size_t malloc_count_current(void)
{
    return 0;
}

extern size_t malloc_count_peak(void)
{
    return 0;
}

extern void malloc_count_reset_peak(void)
{
}

extern size_t malloc_count_num_allocs(void)
{
    return 0;
}

void malloc_count_set_callback(malloc_count_callback_type cb, void* cookie)
{
    (void)cb;
    (void)cookie;
}

extern void malloc_count_print_status(void)
{
}
