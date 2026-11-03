#include "http/HttpClient.h"

#include "http/Resolver.h"
#include "http/Url.h"

// BJORNE_AGENT_HAS_OPENSSL comes from the build. At 0 the OpenSSL headers and library
// are absent entirely, so everything that touches SSL is compiled out and https
// endpoints are rejected at the model client.
#if BJORNE_AGENT_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "log/Log.h"

namespace bjorne_agent {
namespace http {

namespace {

constexpr int kReadBufferSize = 65536;

#if BJORNE_AGENT_HAS_OPENSSL
// OpenSSL's socket BIO writes with send(..., 0), so a peer that resets the
// connection raises SIGPIPE and would kill the process. The plain transport
// uses MSG_NOSIGNAL instead, which OpenSSL cannot be told to use without a
// custom BIO. Ignore the signal once, process wide.
void ignoreSigPipe() {
#ifdef SIGPIPE
  static const bool ignored = [] {
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      log::fail("Could not ignore SIGPIPE. A peer that resets the connection may kill the process.");
      return false;
    }
    return true;
  }();
  (void)ignored;
#endif
}
#endif

// Case-insensitive header lookup. Returns the trimmed value, or an empty
// string when the header is absent.
std::string findHeader(const std::string& headers, const std::string& name) {
  std::string wanted = name;
  std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  size_t pos = 0;
  while (pos < headers.size()) {
    const size_t lineEnd = headers.find("\r\n", pos);
    const std::string line = headers.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (key == wanted) {
        std::string value = line.substr(colon + 1);
        const size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
          return "";
        }
        const size_t last = value.find_last_not_of(" \t");
        return value.substr(first, last - first + 1);
      }
    }
    if (lineEnd == std::string::npos) {
      break;
    }
    pos = lineEnd + 2;
  }
  return "";
}

// Reads the hex chunk size at `pos` and advances past the size line.
bool parseChunkSize(const std::string& raw, size_t& pos, size_t& size, std::string& error) {
  const size_t lineEnd = raw.find("\r\n", pos);
  if (lineEnd == std::string::npos) {
    error = "Malformed chunked body: missing chunk size line";
    return false;
  }
  std::string digits = raw.substr(pos, lineEnd - pos);
  const size_t semicolon = digits.find(';');
  if (semicolon != std::string::npos) {
    digits = digits.substr(0, semicolon);
  }
  if (digits.empty()) {
    error = "Malformed chunked body: empty chunk size";
    return false;
  }
  size_t value = 0;
  for (const char c : digits) {
    int digit = 0;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      error = "Malformed chunked body: invalid chunk size";
      return false;
    }
    value = value * 16 + static_cast<size_t>(digit);
  }
  size = value;
  pos = lineEnd + 2;
  return true;
}

// Decodes an HTTP/1.1 chunked body. The raw text still carries the chunk
// framing, which is not valid JSON, so it has to be stripped before the body
// reaches the caller.
bool decodeChunked(const std::string& raw, std::string& out, std::string& error) {
  out.clear();
  size_t pos = 0;
  for (;;) {
    size_t chunkSize = 0;
    if (!parseChunkSize(raw, pos, chunkSize, error)) {
      return false;
    }
    if (chunkSize == 0) {
      return true;
    }
    if (pos + chunkSize > raw.size()) {
      error = "Malformed chunked body: truncated chunk";
      return false;
    }
    out.append(raw, pos, chunkSize);
    pos += chunkSize;
    if (pos + 2 > raw.size() || raw.compare(pos, 2, "\r\n") != 0) {
      error = "Malformed chunked body: missing chunk terminator";
      return false;
    }
    pos += 2;
  }
}

// Produces the body the caller expects: chunk framing removed, and the
// length honoured when the server states one.
bool readBody(const std::string& headers, const std::string& raw, std::string& out, std::string& error) {
  const std::string transferEncoding = findHeader(headers, "transfer-encoding");
  if (transferEncoding.find("chunked") != std::string::npos) {
    return decodeChunked(raw, out, error);
  }
  out = raw;
  const std::string contentLength = findHeader(headers, "content-length");
  if (contentLength.empty()) {
    return true;
  }
  size_t length = 0;
  for (const char c : contentLength) {
    if (c < '0' || c > '9') {
      // A malformed length says nothing about the body, so the body is kept
      // as it arrived rather than cut at an arbitrary point.
      return true;
    }
    length = length * 10 + static_cast<size_t>(c - '0');
  }
  if (length < out.size()) {
    out.resize(length);
  }
  return true;
}

std::string buildRequest(const HttpRequest& request) {
  std::string requestText = request.method + " " + request.path + " HTTP/1.1\r\n";
  requestText += "Host: " + (isDefaultPort(request.port) ? request.host : request.host + ":" + request.port) + "\r\n";
  requestText += "Content-Type: " + request.content_type + "\r\n";
  requestText += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  for (const auto& header : request.headers) {
    requestText += header.first + ": " + header.second + "\r\n";
  }
  requestText += "Connection: close\r\n";
  requestText += "\r\n";
  requestText += request.body;
  return requestText;
}

// Resolves the host and connects, trying each address in turn. Returns the
// connected socket, or -1 with error set.
int connectToHost(const std::string& host, const std::string& port, std::string& error) {
  std::vector<ResolvedAddress> addresses;
  log::detail("Resolving " + host + ":" + port);
  if (!resolveHost(host, port, addresses, error)) {
    log::fail(error);
    return -1;
  }
  for (const ResolvedAddress& address : addresses) {
    const int fd = ::socket(address.storage.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
      error = "Socket creation failed";
      log::fail(error);
      return -1;
    }
    log::detail("Connecting to " + host + ":" + port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address.storage), address.length) == 0) {
      return fd;
    }
    ::close(fd);
  }
  error = "Socket connect failed";
  log::fail(error);
  return -1;
}

bool readAllPlain(int fd, std::string& data, std::string& error) {
  data.clear();
  char buffer[kReadBufferSize];
  for (;;) {
    const ssize_t bytes = ::recv(fd, buffer, kReadBufferSize, 0);
    if (bytes < 0) {
      error = "Socket read failed";
      return false;
    }
    if (bytes == 0) {
      return true;
    }
    data.append(buffer, static_cast<size_t>(bytes));
  }
}

#if BJORNE_AGENT_HAS_OPENSSL
void tlsError(std::string& error) {
  const unsigned long code = ERR_get_error();
  char buffer[256];
  ERR_error_string_n(code, buffer, sizeof(buffer));
  error = std::string("TLS error: ") + buffer;
}

bool readAllTls(SSL* ssl, std::string& data, std::string& error) {
  data.clear();
  char buffer[kReadBufferSize];
  for (;;) {
    const int bytes = SSL_read(ssl, buffer, kReadBufferSize);
    if (bytes < 0) {
      tlsError(error);
      return false;
    }
    if (bytes == 0) {
      return true;
    }
    data.append(buffer, static_cast<size_t>(bytes));
  }
}

void closeTransport(SSL* ssl, int fd) {
  SSL_free(ssl);
  ::close(fd);
}

// Sets the SNI name and the hostname the certificate has to match. The
// resolved name is written to `sniHost` so the caller can report it.
bool configureTlsSession(SSL* ssl, const std::string& host, const OpenSslTransport::Options& options,
                          std::string& sniHost, std::string& error) {
  sniHost = options.hostname.empty() ? host : options.hostname;
  if (!sniHost.empty()) {
    SSL_set_tlsext_host_name(ssl, sniHost.c_str());
  }
  if (options.ignore_self_signed || sniHost.empty()) {
    return true;
  }
  // Chain verification alone does not prove the certificate belongs to this
  // host. The hostname check does, so it runs as part of the verification.
  X509_VERIFY_PARAM* parameters = SSL_get0_param(ssl);
  X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  if (X509_VERIFY_PARAM_set1_host(parameters, sniHost.c_str(), 0) != 1) {
    error = "Could not set the expected TLS hostname: " + sniHost;
    return false;
  }
  return true;
}

// Runs the handshake and reports the certificate verification result. A
// rejected certificate aborts the handshake, and the socket error that follows
// says nothing useful, so the verification result names the real reason.
bool handshakeTls(SSL* ssl, const std::string& sniHost, bool ignoreSelfSigned, std::string& error) {
  log::detail("TLS handshake with " + sniHost);
  if (SSL_connect(ssl) != 1) {
    const int verifyResult = SSL_get_verify_result(ssl);
    if (!ignoreSelfSigned && verifyResult != X509_V_OK) {
      error = std::string("Certificate verification failed: ") + X509_verify_cert_error_string(verifyResult);
    } else {
      tlsError(error);
    }
    log::fail(error);
    return false;
  }

  if (ignoreSelfSigned) {
    log::detail("Certificate verification is off");
    return true;
  }

  const int verifyResult = SSL_get_verify_result(ssl);
  if (verifyResult != X509_V_OK) {
    error = std::string("Certificate verification failed: ") + X509_verify_cert_error_string(verifyResult);
    log::fail(error);
    return false;
  }
  log::detail("Certificate verified for " + sniHost);
  return true;
}

// Writes the whole request. SSL_write can accept fewer bytes than asked for.
bool writeTlsRequest(SSL* ssl, const std::string& request, std::string& error) {
  size_t sent = 0;
  while (sent < request.size()) {
    const size_t chunk = std::min(request.size() - sent, static_cast<size_t>(kReadBufferSize));
    const ssize_t written = SSL_write(ssl, request.data() + sent, static_cast<int>(chunk));
    if (written <= 0) {
      error = "TLS write failed";
      return false;
    }
    sent += static_cast<size_t>(written);
  }
  return true;
}
#endif

}  // namespace

OpenSslTransport::OpenSslTransport(Options options) : options_(std::move(options)) {}

bool OpenSslTransport::send(const std::string& request, const std::string& host, const std::string& port,
                            std::string& response, std::string& error) {
#if !BJORNE_AGENT_HAS_OPENSSL
  (void)request;
  (void)host;
  (void)port;
  (void)response;
  error = "TLS is not available: this build has no OpenSSL";
  return false;
#else
  ignoreSigPipe();
  const int fd = connectToHost(host, port, error);
  if (fd < 0) {
    return false;
  }

  SSL* ssl = nullptr;
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  if (context == nullptr) {
    error = "TLS context creation failed";
    ::close(fd);
    return false;
  }

  // Configure certificate verification. Ignoring self-signed certificates turns
  // peer verification off entirely, so the hostname is not checked either.
  if (options_.ignore_self_signed) {
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
  } else {
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    // Use system trust store
    SSL_CTX_set_default_verify_paths(context);
  }

  ssl = SSL_new(context);
  SSL_CTX_free(context);
  if (ssl == nullptr) {
    error = "TLS session creation failed";
    log::fail(error);
    ::close(fd);
    return false;
  }

  std::string sniHost;
  if (!configureTlsSession(ssl, host, options_, sniHost, error)) {
    log::fail(error);
    closeTransport(ssl, fd);
    return false;
  }

  SSL_set_fd(ssl, fd);
  SSL_set_connect_state(ssl);
  if (!handshakeTls(ssl, sniHost, options_.ignore_self_signed, error)) {
    closeTransport(ssl, fd);
    return false;
  }

  if (!writeTlsRequest(ssl, request, error)) {
    closeTransport(ssl, fd);
    return false;
  }

  std::string responseText;
  std::string readError;
  if (!readAllTls(ssl, responseText, readError)) {
    error = readError;
    closeTransport(ssl, fd);
    return false;
  }

  const std::string result = responseText;
  closeTransport(ssl, fd);
  response = result;
  return true;
#endif
}

bool PlainTcpTransport::send(const std::string& request, const std::string& host, const std::string& port,
                             std::string& response, std::string& error) {
  const int fd = connectToHost(host, port, error);
  if (fd < 0) {
    return false;
  }

  size_t sent = 0;
  while (sent < request.size()) {
    const size_t chunk = std::min(request.size() - sent, static_cast<size_t>(kReadBufferSize));
    const ssize_t written = ::send(fd, request.data() + sent, chunk, MSG_NOSIGNAL);
    if (written <= 0) {
      error = "Socket write failed";
      ::close(fd);
      return false;
    }
    sent += static_cast<size_t>(written);
  }

  std::string responseText;
  std::string readError;
  if (!readAllPlain(fd, responseText, readError)) {
    error = readError;
    ::close(fd);
    return false;
  }

  const std::string result = responseText;
  ::close(fd);
  response = result;
  return true;
}

std::unique_ptr<HttpClient> HttpClient::createOpenSsl() {
  return std::make_unique<HttpClient>(std::make_shared<OpenSslTransport>());
}

std::unique_ptr<HttpClient> HttpClient::createOpenSsl(OpenSslTransport::Options options) {
  return std::make_unique<HttpClient>(std::make_shared<OpenSslTransport>(std::move(options)));
}

std::unique_ptr<HttpClient> HttpClient::createPlainTcp() {
  return std::make_unique<HttpClient>(std::make_shared<PlainTcpTransport>());
}

HttpClient::HttpClient(std::shared_ptr<HttpTransport> transport) : transport_(std::move(transport)) {}

bool HttpClient::request(const HttpRequest& request, HttpResponse& out, std::string& error) {
  const std::string requestText = buildRequest(request);
  std::string responseText;
  std::string transportError;
  if (!transport_->send(requestText, request.host, request.port, responseText, transportError)) {
    error = transportError;
    return false;
  }

  const size_t headerEnd = responseText.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    error = "Malformed HTTP response: missing header terminator";
    return false;
  }
  const std::string headers = responseText.substr(0, headerEnd);
  const size_t lineEnd = responseText.find("\r\n");
  out.status_line = lineEnd == std::string::npos ? responseText.substr(0, headerEnd) : responseText.substr(0, lineEnd);
  out.status = 0;
  const size_t firstSpace = out.status_line.find(' ');
  const size_t secondSpace = out.status_line.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
  if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
    error = "Malformed HTTP status line";
    return false;
  }
  out.status = std::atoi(out.status_line.c_str() + firstSpace + 1);

  const std::string rawBody = responseText.substr(headerEnd + 4);
  if (!readBody(headers, rawBody, out.body, error)) {
    return false;
  }
  return true;
}

}  // namespace http
}  // namespace bjorne_agent
