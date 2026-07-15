#pragma once

#include "common/filter_rules.h"
#include "common/entry_info.h"
#include <memory>

namespace backup {

class IFilter {
public:
    virtual ~IFilter() = default;

    // 判断某个条目是否应该被包含（FilterRules 在工厂函数时烘焙）
    virtual bool should_include(const EntryInfo& entry) = 0;
};

// 工厂函数：创建筛选器，FilterRules 烘焙进实例
std::unique_ptr<IFilter> create_filter(const FilterRules& rules);

}  // namespace backup
