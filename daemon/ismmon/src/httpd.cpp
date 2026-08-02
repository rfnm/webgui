#include "httpd.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ismmon {

httpd::httpd(int port, std::function<std::string(const std::string&)> handler) : handler_(std::move(handler)) {
	listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if(bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listen_fd_, 8) < 0) {
		perror("ismmond httpd bind/listen");
		close(listen_fd_);
		listen_fd_ = -1;
		return;
	}
	thread_ = std::thread([this] { loop(); });
}

httpd::~httpd() {
	stop_ = true;
	if(listen_fd_ >= 0) {
		shutdown(listen_fd_, SHUT_RDWR);
		close(listen_fd_);
	}
	if(thread_.joinable()) { thread_.join(); }
}

static bool send_all(int fd, const char* p, size_t n) {
	while(n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if(w <= 0) { return false; }
		p += w;
		n -= (size_t)w;
	}
	return true;
}

void httpd::loop() {
	while(!stop_) {
		int fd = accept(listen_fd_, nullptr, nullptr);
		if(fd < 0) {
			if(stop_) { break; }
			continue;
		}
		struct timeval tv = { 0, 500000 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		char req[2048];
		size_t got = 0;
		while(got < sizeof(req) - 1) {
			ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
			if(r <= 0) { break; }
			got += (size_t)r;
			req[got] = 0;
			if(memchr(req, '\n', got)) { break; }
		}
		if(got > 0) {
			req[got] = 0;
			char method[8] = {0}, path[256] = {0};
			sscanf(req, "%7s %255s", method, path);
			std::string body;
			if(strcmp(method, "GET") == 0) { body = handler_(path); }
			char hdr[256];
			if(!body.empty()) {
				snprintf(hdr, sizeof(hdr),
				         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
				         "Cache-Control: no-store\r\nConnection: close\r\n\r\n", body.size());
			} else {
				body = "{\"error\":\"not found\"}";
				snprintf(hdr, sizeof(hdr),
				         "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
				         "Connection: close\r\n\r\n", body.size());
			}
			if(send_all(fd, hdr, strlen(hdr))) { send_all(fd, body.data(), body.size()); }
		}
		close(fd);
	}
}

} // namespace ismmon
