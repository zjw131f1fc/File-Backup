#include "modules/filter/filter.h"

namespace backup {

class StubFilter : public IFilter {
public:
    explicit StubFilter(const FilterRules& rules) : rules_(rules) {}

    bool should_include(const EntryInfo& entry) override {
        (void)entry;
        // 桩实现：默认包含所有条目
        return true;
    }

private:
    FilterRules rules_;
};

std::unique_ptr<IFilter> create_filter(const FilterRules& rules) {
    return std::make_unique<StubFilter>(rules);
}

}  // namespace backup
