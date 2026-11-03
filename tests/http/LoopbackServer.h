#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if BJORNE_AGENT_HAS_OPENSSL
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#else
struct SSL;
#endif

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

namespace bjorne_agent_test {

// Single connection HTTP(S) server on 127.0.0.1 for transport tests. TLS mode
// generates a throwaway self-signed certificate, which the transport accepts
// because it does not verify certificates. A build without OpenSSL has no TLS
// mode, so the TLS cases in the test files are compiled out with it.
class LoopbackServer {
 public:
  // CloseImmediately drops the connection before any handshake, which makes
  // TLS handshake fail. ResetAfterRead and TlsResetAfterRead read the whole
  // request and then reset the connection, which makes the client's read fail
  // instead of returning a clean end of stream.
  enum class Mode { Plain, Tls, CloseImmediately, ResetAfterRead, TlsResetAfterRead };

  explicit LoopbackServer(Mode mode = Mode::Plain) : mode_(mode) {
#if BJORNE_AGENT_HAS_OPENSSL
    if (mode_ == Mode::Tls || mode_ == Mode::TlsResetAfterRead) {
      makeCertificate();
    }
#endif
    openListener();
    thread_ = std::thread([this] { serve(); });
  }

  ~LoopbackServer() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
#if BJORNE_AGENT_HAS_OPENSSL
    if (cert_ != nullptr) {
      X509_free(cert_);
    }
    if (key_ != nullptr) {
      EVP_PKEY_free(key_);
    }
#endif
  }

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  const std::string& port() const { return port_; }
  const std::string& request() const { return request_; }
  void setResponse(std::string response) { response_ = std::move(response); }

  // Joins the server thread and reports whether a connection was served.
  bool wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
    return served_;
  }

 private:
  void openListener() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int enable = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 1) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    port_ = std::to_string(::ntohs(bound.sin_port));
  }

  int acceptClient() {
    for (int attempt = 0; attempt < 50 && !stop_; ++attempt) {
      pollfd entry{listen_fd_, POLLIN, 0};
      const int ready = ::poll(&entry, 1, 100);
      if (ready < 0) {
        return -1;
      }
      if (ready > 0 && (entry.revents & POLLIN) != 0) {
        return ::accept(listen_fd_, nullptr, nullptr);
      }
    }
    return -1;
  }

  void serve() {
    if (listen_fd_ < 0) {
      return;
    }
    const int client = acceptClient();
    if (client < 0) {
      return;
    }
    timeval timeout{2, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (mode_ == Mode::CloseImmediately) {
      ::close(client);
      served_ = true;
      return;
    }
    if (mode_ == Mode::Tls || mode_ == Mode::TlsResetAfterRead) {
#if BJORNE_AGENT_HAS_OPENSSL
      serveTls(client);
#endif
    } else {
      servePlain(client);
    }
    ::close(client);
  }

  // A zero linger timeout turns close() into a reset, so the peer sees an
  // error instead of an orderly shutdown.
  static void prepareReset(int client) {
    linger option{1, 0};
    ::setsockopt(client, SOL_SOCKET, SO_LINGER, &option, sizeof(option));
  }

  void servePlain(int client) {
    if (!readRequest(client, nullptr)) {
      return;
    }
    if (mode_ == Mode::ResetAfterRead) {
      prepareReset(client);
      served_ = true;
      return;
    }
    ::send(client, response_.data(), response_.size(), MSG_NOSIGNAL);
    served_ = true;
  }

#if BJORNE_AGENT_HAS_OPENSSL
  void serveTls(int client) {
    SSL_CTX* context = SSL_CTX_new(TLS_server_method());
    if (context == nullptr) {
      return;
    }
    SSL_CTX_use_certificate(context, cert_);
    SSL_CTX_use_PrivateKey(context, key_);
    SSL* ssl = SSL_new(context);
    SSL_CTX_free(context);
    if (ssl == nullptr) {
      return;
    }
    SSL_set_fd(ssl, client);
    if (SSL_accept(ssl) == 1) {
      if (readRequest(client, ssl)) {
        if (mode_ == Mode::TlsResetAfterRead) {
          prepareReset(client);
          served_ = true;
          SSL_free(ssl);
          return;
        }
        SSL_write(ssl, response_.data(), static_cast<int>(response_.size()));
        served_ = true;
      }
      SSL_shutdown(ssl);
    }
    SSL_free(ssl);
  }
#endif

  // One read from either the TLS session or the plain socket.
  static int readFrom(int client, SSL* ssl, char* buffer, int size) {
#if BJORNE_AGENT_HAS_OPENSSL
    if (ssl != nullptr) {
      return SSL_read(ssl, buffer, size);
    }
#else
    (void)ssl;
#endif
    return static_cast<int>(::recv(client, buffer, size, 0));
  }

  // Reads until the headers and the declared body have arrived.
  bool readRequest(int client, SSL* ssl) {
    std::string data;
    size_t content_length = 0;
    bool have_headers = false;
    for (;;) {
      char buffer[4096];
      const int read = readFrom(client, ssl, buffer, sizeof(buffer));
      if (read <= 0) {
        break;
      }
      data.append(buffer, static_cast<size_t>(read));
      const size_t header_end = data.find("\r\n\r\n");
      if (!have_headers && header_end != std::string::npos) {
        have_headers = true;
        content_length = parseContentLength(data.substr(0, header_end));
      }
      if (have_headers && data.size() >= header_end + 4 + content_length) {
        break;
      }
    }
    request_ = data;
    return !data.empty();
  }

  static size_t parseContentLength(const std::string& headers) {
    const std::string key = "Content-Length:";
    const size_t at = headers.find(key);
    if (at == std::string::npos) {
      return 0;
    }
    return static_cast<size_t>(std::strtoul(headers.c_str() + at + key.size(), nullptr, 10));
  }

#if BJORNE_AGENT_HAS_OPENSSL
  void makeCertificate() {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (context == nullptr) {
      return;
    }
    if (EVP_PKEY_keygen_init(context) > 0 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) > 0) {
      EVP_PKEY_keygen(context, &key_);
    }
    EVP_PKEY_CTX_free(context);
    if (key_ == nullptr) {
      return;
    }

    cert_ = X509_new();
    if (cert_ == nullptr) {
      return;
    }
    X509_set_version(cert_, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert_), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert_), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert_), 3600);
    X509_set_pubkey(cert_, key_);
    X509_NAME* name = X509_get_subject_name(cert_);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert_, name);
    if (X509_sign(cert_, key_, EVP_sha256()) == 0) {
      X509_free(cert_);
      cert_ = nullptr;
    }
  }
#endif

  Mode mode_;
  std::atomic<bool> stop_{false};
  bool served_ = false;
  int listen_fd_ = -1;
  std::string port_;
  std::string request_;
  std::string response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}";
#if BJORNE_AGENT_HAS_OPENSSL
  X509* cert_ = nullptr;
  EVP_PKEY* key_ = nullptr;
#endif
  std::thread thread_;
};

}  // namespace bjorne_agent_test
