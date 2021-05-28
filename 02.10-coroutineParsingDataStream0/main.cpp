// Copyright (c) Andreas Fertig.
// SPDX-License-Identifier: MIT

#include <cassert>

#if __has_include(<experimental/coroutine>)
#include <experimental/coroutine>
namespace std { using namespace std::experimental; }
#elif __has_include(<coroutine>)

#include <coroutine>

#endif

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <version>

using std::byte;

std::byte operator ""_B(char c) {
    return static_cast<byte>(c);
}

std::byte operator ""_B(unsigned long long c) {
    return static_cast<byte>(c);
}


namespace coro_iterator {
    template<typename PT>
    struct iterator {
        using coro_handle = std::coroutine_handle<PT>;

        coro_handle mCoroHdl{nullptr};
        bool mDone{true};
        using RetType = decltype(mCoroHdl.promise().mValue);
        void resume() {
            mCoroHdl.resume();
            mDone = mCoroHdl.done();
        }
        iterator() = default;
        iterator(coro_handle hco) : mCoroHdl{hco} {
            resume();
        }
        iterator &operator++() {
            resume();
            return *this;
        }
        bool operator==(const iterator &o) const { return mDone == o.mDone; }
        const RetType &operator*() const { return mCoroHdl.promise().mValue; }
        const RetType *operator->() const { return &(operator*()); }
    };
}  // namespace coro_iterator


template<typename T, typename G, typename... Bases>  // #A Allow multiple bases for awaiter
struct promise_type_base : public Bases ... {
    T mValue;

    auto yield_value(T value) {
        mValue = value;
        return std::suspend_always{};
    }
    G get_return_object() { return G{this}; };
    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
};

// coroutine 외부에서 데이터를 기다리기 위해 co_await
// 이 예제에서 byte를 기다린다.
// compiler to look for either an
// - operator co_await(byte) or for
// - await_transform(byte)
// the Awaitable-part needs to store the awaited data
template<typename T>
struct awaitable_promise_type_base {
    std::optional<T> mRecentSignal;

    struct awaiter {
        std::optional<T> &mRecentSignal;

        bool await_ready() { return mRecentSignal.has_value(); }
        void await_suspend(std::coroutine_handle<>) {}
        T await_resume() {
            assert(mRecentSignal.has_value());
            auto tmp = *mRecentSignal;
            mRecentSignal.reset();
            return tmp;
        }
    };

    // Awaitbale type이 되기 위해서 await_transform를 구현해야 된다.
    // This method takes a type as a single argument and returns an Awaiter
    [[nodiscard]] awaiter await_transform(T) {
        return awaiter{mRecentSignal};
    }
};

// <output, input>
template<typename T, typename U>
struct [[nodiscard]] async_generator {
    using promise_type = promise_type_base<T, async_generator, awaitable_promise_type_base<U>>;
    using PromiseTypeHandle = std::coroutine_handle<promise_type>;

    T operator()() {
        // #A the move also clears the mValue of the promise
        auto tmp{std::move(mCoroHdl.promise().mValue)};

        // #B but we have to set it to a defined state
        mCoroHdl.promise().mValue.clear();

        return tmp;
    }

    void SendSignal(U signal) {
        mCoroHdl.promise().mRecentSignal = signal;
        if (not mCoroHdl.done()) {
            mCoroHdl.resume();
        }
    }

    async_generator(async_generator const &) = delete;
    async_generator(async_generator &&rhs) : mCoroHdl{std::exchange(rhs.mCoroHdl, nullptr)} {}
    ~async_generator() {
        if (mCoroHdl) { mCoroHdl.destroy(); }
    }

private:
    friend promise_type;  // #C As the default ctor is private G needs to be a friend
    explicit async_generator(promise_type *p) : mCoroHdl(PromiseTypeHandle::from_promise(*p)) {}
    PromiseTypeHandle mCoroHdl;
};


template<typename T>
struct generator {
    using promise_type = promise_type_base<T, generator>;
    using PromiseTypeHandle = std::coroutine_handle<promise_type>;
    using iterator = coro_iterator::iterator<promise_type>;

    iterator begin() { return {mCoroHdl}; }
    iterator end() { return {}; }

    generator(generator const &) = delete;
    generator(generator &&rhs) : mCoroHdl(rhs.mCoroHdl) {
        rhs.mCoroHdl = nullptr;
    }
    ~generator() {
        if (mCoroHdl) { mCoroHdl.destroy(); }
    }

private:
    friend promise_type;  // #A As the default ctor is private G needs to be a friend
    explicit generator(promise_type *p) : mCoroHdl(PromiseTypeHandle::from_promise(*p)) {}
    PromiseTypeHandle mCoroHdl;
};


using FSM = async_generator<std::string, byte>;


static const byte ESC{'H'};
static const byte SOF{0x10};


FSM Parse() {
    while (true) {
        byte b = co_await byte{};
        std::string frame{};

        if (ESC == b) {
            b = co_await byte{};

            if (SOF != b) { continue; }  // #A  not looking at a start sequence

            while (true) {  // #B capture the full frame
                b = co_await byte{};

                if (ESC == b) {
                    // #C skip this byte and look at the next one
                    b = co_await byte{};

                    if (SOF == b) {
                        co_yield frame;
                        break;
                    } else if (ESC != b) {  // #D out of sync
                        break;
                    }
                }

                frame += static_cast<char>(b);
            }
        }
    }
}


generator<byte> sender(std::vector<byte> fakeBytes) {
    for (const auto &b : fakeBytes) {
        co_yield b;
    }
}


void HandleFrame(const std::string &frame);

void ProcessStream(generator<byte> &stream, FSM &parse) {
    for (const auto &b : stream) {
        parse.SendSignal(b);  // #A Send the new byte to the waiting Parse coroutine

        // #B Check whether we have a complete frame
        if (const auto &res = parse(); res.length()) {
            HandleFrame(res);
        }
    }
}


void HandleFrame(const std::string &frame) {
    printf("%s\n", frame.c_str());
}

int main() {
    auto p = Parse();  // #B Create the Parse coroutine and store the handle in p

    std::vector<byte> fakeBytes1{0x70_B, ESC, SOF, ESC,
                                 'H'_B, 'e'_B, 'l'_B, 'l'_B, 'o'_B,
                                 ESC, SOF, 0x7_B, ESC, SOF};
    auto stream1 = sender(std::move(fakeBytes1));  // #A Simulate the first network stream
    ProcessStream(stream1, p);  // #C Process the bytes

    // #D Simulate the reopening of the network stream
    std::vector<byte> fakeBytes2{'W'_B, 'o'_B, 'r'_B, 'l'_B, 'd'_B, ESC, SOF, 0x99_B};
    auto stream2 = sender(std::move(fakeBytes2));  // #E Simulate a second network stream
    ProcessStream(stream2, p);  // #F We still use the former p and feed it with new bytes

}