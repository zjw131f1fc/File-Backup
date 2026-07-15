#pragma once

namespace backup {

enum class EntryType {
    REGULAR_FILE,
    DIRECTORY,
    SYMBOLIC_LINK,
    HARD_LINK,
    FIFO,
    CHARACTER_DEVICE,
    BLOCK_DEVICE,
};

}  // namespace backup
