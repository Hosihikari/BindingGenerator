// Linux fixture for the symbol dispatch test.
//
// The shared object exports the mangled Itanium names used by the IR fixture, so the
// dispatch shim can resolve them through dlsym when no link-time address is present.

class FixtureBlock
{
public:
    FixtureBlock(int x, int y);
    int add(int value) const;
    virtual int isReady() const;
    virtual int scale(int factor) const;
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

FixtureBlock* FixtureBlock::create(int x, int y)
{
    return new FixtureBlock(x, y);
}

void FixtureBlock::release(FixtureBlock* block)
{
    delete block;
}
