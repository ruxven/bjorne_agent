#include "http/Url.h"

#include <string>

#include <catch_amalgamated.hpp>

TEST_CASE("URL parser splits an https endpoint") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("https://api.example.com/v1", url, error));
  REQUIRE(error.empty());
  REQUIRE(url.scheme == "https");
  REQUIRE(url.host == "api.example.com");
  REQUIRE(url.port == "443");
  REQUIRE(url.path == "/v1");
  REQUIRE(url.tls);
}

TEST_CASE("URL parser splits an http endpoint") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("http://llama.local/v1", url, error));
  REQUIRE(url.scheme == "http");
  REQUIRE(url.host == "llama.local");
  REQUIRE(url.port == "80");
  REQUIRE(url.path == "/v1");
  REQUIRE_FALSE(url.tls);
}

TEST_CASE("URL parser keeps an explicit port") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("http://127.0.0.1:11434/v1", url, error));
  REQUIRE(url.host == "127.0.0.1");
  REQUIRE(url.port == "11434");
  REQUIRE(url.path == "/v1");

  REQUIRE(bjorne_agent::http::parseUrl("https://api.example.com:8443/v1", url, error));
  REQUIRE(url.host == "api.example.com");
  REQUIRE(url.port == "8443");
  REQUIRE(url.tls);
}

TEST_CASE("URL parser accepts an endpoint without a path") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("https://api.example.com", url, error));
  REQUIRE(url.host == "api.example.com");
  REQUIRE(url.port == "443");
  REQUIRE(url.path.empty());
}

TEST_CASE("URL parser drops trailing slashes") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("https://api.example.com/v1/", url, error));
  REQUIRE(url.path == "/v1");

  REQUIRE(bjorne_agent::http::parseUrl("https://api.example.com/", url, error));
  REQUIRE(url.path.empty());
}

TEST_CASE("URL parser handles bracketed IPv6 hosts") {
  bjorne_agent::http::Url url;
  std::string error;
  REQUIRE(bjorne_agent::http::parseUrl("http://[::1]:8080/v1", url, error));
  REQUIRE(url.host == "[::1]");
  REQUIRE(url.port == "8080");

  REQUIRE(bjorne_agent::http::parseUrl("http://[::1]/v1", url, error));
  REQUIRE(url.host == "[::1]");
  REQUIRE(url.port == "80");
}

TEST_CASE("URL parser rejects bad endpoints") {
  bjorne_agent::http::Url url;
  std::string error;

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("api.example.com/v1", url, error));
  REQUIRE(error == "Endpoint must include a scheme: api.example.com/v1");

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("ftp://api.example.com/v1", url, error));
  REQUIRE(error == "Unsupported endpoint scheme: ftp");

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("https:///v1", url, error));
  REQUIRE(error.find("missing a host") != std::string::npos);

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("https://api.example.com:http/v1", url, error));
  REQUIRE(error == "Endpoint port is not numeric: http");

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("http://[::1/v1", url, error));
  REQUIRE(error.find("unterminated IPv6 host") != std::string::npos);

  REQUIRE_FALSE(bjorne_agent::http::parseUrl("http://[::1]x/v1", url, error));
  REQUIRE(error.find("invalid host suffix") != std::string::npos);
}

TEST_CASE("URL parser reports default ports") {
  REQUIRE(bjorne_agent::http::isDefaultPort("443"));
  REQUIRE(bjorne_agent::http::isDefaultPort("80"));
  REQUIRE(bjorne_agent::http::isDefaultPort(""));
  REQUIRE_FALSE(bjorne_agent::http::isDefaultPort("8080"));
  REQUIRE_FALSE(bjorne_agent::http::isDefaultPort("8443"));
}
