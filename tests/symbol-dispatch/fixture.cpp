// Native fixture for the symbol dispatch smoke test.
//
// The DLL is built with a PDB so the symbol frontend can recover RVAs and vtable
// slots without any headers. Only the exported behaviour is stable; the compiler
// is free to lay out the class as it sees fit.

#include <string>
#include <vector>

struct FixturePair
{
    int x;
    int y;

    FixturePair(int x_, int y_);
    FixturePair(FixturePair const& other);
};

FixturePair::FixturePair(int x_, int y_) : x(x_), y(y_) {}

FixturePair::FixturePair(FixturePair const& other) : x(other.x), y(other.y) {}

class FixtureBlock
{
public:
    FixtureBlock(int x, int y);
    int add(int value) const;
    virtual int isReady() const;
    virtual int scale(int factor) const;
    std::string label() const;
    std::vector<int> numbers() const;
    std::vector<std::string> names() const;
    static std::vector<int> squares(int count);
    int countNames(std::vector<std::string> const& values) const;
    int sumValues(std::vector<int> const& values) const;
    FixturePair pair() const;
    int sumPair(FixturePair value) const;
    static FixtureBlock* create(int x, int y);
    static void release(FixtureBlock* block);

private:
    int x_;
    int y_;
};

FixtureBlock::FixtureBlock(int x, int y) : x_(x), y_(y) {}

int FixtureBlock::add(int value) const
{
    return x_ + y_ + value;
}

int FixtureBlock::isReady() const
{
    return 1;
}

int FixtureBlock::scale(int factor) const
{
    return (x_ + y_) * factor;
}

std::string FixtureBlock::label() const
{
    return std::to_string(x_ + y_);
}

std::vector<int> FixtureBlock::numbers() const
{
    return {x_, y_};
}

std::vector<std::string> FixtureBlock::names() const
{
    return {std::to_string(x_), std::to_string(y_)};
}

std::vector<int> FixtureBlock::squares(int count)
{
    std::vector<int> result;
    for (int i = 0; i < count; ++i)
    {
        result.push_back(i * i);
    }

    return result;
}

int FixtureBlock::countNames(std::vector<std::string> const& values) const
{
    return static_cast<int>(values.size()) + x_;
}

int FixtureBlock::sumValues(std::vector<int> const& values) const
{
    int total = 0;
    for (int value : values)
    {
        total += value;
    }

    return total + y_;
}

FixturePair FixtureBlock::pair() const
{
    return FixturePair(x_, y_);
}

int FixtureBlock::sumPair(FixturePair value) const
{
    return value.x + value.y + x_;
}

FixtureBlock* FixtureBlock::create(int x, int y)
{
    return new FixtureBlock(x, y);
}

void FixtureBlock::release(FixtureBlock* block)
{
    delete block;
}
