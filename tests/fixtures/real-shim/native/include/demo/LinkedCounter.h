#pragma once

namespace demo {
class LinkedCounter {
public:
    explicit LinkedCounter(int value);
    ~LinkedCounter();

    [[nodiscard]] int value() const;

private:
    int value_;
};
} // namespace demo
