// The bits of the C++ runtime a freestanding kernel still needs.
#include <mm/heap.hpp>
#include <klib/panic.hpp>

namespace {

// The compiler assumes a throwing operator new never returns null: it deletes
// null checks after a new-expression and runs the constructor unconditionally.
// With no exceptions to throw, running out of memory here has to be fatal.
// Code that can tolerate failure calls kmalloc and checks the result.
void* allocate_or_panic(usize size) {
    void* pointer = kmalloc(size ? size : 1);
    if (pointer == nullptr) panic("c++: operator new(%lu) out of memory", size);
    return pointer;
}

} // namespace

void* operator new(usize size)                 { return allocate_or_panic(size); }
void* operator new[](usize size)               { return allocate_or_panic(size); }
void  operator delete(void* pointer)  noexcept { kfree(pointer); }
void  operator delete[](void* pointer) noexcept { kfree(pointer); }

// Sized deletes: the compiler knows the object size, but the heap recovers it
// from the page frame, so the size is simply unused.
void operator delete(void* pointer, usize)   noexcept { kfree(pointer); }
void operator delete[](void* pointer, usize) noexcept { kfree(pointer); }

extern "C" void __cxa_pure_virtual() {
    panic("c++: pure virtual function called");
}

// Emitted for function-local statics. Guards are no-ops until there is more
// than one thread of control in the kernel; M2 replaces these.
extern "C" int  __cxa_guard_acquire(u64* guard) { return !*reinterpret_cast<u8*>(guard); }
extern "C" void __cxa_guard_release(u64* guard) { *reinterpret_cast<u8*>(guard) = 1; }
extern "C" void __cxa_guard_abort(u64*) {}
