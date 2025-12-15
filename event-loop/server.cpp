#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

class Conn {
public:
  int fd = -1;
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
  std::vector<uint8_t> incomming;
  std::vector<uint8_t> outgoing;
};

void set_fd_flag(int fd) {
  int flags = fcntl(fd, F_GETFL, 0); // Use F_GETFL, not F_GETFD
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags); // Use F_SETFL, not F_SETFD
}

void addToBuffer(std::vector<uint8_t> &buffer, const uint8_t *data, int size) {
  buffer.insert(buffer.end(), data, data + size);
}

void rmFromBuffer(std::vector<uint8_t> &buffer, int size) {
  buffer.erase(buffer.begin(), buffer.begin() + size);
}

static Conn *handle_accept(int fd) {
  sockaddr_in clientaddr;
  socklen_t len = sizeof(clientaddr);
  int con = accept(fd, (sockaddr *)&clientaddr, &len);
  if (con < 0) {
    // Expected error in non-blocking mode if queue empty
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("accept:");
    }
    return NULL;
  }

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip));
  std::cout << "new client: " << ip << ":" << ntohs(clientaddr.sin_port)
            << std::endl;

  set_fd_flag(con);
  Conn *client = new Conn();
  client->fd = con;
  client->want_read = true;
  return client;
}

static bool try_one_request(Conn *conn) {
  if (conn->incomming.size() < 4) {
    return false;
  }

  uint32_t body_size;
  memcpy(&body_size, conn->incomming.data(), 4);
  body_size = ntohl(body_size);

  uint32_t k_max_msg = 32 << 22;

  if (body_size > k_max_msg) {
    conn->want_close = true;
    return false;
  }

  if (4 + body_size > conn->incomming.size()) {
    return false;
  }

  const uint8_t *header = conn->incomming.data();
  const uint8_t *request = &conn->incomming[4];

  // Echo: Copy header + body to outgoing
  addToBuffer(conn->outgoing, header, 4);
  addToBuffer(conn->outgoing, request, body_size);

  std::cout << "Message: " << request << std::endl;
  rmFromBuffer(conn->incomming, 4 + body_size);

  return true;
}

static void handle_read(Conn *conn) {
  uint8_t buffer[1024 * 64];
  int rv = read(conn->fd, buffer, sizeof(buffer));

  if (rv <= 0) {
    conn->want_close = true;
    return;
  }

  addToBuffer(conn->incomming, buffer, rv);

  // FIX 2: We must try to process requests after reading!
  while (try_one_request(conn)) {
  }

  if (conn->outgoing.size() > 0) {
    conn->want_read = false;
    conn->want_write = true;
  }
}

static void handle_write(Conn *conn) {

  int wt = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());

  if (wt < 0 && errno == EAGAIN) {
    return;
  }
  if (wt < 0) {
    perror("write() error");
    conn->want_close = true; // error handling
    return;
  }

  rmFromBuffer(conn->outgoing, wt);

  if (conn->outgoing.size() == 0) {
    conn->want_read = true;
    conn->want_write = false;
  }
}

int main() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket: ");
    exit(1);
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in serveraddr = {};
  serveraddr.sin_port = htons(8000);
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_fd, (sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
    perror("bind: ");
    exit(1);
  };

  // FIX 5: Set listener to non-blocking
  set_fd_flag(server_fd);

  if (listen(server_fd, SOMAXCONN) < 0) {
    perror("listen: ");
    exit(1);
  }
  std::cout << "Server Started at port 8000...\n";

  std::vector<Conn *> fd2conn;
  std::vector<pollfd> poll_args;

  while (true) {
    poll_args.clear();

    // FIX 1: Listen for POLLIN (Read) events on server socket
    pollfd pfd = {server_fd, POLLIN, 0};
    poll_args.push_back(pfd);

    for (Conn *conn : fd2conn) {
      // FIX 4: Check if pointer is NULL
      if (!conn) {
        continue;
      }

      pollfd pfd = {conn->fd, POLLERR, 0};
      if (conn->want_read)
        pfd.events |= POLLIN;
      if (conn->want_write)
        pfd.events |= POLLOUT;

      poll_args.push_back(pfd);
    }

    int prv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
    if (prv < 0 && errno == EINTR)
      continue;
    if (prv < 0)
      perror("poll: ");

    // Check listener
    if (poll_args[0].revents & POLLIN) {
      if (Conn *conn = handle_accept(server_fd)) {
        // FIX 3: Resize to fd + 1
        if (fd2conn.size() <= (size_t)conn->fd) {
          fd2conn.resize(conn->fd + 1);
        }
        fd2conn[conn->fd] = conn;
      }
    }

    // Check clients (skip index 0)
    for (size_t i = 1; i < poll_args.size(); i++) {
      int ready = poll_args[i].revents;
      Conn *conn = fd2conn[poll_args[i].fd];

      if (ready & POLLIN) {
        handle_read(conn);
      }
      if (ready & POLLOUT) {
        handle_write(conn);
      }
      if ((ready & POLLERR) || conn->want_close) {
        close(conn->fd);
        fd2conn[conn->fd] = NULL;
        delete conn;
      }
    }
  }
  close(server_fd);
}
