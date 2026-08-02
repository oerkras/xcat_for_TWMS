#pragma once

// Security dig: reversible GameAssembly .text dirty probe (default OFF).
// Enable: env GA_TEXT_PROBE=1  or  state/ga_text_probe.enable starts with '1'
// Optional MemoryCrc extinguish (only after kill evidence): GA_TEXT_PROBE_CRC=1
//   or state/ga_text_probe_crc.enable
// Logs: XCat_data/logs/ga_text_probe.log
// Status: XCat_data/state/ga_text_probe_status.txt

namespace x {
namespace features {
namespace ga_text_probe {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

bool IsEnabled();
bool IsRunning();

}  // namespace ga_text_probe
}  // namespace features
}  // namespace x
