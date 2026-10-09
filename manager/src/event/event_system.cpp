#include "event_system.h"
#include "../log/logger.h"
#include <algorithm>
#include <mutex>
#include <windows.h>

namespace eam {

namespace {

// Kept apart from anything that owns C++ objects: a function with a __try block cannot also have destructors to run.
bool CallGuarded(EventCallback callback, uint32_t eventId, const void* data, uint32_t size, void* userData) {
    __try {
        callback(eventId, data, size, userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

thread_local int t_publishing = 0;   // the publishes this thread is inside (a callback that unsubscribes must not wait for its own publish)

} // namespace

void EventBus::WaitForPublishes() const {
    if (t_publishing > 0) return;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (m_inFlight.load() > 0 && GetTickCount64() < deadline) Sleep(1);
}

EventBus& EventBus::Instance() {
    static EventBus bus;
    return bus;
}

void EventBus::Subscribe(uint32_t eventId, EventCallback callback, void* userData) {
    if (!callback) return;
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_subscribers[eventId].push_back({ callback, userData });
}

void EventBus::Unsubscribe(uint32_t eventId, EventCallback callback) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const auto list = m_subscribers.find(eventId);
    if (list == m_subscribers.end()) return;
    auto& subs = list->second;
    subs.erase(std::remove_if(subs.begin(), subs.end(), [callback](const Subscriber& s) { return s.callback == callback; }), subs.end());
    lock.unlock();
    WaitForPublishes();
}

void EventBus::Publish(uint32_t eventId, const void* data, uint32_t dataSize) {
    std::vector<Subscriber> subscribers;
    struct InFlight {   // counted from before the list is copied until the last callback returns
        const EventBus& bus;
        explicit InFlight(const EventBus& b) : bus(b) { ++bus.m_inFlight; ++t_publishing; }
        ~InFlight() { --bus.m_inFlight; --t_publishing; }
    } inFlight(*this);
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        const auto list = m_subscribers.find(eventId);
        if (list == m_subscribers.end()) return;
        subscribers = list->second;
    }
    for (const Subscriber& s : subscribers)
        if (!CallGuarded(s.callback, eventId, data, dataSize, s.userData))
            LOG_ERROR("EventBus", "A subscriber to event %u faulted and was skipped", (unsigned)eventId);
}

size_t EventBus::ForgetCode(uintptr_t begin, uintptr_t end) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    size_t removed = 0;
    for (auto& [id, subs] : m_subscribers) {
        const size_t before = subs.size();
        subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Subscriber& s) {
            const uintptr_t at = reinterpret_cast<uintptr_t>(s.callback);
            return at >= begin && at < end;
        }), subs.end());
        removed += before - subs.size();
    }
    lock.unlock();
    if (removed) WaitForPublishes();   // a publish that copied the list before this may still be inside the code being forgotten
    return removed;
}

size_t EventBus::SubscriberCount(uint32_t eventId) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto list = m_subscribers.find(eventId);
    return list == m_subscribers.end() ? 0 : list->second.size();
}

} // namespace eam
