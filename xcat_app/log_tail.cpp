#include "log_tail.h"

#include "../common/xcat_log.h"

namespace xcat::app {

std::vector<std::string> ReadLogTail(const char* path, size_t maxLines, size_t readBytes) {
    return xcat::log::ReadTailLines(path, maxLines, readBytes);
}

}  // namespace xcat::app
