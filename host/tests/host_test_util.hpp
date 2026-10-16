#pragma once

#include <functional>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

namespace mchost_test {

// 跑事件循环若干毫秒。
// 被测对象靠 QTimer 推进 —— 不进事件循环，它一次都不会动。
// 所以这里不能用 sleep：sleep 期间事件循环不转，定时器根本不触发。
inline void pumpFor(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 轮询等待条件成立，超时返回 false（并把失败原因留给调用方去断言）。
// 不用固定 sleep 是刻意的：固定 sleep 在慢机器上会假红，在快机器上会白等。
inline bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        pumpFor(10);
    }
    return true;
}

}  // namespace mchost_test
