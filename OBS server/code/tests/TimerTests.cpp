#include "EventLoop.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace obs::net;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void oneShotAndCancellation() {
    EventLoop loop;
    int calls = 0;
    auto cancelled = loop.runAfter(1ms, [&] { calls += 100; });
    loop.cancelTimer(cancelled);
    loop.cancelTimer(cancelled);
    TimerId later;
    loop.runAfter(0ns, [&] {
        loop.cancelTimer(later);
        ++calls;
    });
    later = loop.runAfter(0ns, [&] { calls += 1000; });
    loop.runAfter(15ms, [&] {
        ++calls;
        loop.quit();
    });
    loop.loop();
    check(calls == 2 && cancelled->load() && later->load(), "one-shot and same-batch cancel");
}
void repeatingAndNested() {
    EventLoop loop;
    int calls = 0, nested = 0;
    TimerId repeating;
    repeating = loop.runEvery(2ms, [&] {
        if (++calls == 3) {
            loop.cancelTimer(repeating);
            loop.runAfter(1ms, [&] { ++nested; });
        }
    });
    loop.runAfter(35ms, [&] { loop.quit(); });
    loop.loop();
    check(calls == 3 && nested == 1, "repeat self cancel and nested add");
}
void crossThreadAndOverdue() {
    EventLoop loop;
    int calls = 0;
    const auto owner = std::this_thread::get_id();
    // 主线程尚未启动 loop，add 和 cancel 的排队顺序也必须正确处理。
    std::thread producer([&] {
        auto id = loop.runAfter(0ns, [&] { calls += 100; });
        loop.cancelTimer(id);
        loop.runAfter(0ns, [&] {
            check(std::this_thread::get_id() == owner, "timer owner thread");
            ++calls;
            loop.quit();
        });
    });
    producer.join();
    loop.loop();
    check(calls == 1, "cross-thread add/cancel before insertion");
}
void invalidAndException() {
    EventLoop loop;
    bool rejected = false;
    try {
        loop.runEvery(0ns, [] {});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "zero repeat interval rejected");
    rejected = false;
    try {
        loop.runAfter(-1ns, [] {});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "negative delay rejected");
    rejected = false;
    try {
        loop.runAfter(1ms, {});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "empty callback rejected");
    auto id = loop.runAfter(0ns, [] { throw std::runtime_error("expected timer failure"); });
    bool propagated = false;
    try {
        loop.loop();
    } catch (const std::runtime_error &) {
        propagated = true;
    }
    check(propagated && id->load(), "timer exception propagation and handle cleanup");
}
} // namespace
int main() {
    try {
        oneShotAndCancellation();
        repeatingAndNested();
        crossThreadAndOverdue();
        invalidAndException();
        std::cout << "4 timer behavior groups passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
