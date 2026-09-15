#pragma once

#include <utility>
#include <type_traits>

namespace QuickView {

// ============================================================================
// Lightweight Type-Erased Async Job (No virtual functions, no STL tuple overhead)
// Eradicates std::thread::_Invoke<std::tuple<...>> template bloat.
// ============================================================================
class AsyncJob {
    void* m_target = nullptr;
    void (*m_invoker)(void*) = nullptr;
    void (*m_deleter)(void*) = nullptr;

public:
    AsyncJob() noexcept = default;

    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, AsyncJob>)
    AsyncJob(F&& f) {
        using DecayF = std::decay_t<F>;
        auto* p = new DecayF(std::forward<F>(f));
        m_target = p;
        m_invoker = [](void* ptr) noexcept {
            (*static_cast<DecayF*>(ptr))();
        };
        m_deleter = [](void* ptr) noexcept {
            delete static_cast<DecayF*>(ptr);
        };
    }

    AsyncJob(AsyncJob&& o) noexcept
        : m_target(std::exchange(o.m_target, nullptr))
        , m_invoker(std::exchange(o.m_invoker, nullptr))
        , m_deleter(std::exchange(o.m_deleter, nullptr)) {}

    AsyncJob& operator=(AsyncJob&& o) noexcept {
        if (this != &o) {
            Reset();
            m_target = std::exchange(o.m_target, nullptr);
            m_invoker = std::exchange(o.m_invoker, nullptr);
            m_deleter = std::exchange(o.m_deleter, nullptr);
        }
        return *this;
    }

    AsyncJob(const AsyncJob&) = delete;
    AsyncJob& operator=(const AsyncJob&) = delete;

    ~AsyncJob() {
        Reset();
    }

    void RunAndDestroy() noexcept {
        if (m_invoker && m_target) {
            m_invoker(m_target);
        }
        Reset();
    }

    void Reset() noexcept {
        if (m_deleter && m_target) {
            m_deleter(m_target);
            m_target = nullptr;
            m_invoker = nullptr;
            m_deleter = nullptr;
        }
    }

    explicit operator bool() const noexcept {
        return m_target != nullptr;
    }
};

// Fire-and-forget OS thread dispatch (single native entrypoint across whole binary)
void RunDetached(AsyncJob job);

// Fire-and-forget Windows kernel threadpool dispatch (near-zero latency, reuses threads)
void PostThreadPool(AsyncJob job);

} // namespace QuickView
