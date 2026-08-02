#pragma once

#include "../include/xcat_krw_abi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xcat::krw {

class Client {
 public:
  Client() = default;
  ~Client() { Close(); }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Open \\.\XCatKrw and handshake with session_key (!= 0).
  bool Open(std::uint64_t session_key, std::string* err = nullptr);

  void Close();

  bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE && ready_; }

  bool QueryInfo(XcatKrwQueryInfoOut* out, std::string* err = nullptr);

  // use_phys=true -> XcatKrwRwFlag_Phys (CR3 walk + MmMapIoSpace).
  bool Read(std::uint32_t pid, std::uint64_t remote_va, void* dst, std::uint32_t size,
            std::uint32_t* transferred = nullptr, std::string* err = nullptr,
            bool use_phys = false);

  bool Write(std::uint32_t pid, std::uint64_t remote_va, const void* src, std::uint32_t size,
             std::uint32_t* transferred = nullptr, std::string* err = nullptr,
             bool use_phys = false);

  // Inject DLL by absolute path (UTF-8 / ACP) into target pid.
  bool Inject(std::uint32_t pid, const char* dll_path_acp, std::string* err = nullptr);

 private:
  bool DeviceIo(DWORD code, void* in_out, DWORD in_len, DWORD out_len, DWORD* ret_len,
                std::string* err);

  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::uint64_t session_key_ = 0;
  bool ready_ = false;
};

}  // namespace xcat::krw
