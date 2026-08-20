#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace kvsd {
namespace sock {
namespace {

std::string errno_str(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

bool set_nonblocking(int fd, std::string* err) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    if (err) *err = errno_str("fcntl(O_NONBLOCK)");
    return false;
  }
  return true;
}

bool set_tcp_nodelay(int fd) {
  int on = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
}

int listen_on(const std::string& bind_addr, uint16_t port, int backlog, std::string* err) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = errno_str("socket");
    return -1;
  }

  // WHY SO_REUSEADDR: without it, restarting the server fails with EADDRINUSE for as
  // long as the previous process's connections sit in TIME_WAIT. That is a minute of
  // downtime on every deploy, in exchange for a semantic nobody wants.
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    if (err) *err = "invalid bind address: " + bind_addr;
    ::close(fd);
    return -1;
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    if (err) *err = errno_str("bind");
    ::close(fd);
    return -1;
  }

  if (::listen(fd, backlog) < 0) {
    if (err) *err = errno_str("listen");
    ::close(fd);
    return -1;
  }

  // The listener must be non-blocking too: accept() on a level-triggered readable
  // listener can still return EAGAIN if the pending connection was reset between the
  // readiness report and the call, and a blocking accept() would stall the whole loop.
  if (!set_nonblocking(fd, err)) {
    ::close(fd);
    return -1;
  }

  return fd;
}

std::string peer_name(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
  char ip[INET_ADDRSTRLEN];
  if (!::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip))) return "?";
  return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace sock
}  // namespace kvsd
