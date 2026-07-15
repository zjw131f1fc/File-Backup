#pragma once

namespace backup {

enum class Status {
    SUCCESS,
    FAILED,
    PARTIAL_SUCCESS,
    CANCELLED,
};

}  // namespace backup
