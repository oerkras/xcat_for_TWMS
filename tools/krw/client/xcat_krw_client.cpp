#include "xcat_krw_client.h"

#include <cstdio>
#include <cstring>

namespace xcat::krw {
namespace {

std::string WinErr(DWORD e) {
  char buf[128]{};
  std::snprintf(buf, sizeof(buf), "win32=%lu", static_cast<unsigned long>(e));
  return buf;
}

}  // namespace

bool Client::Open(std::uint64_t session_key, std::string* err) {
  Close();
  if (session_key == 0) {
    if (err) *err = "session_key must be non-zero";
    return false;
  }

  handle_ = CreateFileW(XCAT_KRW_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE) {
    if (err) *err = std::string("CreateFile ") + WinErr(GetLastError());
    return false;
  }

  alignas(8) unsigned char buf[sizeof(XcatKrwHandshakeIn) > sizeof(XcatKrwHandshakeOut)
                                   ? sizeof(XcatKrwHandshakeIn)
                                   : sizeof(XcatKrwHandshakeOut)]{};
  auto* in = reinterpret_cast<XcatKrwHandshakeIn*>(buf);
  in->session_key = session_key;
  in->client_abi = XCAT_KRW_ABI_VERSION;

  DWORD ret = 0;
  if (!DeviceIo(static_cast<DWORD>(XcatKrwIoctl_Handshake), buf, sizeof(XcatKrwHandshakeIn),
                sizeof(XcatKrwHandshakeOut), &ret, err)) {
    Close();
    return false;
  }

  auto* out = reinterpret_cast<XcatKrwHandshakeOut*>(buf);
  if (out->status != XcatKrwStatus_Ok || out->driver_abi != XCAT_KRW_ABI_VERSION) {
    if (err) *err = "handshake rejected";
    Close();
    return false;
  }

  session_key_ = session_key;
  ready_ = true;
  return true;
}

void Client::Close() {
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
  session_key_ = 0;
  ready_ = false;
}

bool Client::DeviceIo(DWORD code, void* in_out, DWORD in_len, DWORD out_len, DWORD* ret_len,
                      std::string* err) {
  if (handle_ == INVALID_HANDLE_VALUE) {
    if (err) *err = "not open";
    return false;
  }
  DWORD got = 0;
  if (!DeviceIoControl(handle_, code, in_out, in_len, in_out, out_len, &got, nullptr)) {
    if (err) *err = std::string("DeviceIoControl ") + WinErr(GetLastError());
    return false;
  }
  if (ret_len) *ret_len = got;
  return true;
}

bool Client::QueryInfo(XcatKrwQueryInfoOut* out, std::string* err) {
  if (!out) return false;
  XcatKrwQueryInfoOut local{};
  DWORD ret = 0;
  if (!DeviceIo(static_cast<DWORD>(XcatKrwIoctl_QueryInfo), &local, 0, sizeof(local), &ret, err)) {
    return false;
  }
  *out = local;
  return out->status == XcatKrwStatus_Ok;
}

bool Client::Read(std::uint32_t pid, std::uint64_t remote_va, void* dst, std::uint32_t size,
                  std::uint32_t* transferred, std::string* err, bool use_phys) {
  if (!ready_ || !dst || size == 0) {
    if (err) *err = "bad args";
    return false;
  }

  std::vector<std::uint8_t> buf(sizeof(XcatKrwRwOut) + size);
  XcatKrwRwIn req{};
  req.session_key = session_key_;
  req.pid = pid;
  req.size = size;
  req.remote_va = remote_va;
  req.flags = use_phys ? XcatKrwRwFlag_Phys : XcatKrwRwFlag_None;
  std::memcpy(buf.data(), &req, sizeof(req));

  DWORD ret = 0;
  if (!DeviceIo(static_cast<DWORD>(XcatKrwIoctl_Read), buf.data(), sizeof(req),
                static_cast<DWORD>(buf.size()), &ret, err)) {
    return false;
  }

  auto* out = reinterpret_cast<XcatKrwRwOut*>(buf.data());
  if (out->status != XcatKrwStatus_Ok) {
    if (err) *err = "read status=" + std::to_string(out->status);
    return false;
  }
  const std::uint32_t n = out->bytes_transferred;
  if (n > size) {
    if (err) *err = "driver returned oversized transfer";
    return false;
  }
  std::memcpy(dst, buf.data() + sizeof(XcatKrwRwOut), n);
  if (transferred) *transferred = n;
  return true;
}

bool Client::Write(std::uint32_t pid, std::uint64_t remote_va, const void* src, std::uint32_t size,
                   std::uint32_t* transferred, std::string* err, bool use_phys) {
  if (!ready_ || !src || size == 0) {
    if (err) *err = "bad args";
    return false;
  }

  std::vector<std::uint8_t> buf(sizeof(XcatKrwRwIn) + size);
  XcatKrwRwIn req{};
  req.session_key = session_key_;
  req.pid = pid;
  req.size = size;
  req.remote_va = remote_va;
  req.flags = use_phys ? XcatKrwRwFlag_Phys : XcatKrwRwFlag_None;
  std::memcpy(buf.data(), &req, sizeof(req));
  std::memcpy(buf.data() + sizeof(req), src, size);

  DWORD ret = 0;
  if (!DeviceIo(static_cast<DWORD>(XcatKrwIoctl_Write), buf.data(),
                static_cast<DWORD>(buf.size()), sizeof(XcatKrwRwOut), &ret, err)) {
    return false;
  }

  auto* out = reinterpret_cast<XcatKrwRwOut*>(buf.data());
  if (out->status != XcatKrwStatus_Ok) {
    if (err) *err = "write status=" + std::to_string(out->status);
    return false;
  }
  if (transferred) *transferred = out->bytes_transferred;
  return true;
}

bool Client::Inject(std::uint32_t pid, const char* dll_path_acp, std::string* err) {
  if (!ready_ || !dll_path_acp || pid == 0) {
    if (err) *err = "bad args";
    return false;
  }
  const int wlen = MultiByteToWideChar(CP_ACP, 0, dll_path_acp, -1, nullptr, 0);
  if (wlen <= 1) {
    if (err) *err = "bad path";
    return false;
  }
  std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
  MultiByteToWideChar(CP_ACP, 0, dll_path_acp, -1, wpath.data(), wlen);
  const UINT32 path_bytes = static_cast<UINT32>(wlen * sizeof(wchar_t));
  std::vector<std::uint8_t> buf(sizeof(XcatKrwInjectIn) + path_bytes);
  XcatKrwInjectIn in{};
  in.session_key = session_key_;
  in.pid = pid;
  in.path_bytes = path_bytes;
  std::memcpy(buf.data(), &in, sizeof(in));
  std::memcpy(buf.data() + sizeof(in), wpath.data(), path_bytes);

  DWORD ret = 0;
  if (!DeviceIo(static_cast<DWORD>(XcatKrwIoctl_Inject), buf.data(), static_cast<DWORD>(buf.size()),
                sizeof(XcatKrwInjectOut), &ret, err)) {
    return false;
  }
  auto* out = reinterpret_cast<XcatKrwInjectOut*>(buf.data());
  if (out->status != XcatKrwStatus_Ok) {
    if (err) *err = "inject status=" + std::to_string(out->status) + " nt=" + std::to_string(out->ntstatus);
    return false;
  }
  return true;
}

}  // namespace xcat::krw
