#pragma once

// 本机「粘性拒绝」探测（启动器 / 注入 DLL 共用）。
// 路径约定与 update_client 写入一致；只判断「是否存在有效拒绝缓存」，不解析业务字段。

namespace xcat {

// payloadBinDir：安装侧 XCat_data 目录（可空；空则只查 ProgramData 机级粘性）。
bool AccessDenyStickyPresent(const char* payloadBinDir);

}  // namespace xcat
