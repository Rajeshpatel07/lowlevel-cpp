#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int cnt_space(std::string &str) {
  int cnt = 0;
  for (char c : str) {
    if (c == ' ')
      cnt++;
  }
  return cnt;
}

std::vector<std::string> split(std::string s, const std::string &delimiter) {
  std::vector<std::string> tokens;
  size_t pos = 0;
  std::string token;

  while ((pos = s.find(delimiter)) != std::string::npos) {
    token = s.substr(0, pos);
    tokens.push_back(token);
    s.erase(0, pos + delimiter.length());
  }
  tokens.push_back(s);

  return tokens;
}

bool handle_read(int fd, std::string &buffer) {
  char header[4];

  int header_size = 4;
  char *ptr = header;
  while (header_size > 0) {
    int rv = read(fd, ptr, header_size);
    if (rv <= 0) {

      std::cout << "wt errno: " << rv << std::endl;
      perror("read header:");
      return false;
    }

    ptr += rv;
    header_size -= rv;
  }

  uint32_t body_size;
  memcpy(&body_size, header, 4);
  body_size = ntohl(body_size);

  buffer.resize(body_size);
  ptr = &buffer[0];
  while (body_size > 0) {
    int rv = read(fd, ptr, body_size);
    if (rv <= 0) {
      perror("read header:");
      return false;
    }
    body_size -= rv;
    ptr += rv;
  }

  std::cout << buffer << std::endl;

  return true;
}

bool handle_write(int fd, std::string &cmd) {
  // Write the full length of cmd.
  // Divide the cmd into tokens.
  std::vector<std::string> words = split(cmd, " ");
  int spaces = cnt_space(cmd);
  int full_size = htonl((cmd.size() - spaces) + (words.size() * 4));
  char header[4];
  memcpy(header, &full_size, 4);

  // Send header
  size_t header_size = 4;
  const char *ptr = header;
  while (header_size > 0) {
    int wt = write(fd, ptr, header_size);
    if (wt <= 0) {
      std::cout << "wt errno: " << wt << std::endl;
      perror("write header: ");
      return false;
    }
    ptr += wt;
    header_size -= wt;
  }

  // sending every token to server..
  for (std::string msg : words) {
    int body_size = htonl(msg.size());
    char header[4];
    memcpy(header, &body_size, 4);

    // Send header
    size_t header_size = 4;
    const char *ptr = header;
    while (header_size > 0) {
      int wt = write(fd, ptr, header_size);
      if (wt <= 0) {
        std::cout << "wt errno: " << wt << std::endl;
        perror("write header: ");
        return false;
      }
      ptr += wt;
      header_size -= wt;
    }

    body_size = msg.size();
    ptr = msg.data();
    while (body_size > 0) {
      int wt = write(fd, ptr, body_size);
      if (wt <= 0) {
        perror("write: ");
        return false;
      }
      ptr += wt;
      body_size -= wt;
    }
  }
  return true;
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(8000);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

  if (connect(fd, (sockaddr *)&address, sizeof(address)) < 0) {
    perror("connect: ");
    exit(1);
  }

  // std::vector<std::string> query_list = {"get name"};
  std::string input;
  while (true) {
    std::cout << ">";
    std::getline(std::cin, input);
    if (input.size() == 0) {
      continue;
    }

    if (!handle_write(fd, input)) {
      std::cerr << "Error while writting..." << std::endl;
    }
    if (input == "exit") {
      break;
    }
    std::string buffer;
    if (!handle_read(fd, buffer)) {
      std::cerr << "Error while reading..." << std::endl;
    }
  }
  close(fd);
}
