#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define K_BUFFLEN ((uint32_t)32 << 22)

std::unordered_map<std::string, std::string> kvstore;

class Conn {
public:
  int fd = -1;
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
  std::vector<uint8_t> read;
  std::vector<uint8_t> write;
};

void set_sock_opt(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
}

void addToBuffer(std::vector<uint8_t> &dist, uint8_t *src, int size) {
  dist.insert(dist.end(), src, src + size);
}

void rmFromBuffer(std::vector<uint8_t> &src, int size) {
  src.erase(src.begin(), src.begin() + size);
}

void gen_response(std::vector<uint8_t> &dist, std::string &res) {
  uint32_t len = htonl(res.size());
  addToBuffer(dist, (uint8_t *)&len, 4);
  addToBuffer(dist, (uint8_t *)res.data(), res.size());
}

void executeCmd(Conn *conn, std::vector<std::string> &cmd) {
  if (cmd.size() == 0)
    return;

  std::string res;
  if (cmd[0] == "exit" && cmd.size() == 1) {
    conn->want_close = true;
  } else if (cmd[0] == "ping" && cmd.size() == 1) {
    res = "pong";
  } else if (cmd[0] == "get" && cmd.size() == 2) {
    if (kvstore.count(cmd[1])) {
      res = kvstore[cmd[1]];
    } else {
      res = "nil";
    }
  } else if (cmd[0] == "set" && cmd.size() == 3) {
    kvstore[cmd[1]] = cmd[2];
    res = "ok";
  } else if (cmd[0] == "del" && cmd.size() == 2) {
    res = kvstore.count(cmd[1]) ? "ok" : "nil";
    kvstore.erase(cmd[1]);
  } else {
    res = "ERROR";
  }

  gen_response(conn->write, res);
}

void getCmd(std::vector<std::string> &cmd, std::vector<uint8_t> &src,
            int total_len) {
  while (total_len > 0) {
    if (src.size() < 4)
      break;

    uint32_t len;
    memcpy(&len, src.data(), 4);
    len = ntohl(len);

    rmFromBuffer(src, 4);
    total_len -= 4;

    if (src.size() < len)
      break;

    std::string word((char *)src.data(), len);
    cmd.push_back(word);

    rmFromBuffer(src, len);
    total_len -= len;
  }
}

bool parse_request(Conn *conn, const uint8_t *buffer) {
  if (conn->read.size() < 4) {
    return false;
  }

  uint32_t body_size;
  memcpy(&body_size, &conn->read[0], 4);
  body_size = ntohl(body_size);

  if (body_size > K_BUFFLEN) {
    conn->want_close = true;
    return false;
  }

  if (4 + body_size > conn->read.size()) {
    return false;
  }

  // got the full message..
  uint32_t full_len;
  memcpy(&full_len, &conn->read[0], 4);
  full_len = ntohl(full_len);

  std::vector<std::string> cmd;

  rmFromBuffer(conn->read, 4);
  getCmd(cmd, conn->read, full_len);
  executeCmd(conn, cmd);

  return true;
}

void handle_read(Conn *conn) {
  uint8_t buf[64 * 1024];
  int rv = read(conn->fd, buf, sizeof(buf));
  if (rv <= 0) {
    if (rv < 0) {
      perror("read");
    }
    conn->want_close = true;
    return;
  }
  addToBuffer(conn->read, buf, rv);

  while (parse_request(conn, buf)) {
  }

  if (conn->write.size() > 0) {
    conn->want_read = false;
    conn->want_write = true;
  }
}

void handle_write(Conn *conn) {

  int wt = write(conn->fd, conn->write.data(), conn->write.size());

  if (wt < 0 && errno == EAGAIN) {
    return;
  }

  if (wt <= 0) {
    perror("write: ");
    conn->want_close = true;
    return;
  }

  rmFromBuffer(conn->write, wt);

  if (conn->write.size() == 0) {
    conn->want_write = false;
    conn->want_read = true;
  }
}

Conn *handle_accept(int fd) {
  sockaddr_in client_addr;
  socklen_t len = sizeof(client_addr);
  int client_fd = accept(fd, (sockaddr *)&client_addr, &len);

  char ipaddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ipaddr, sizeof(ipaddr));

  std::cout << "client: " << ipaddr << ":" << ntohs(client_addr.sin_port)
            << "\n";

  if (client_fd < 0) {
    perror("accept: ");
    return nullptr;
  }

  Conn *conn = new Conn();
  set_sock_opt(client_fd);
  conn->fd = client_fd;
  conn->want_read = true;
  return conn;
}

int main() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket: ");
    exit(1);
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  int live = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_KEEPALIVE, &live, sizeof(live));

  sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(8000);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind:");
    exit(1);
  }

  if (listen(server_fd, SOMAXCONN) < 0) {
    perror("listen: ");
    exit(1);
  }
  set_sock_opt(server_fd);
  std::cout << "TCP server listening on port 8000\n";

  std::vector<Conn *> fd2Conn;
  std::vector<pollfd> poll_args;

  while (true) {
    poll_args.clear();

    // pushing the server socket into poll args.
    pollfd server_pollfd = {server_fd, POLLIN, 0};
    poll_args.push_back(server_pollfd);

    // Moving every connection into poll args.
    for (Conn *conn : fd2Conn) {
      if (!conn) {
        continue;
      }

      pollfd pfd = {conn->fd, POLLERR, 0};

      if (conn->want_read) {
        pfd.events |= POLLIN;
      }
      if (conn->want_write) {
        pfd.events |= POLLOUT;
      }
      poll_args.push_back(pfd);
    }

    int prv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);

    if (prv < 0 && errno == EINTR) {
      continue;
    }
    if (prv < 0) {
      perror("poll: ");
    }

    if (poll_args[0].revents & POLLIN) {
      if (Conn *conn = handle_accept(poll_args[0].fd)) {
        if (fd2Conn.size() <= (size_t)conn->fd) {
          fd2Conn.resize(conn->fd + 1);
        }
        fd2Conn[conn->fd] = conn;
      }
    }

    for (size_t i = 1; i < poll_args.size(); i++) {
      int ready = poll_args[i].revents;
      Conn *conn = fd2Conn[poll_args[i].fd];

      if (ready & POLLIN) {
        handle_read(conn);
      }
      if (ready & POLLOUT) {
        handle_write(conn);
      }
      if ((ready & POLLERR) || conn->want_close) {
        close(conn->fd);
        fd2Conn[conn->fd] = nullptr;
        delete conn;
      }
    }
  }
  close(server_fd);
}
