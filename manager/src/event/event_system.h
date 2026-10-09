#pragma once
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace eam {

using EventCallback = void(*)(uint32_t eventId, const void* data, uint32_t dataSize, void* userData);

// The publish and subscribe channel between the host and its addons. Subscribers to one event are called in the order they subscribed, on
// the thread that publishes. A subscriber that faults is skipped and logged, and never stops the ones after it. Publishing works from a copy
// of the subscriber list, so a subscriber may subscribe or unsubscribe (itself included) from inside its own callback. Unsubscribe and ForgetCode wait (at most
// two seconds) for the publishes already running on other threads before they return, so that the callback and its userData can be freed, or the DLL unloaded, as
// soon as they return; called from inside a callback they do not wait (that publish is their own).
class EventBus {
public:
    static EventBus& Instance();

    void Subscribe(uint32_t eventId, EventCallback callback, void* userData = nullptr);   // a null callback is ignored
    void Unsubscribe(uint32_t eventId, EventCallback callback);   // removes every subscription of that callback to the event
    void Publish(uint32_t eventId, const void* data = nullptr, uint32_t dataSize = 0);

    // Removes every subscription whose callback lies in [begin, end): an addon's DLL that is being unloaded. Returns how many.
    size_t ForgetCode(uintptr_t begin, uintptr_t end);
    size_t SubscriberCount(uint32_t eventId) const;   // for tests

private:
    EventBus() = default;

    struct Subscriber {
        EventCallback callback;
        void* userData;
    };

    void WaitForPublishes() const;   // the publishes in flight on other threads, up to two seconds

    mutable std::shared_mutex m_mutex;
    mutable std::atomic<int> m_inFlight{ 0 };
    std::unordered_map<uint32_t, std::vector<Subscriber>> m_subscribers;
};

} // namespace eam
