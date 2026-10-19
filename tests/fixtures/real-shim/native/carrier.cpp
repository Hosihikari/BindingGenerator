#include "include/demo/LinkedCounter.h"

namespace demo {
LinkedCounter::LinkedCounter(int value) : value_(value) {}
LinkedCounter::~LinkedCounter() = default;
int LinkedCounter::value() const { return value_; }
} // namespace demo
