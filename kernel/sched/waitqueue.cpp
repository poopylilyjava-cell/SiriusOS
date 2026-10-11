#include <sched/waitqueue.hpp>
#include <sched/sched.hpp>

namespace sched {

void WaitQueue::wait(Spinlock& lock, u64 flags) {
    Thread* self = current();
    self->next = nullptr;
    if (m_tail) m_tail->next = self;
    else        m_head = self;
    m_tail = self;
    block_current(lock, flags);
}

void WaitQueue::wake_all() {
    Thread* thread = m_head;
    m_head = m_tail = nullptr;
    while (thread) {
        Thread* next = thread->next;
        thread->next = nullptr;
        wake(thread);
        thread = next;
    }
}

} // namespace sched
