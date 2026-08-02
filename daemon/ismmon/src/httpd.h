// ismmon: tiny single-purpose HTTP/1.1 GET server (loopback; nginx is the only client), same
// role as aisd/lorad. handler(path) -> JSON body; empty body == 404.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace ismmon {

class httpd {
public:
	httpd(int port, std::function<std::string(const std::string&)> handler);
	~httpd();
	bool ok() const { return listen_fd_ >= 0; }

private:
	void loop();
	int listen_fd_ = -1;
	std::atomic<bool> stop_{false};
	std::thread thread_;
	std::function<std::string(const std::string&)> handler_;
};

} // namespace ismmon
