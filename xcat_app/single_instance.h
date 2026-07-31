#pragma once

#include <Windows.h>

namespace xcat::app {

bool AcquireXcatSingleInstance(DWORD maxWaitMs = 60000);
void ReleaseXcatSingleInstance();

}  // namespace xcat::app
