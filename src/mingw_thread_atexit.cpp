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

/*
 * __cxa_thread_atexit for MinGW-w64 with POSIX threads (compiled only there, see CMakeLists.txt).
 *
 * GCC calls __cxa_thread_atexit to register the destructor of a thread_local object. MinGW-w64 runs these destructors
 * in a TLS callback when the thread ends, but GCC stores thread_local objects in emulated TLS (emutls), whose memory is
 * freed before that by the destructor of a key of winpthreads. The destructors then access freed memory, which
 * corrupts the heap as soon as several threads end at the same time. This replacement runs the destructors of a thread
 * (other than the main thread) from the destructor of a key of winpthreads that is created after the key of emutls;
 * winpthreads calls the destructors of the keys from the last created key to the first one, so the objects still
 * exist then. The destructors registered by the main thread are passed to MinGW-w64, which runs them when the program
 * exits.
 */

// MinGW only (the build adds this file only for MinGW)
#ifdef __MINGW32__

#include <pthread.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

// MinGW-w64's implementation (in libmingw32)
extern "C" int __mingw_cxa_thread_atexit(void (*function)(void*), void* object, void* dso);

namespace {

// a registered destructor; the destructors of a thread form a list, the last registered one first
struct thread_destructor_t {
    void (*function)(void*); // the destructor
    void* object; // the object it destroys
    thread_destructor_t* next; // the destructor registered before it
};

// the key whose value is the list of destructors of a thread
pthread_key_t destructors_key;
// true <=> destructors_key has been created
bool has_destructors_key = false;
// the id of the main thread
DWORD main_thread_id = 0;

// runs and frees a list of destructors (the destructor of destructors_key)
void run_destructors(void* list)
{
    for (auto* entry = static_cast<thread_destructor_t*>(list); entry != nullptr;) {
        entry->function(entry->object);
        thread_destructor_t* next = entry->next;
        std::free(entry);
        entry = next;
    }
}

// a thread_local object whose first access creates the key of emutls
thread_local volatile int emutls_key_creator = 0;

// creates the key of emutls and then destructors_key, before any other initialization of the program
__attribute__((constructor(101))) void create_destructors_key()
{
    main_thread_id = GetCurrentThreadId();
    emutls_key_creator = 1;
    has_destructors_key = pthread_key_create(&destructors_key, run_destructors) == 0;
}

} // namespace

extern "C" int __cxa_thread_atexit(void (*function)(void*), void* object, void* dso)
{
    if (!has_destructors_key || GetCurrentThreadId() == main_thread_id)
        return __mingw_cxa_thread_atexit(function, object, dso);

    auto* entry = static_cast<thread_destructor_t*>(std::malloc(sizeof(thread_destructor_t)));
    if (entry == nullptr) return -1;
    *entry = { function, object, static_cast<thread_destructor_t*>(pthread_getspecific(destructors_key)) };
    return pthread_setspecific(destructors_key, entry);
}

#endif // __MINGW32__
