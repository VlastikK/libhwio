#include "hwio_server.h"
#include "hwio_remote_utils.h"

#include <algorithm>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>

using namespace std;
using namespace hwio;

HwioServer::HwioServer(struct addrinfo * addr, std::vector<ihwio_bus *> buses) :
		addr(addr), master_socket(-1), log_level(logWARNING), buses(buses) {
}

void HwioServer::prepare_server_socket() {
	struct sockaddr_in * addr = ((struct sockaddr_in *) this->addr->ai_addr);
	int opt = true;
	//create a master socket
	if ((master_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		std::stringstream errss;
		errss << "hwio_server socket failed: " << gai_strerror(master_socket);
		throw std::runtime_error(std::string("[HWIO, server]") + errss.str());
	}

	// set master socket to allow multiple connections ,
	// also reuse socket after previous application crashed
	int err = setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &opt,
			sizeof(opt));
	if (err < 0) {
		std::stringstream errss;
		errss << "hwio_server setsockopt SO_REUSEADDR failed: "
				<< gai_strerror(master_socket);
		throw std::runtime_error(std::string("[HWIO, server]") + errss.str());
	}
	int val = 1;
	err = setsockopt(master_socket, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof val);
	if (err < 0) {
		std::stringstream errss;
		errss << "hwio_server setsockopt SO_KEEPALIVE failed: "
				<< gai_strerror(master_socket);
		throw std::runtime_error(std::string("[HWIO, server]") + errss.str());
	}
	//bind the socket
	err = bind(master_socket, (sockaddr *) addr, sizeof(*addr));
	if (err < 0) {
		std::stringstream errss;
		errss << "hwio_server bind failed: " << gai_strerror(err);
		throw std::runtime_error(std::string("[HWIO, server]") + errss.str());
	}

	err = listen(master_socket, MAX_PENDING_CONNECTIONS);
	if (err < 0) {
		std::stringstream errss;
		errss << "hwio_server listen failed: " << gai_strerror(err);
		throw std::runtime_error(std::string("[HWIO, server]") + errss.str());
	}

	struct pollfd pfd;
	pfd.fd = master_socket;
	pfd.events = POLLIN;
	poll_fds.push_back(pfd);

	// now can accept the incoming connection
	if (log_level >= logDEBUG)
		std::cout << "[DEBUG] Hwio server waiting for connections ..." << endl;

}

HwioServer::PProcRes HwioServer::handle_msg(ClientInfo * client,
		Hwio_packet_header header) {
	ErrMsg * errM;
	DevQuery * q;
	Hwio_packet_header * txHeader;
	int cnt;
	stringstream ss;

	HWIO_CMD cmd = static_cast<HWIO_CMD>(header.command);

	switch (cmd) {
	case HWIO_CMD_READ:
		return handle_read(client, header);

	case HWIO_CMD_WRITE:
		return handle_write(client, header);

	case HWIO_CMD_REMOTE_CALL:
		return handle_remote_call(client, header);

	case HWIO_CMD_PING_REQUEST:
		if (header.body_len != 0)
			return send_err(MALFORMED_PACKET, "ECHO_REQUEST: size has to be 0");
		txHeader = reinterpret_cast<Hwio_packet_header*>(tx_buffer);
		txHeader->command = HWIO_CMD_PING_REPLY;
		txHeader->body_len = 0;
		return PProcRes(false, sizeof(Hwio_packet_header));

	case HWIO_CMD_QUERY:
		q = reinterpret_cast<DevQuery*>(rx_buffer);
		cnt = header.body_len / sizeof(Dev_query_item);
		if (header.body_len != cnt * sizeof(Dev_query_item)) {
			ss << "QUERY: wrong size of packet " << header.body_len
					<< "(not divisible by " << sizeof(Dev_query_item) << ")";
			return send_err(MALFORMED_PACKET, ss.str());
		}

		if (cnt == 0 || cnt > MAX_ITEMS_PER_QUERY) {
			ss << "Unsupported number of queries (" << cnt << ")" << endl;
			return send_err(UNKNOWN_COMMAND, ss.str());
		}
		return device_lookup_resp(client, q, cnt, tx_buffer);

	case HWIO_CMD_BYE:
		return PProcRes(true, 0);

	case HWIO_CMD_MSG:
		errM = reinterpret_cast<ErrMsg*>(rx_buffer);
		errM->msg[MAX_NAME_LEN - 1] = 0;
		if (log_level >= logERROR)
			LOG_ERR << "code:" << errM->err_code << ": " << errM->msg << endl;
		return PProcRes(false, 0);

	default:
		if (log_level >= logERROR)
			LOG_ERR << "Unknown command " << cmd;
		return send_err(UNKNOWN_COMMAND, ss.str());
	}
}

ClientInfo * HwioServer::add_new_client(int socket) {
	unsigned id = 0;
	for (auto & client : clients) {
		//if position is empty
		if (client == nullptr) {
			break;
		}
		id++;
	}
	ClientInfo * client = new ClientInfo(id, socket);
	if (id < clients.size())
		clients[id] = client;
	else
		clients.push_back(client);

	assert(fd_to_client.find(socket) == fd_to_client.end());
	fd_to_client[socket] = client;
	struct pollfd pfd;
	pfd.events = POLLIN;
	pfd.fd = socket;
	pfd.revents = 0;
	poll_fds.push_back(pfd);
	return client;
}

size_t HwioServer::get_client_cnt() {
	size_t i = 0;
	for (auto c : clients) {
		if (c != nullptr)
			i++;
	}
	assert(i == fd_to_client.size());
	return i;
}

void HwioServer::handle_client_msgs(bool * run_server) {
	struct timespec timeout;
	timeout.tv_nsec = 1000 * POLL_TIMEOUT_MS;
	timeout.tv_sec = POLL_TIMEOUT_MS / 1000;
	sigset_t origmask;
	sigprocmask(0, nullptr, &origmask);

	while (*run_server) {
		// wait for an activity on one of the sockets , timeout is NULL ,
		// so wait indefinitely
		int err = ppoll(&poll_fds[0], poll_fds.size(), &timeout, &origmask);
		if (err == 0) {
			// timeout
			continue;
		} else if (err < 0) {
			if (log_level >= logERROR)
				LOG_ERR << "poll error" << endl;
			continue;
		}

		// If something happened on the master socket,
		// then its an incoming connection and we need to spot new client info instance
		for (auto & fd : poll_fds) {
			auto e = fd.revents;
			if (fd.fd < 0 || fd.revents == 0)
				continue;

			if (fd.revents != POLLIN)
				std::cerr << "fd:" << fd.fd << " revents:" << fd.revents
						<< std::endl;

			if ((e & POLLERR) | (e & POLLHUP) | (e & POLLNVAL)) {
				if (fd.fd == master_socket) {
					LOG_ERR << "Error on master socket" << std::endl;
					continue;
				} else {
					LOG_ERR << "Error on socket" << fd.fd << std::endl;
				}
			} else if (fd.fd == master_socket) {
				struct sockaddr_in address;
				int addrlen = sizeof(address);
				int new_socket;
				if ((new_socket = accept(master_socket,
						(struct sockaddr *) &address, (socklen_t*) &addrlen))
						< 0)
					throw runtime_error("error in accept for client socket");

				if (log_level >= logINFO) {
					//inform user of socket number - used in send and receive commands
					std::cout << "[INFO] New connection, ip:"
							<< inet_ntoa(address.sin_addr) << ", port:"
							<< ntohs(address.sin_port) << " socket:"
							<< new_socket << endl;
				}
				add_new_client(new_socket);
			} else {
				handle_client_requests(fd.fd);
			}
		}
	}
}
void HwioServer::handle_client_requests(int sd) {
	// else its some IO operation on some other socket
	//Check if it was for closing , and also read the
	//incoming message
	std::map<int, ClientInfo*>::iterator _client = fd_to_client.find(sd);
	ClientInfo * client = nullptr;
	if (_client == fd_to_client.end()) {
		client = add_new_client(sd);
	} else {
		client = _client->second;
	}
	//std::cout << "handle_client_requests:" << sd << " " << client->id
	//		<< std::endl;
	assert(client->fd == sd);

	PProcRes respMeta(true, 0);
	Hwio_packet_header header;

	int rx_data_size = 0;
	while (true) {
		errno = 0;
		int s = recv(sd, &header, sizeof(Hwio_packet_header), MSG_DONTWAIT);
		if (s < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			} else {
				// process as error
				rx_data_size = 0;
				break;
			}
		} else if (s == 0) {
			return;
		} else {
			rx_data_size += s;
			if (rx_data_size == sizeof(Hwio_packet_header))
				break;
		}
	}
	if (rx_data_size != 0) {
		bool err = false;
		if (header.body_len) {
			rx_data_size = recv(sd, rx_buffer, header.body_len, MSG_DONTWAIT);
			if (rx_data_size == 0) {
				respMeta = send_err(MALFORMED_PACKET,
						std::string(
								"Wrong frame body (expectedSize="
										+ std::to_string(int(header.body_len))
										+ " got="
										+ std::to_string(rx_data_size)));
				err = true;
			}
		}
		if (!err) {
			respMeta = handle_msg(client, header);
			if (respMeta.tx_size) {
				if (send(sd, tx_buffer, respMeta.tx_size, 0) < 0) {
					respMeta = PProcRes(true, 0);
					if (log_level >= logERROR) {
						std::cerr
								<< "[HWIO, server] Can not send response to client "
								<< (client->id) << " (socket=" << (client->fd)
								<< ")" << std::endl;
					}
				}
			}
		}
	}

	if (respMeta.disconnect) {
		// Somebody disconnected, get his details and print
		// packet had wrong format or connection was disconnected.
		if (log_level >= logINFO) {
			std::cout << "[INFO] " << "Client " << client->id << " socket:"
					<< client->fd << " disconnected" << endl;

			for (auto d : client->devices) {
				std::cout << "    owned device:" << endl;
				for (auto & s : d->get_spec())
					std::cout << "        " << s.to_str() << endl;
			}
		}
		clients[client->id] = nullptr;
		fd_to_client.erase(sd);
		auto new_poll_fds = std::vector<struct pollfd>();
		int i = 0;
		for (auto fd : poll_fds) {
			if (fd.fd == sd)
				continue;
			new_poll_fds.push_back(fd);
			i++;
		}
		//	poll_fds.erase(
		//			std::remove_if(poll_fds.begin(), poll_fds.end(),
		//					[sd](struct pollfd item) {
		//						return item.fd == sd;
		//					}));
		//
		poll_fds = new_poll_fds;
		//close(sd); is in desctructor
		delete client;
	}
}

HwioServer::~HwioServer() {
	if (log_level >= logINFO)
		std::cout << "[INFO] Hwio server shutting down" << std::endl;

	for (auto & c : clients) {
		delete c;
	}
	if (master_socket >= 0)
		close(master_socket);
}
