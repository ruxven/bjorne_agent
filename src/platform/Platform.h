#pragma once

// Small platform helpers for the parts of the process and socket interface
// that differ between POSIX and Windows. The agent runs on Linux and on Windows
// hosts, so the rest of the code spells these operations through the helpers
// here instead of reaching for the POSIX spellings directly.

#include <cstddef>
#include <string>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h has to come before windows.h. WIN32_LEAN_AND_MEAN stops
// windows.h from pulling in the older winsock.h, which would clash with it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace bjorne_agent {
namespace platform {

// A connected or listening socket. Windows uses a pointer sized handle, so the
// POSIX int would truncate it on 64 bit builds.
using SocketHandle = SOCKET;

// The value a failed socket call returns.
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

// The length argument of getsockname and friends. Windows takes an int; POSIX
// takes its own socklen_t.
using SocketLength = int;

// Winsock needs a process wide startup before the first socket call. The first
// call starts it once; later calls reuse the result. A build that fails to
// start reports it, and the caller turns that into a connection error.
inline bool initializeSockets() {
  static const bool ready = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

inline bool isValidSocket(SocketHandle handle) {
  return handle != kInvalidSocket;
}

// Reads a variable. A variable that is unset and one that holds an empty value
// both report false, which keeps the two platforms in step.
inline bool getEnvironmentVariable(const std::string& name, std::string& out) {
  const DWORD size = ::GetEnvironmentVariableA(name.c_str(), nullptr, 0);
  if (size == 0) {
    return false;
  }
  std::string buffer(size, '\0');
  const DWORD written = ::GetEnvironmentVariableA(name.c_str(), buffer.data(), size);
  if (written == 0 || written >= size) {
    return false;
  }
  out.assign(buffer.data(), written);
  return true;
}

inline void setEnvironmentVariable(const std::string& name, const std::string& value) {
  ::SetEnvironmentVariableA(name.c_str(), value.c_str());
}

inline void unsetEnvironmentVariable(const std::string& name) {
  // The Win32 API spells removal as setting the variable to a null value.
  ::SetEnvironmentVariableA(name.c_str(), nullptr);
}

inline int processId() {
  return static_cast<int>(::GetCurrentProcessId());
}

inline int closeSocket(SocketHandle handle) {
  return ::closesocket(handle);
}

// Text to binary address conversion. Windows spells the call InetPtonA, which
// MinGW aliases to inet_pton, and MSVC does not declare inet_pton at all.
inline int addressToBinary(int family, const char* text, void* address) {
  return ::InetPtonA(family, text, address);
}

// Binary to text address conversion, the reverse of addressToBinary.
inline const char* addressToText(int family, const void* address, char* buffer, std::size_t size) {
  return ::InetNtopA(family, address, buffer, static_cast<DWORD>(size));
}

// The message for a failed getaddrinfo. Windows only declares the ANSI variant
// under its own name; the plain gai_strerror is a MinGW and POSIX spelling.
inline std::string addressErrorMessage(int code) {
  const char* text = ::gai_strerrorA(code);
  return text == nullptr ? std::string("unknown error") : std::string(text);
}

}  // namespace platform
}  // namespace bjorne_agent

#else

#include <arpa/inet.h>
#include <cstdlib>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace bjorne_agent {
namespace platform {

using SocketHandle = int;

constexpr SocketHandle kInvalidSocket = -1;

using SocketLength = socklen_t;

inline bool initializeSockets() {
  return true;
}

inline bool isValidSocket(SocketHandle handle) {
  return handle != kInvalidSocket;
}

// Reads a variable. A variable that is unset and one that holds an empty value
// both report false, which keeps the two platforms in step.
inline bool getEnvironmentVariable(const std::string& name, std::string& out) {
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  out = value;
  return true;
}

inline void setEnvironmentVariable(const std::string& name, const std::string& value) {
  ::setenv(name.c_str(), value.c_str(), 1);
}

inline void unsetEnvironmentVariable(const std::string& name) {
  ::unsetenv(name.c_str());
}

inline int processId() {
  return static_cast<int>(::getpid());
}

inline int closeSocket(SocketHandle handle) {
  return ::close(handle);
}

inline int addressToBinary(int family, const char* text, void* address) {
  return ::inet_pton(family, text, address);
}

inline const char* addressToText(int family, const void* address, char* buffer, std::size_t size) {
  return ::inet_ntop(family, address, buffer, size);
}

inline std::string addressErrorMessage(int code) {
  const char* text = ::gai_strerror(code);
  return text == nullptr ? std::string("unknown error") : std::string(text);
}

}  // namespace platform
}  // namespace bjorne_agent

#endif
