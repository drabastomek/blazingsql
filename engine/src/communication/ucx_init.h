
#include <iostream>
#include <string>
#include <utility>
#include <limits>
#include <memory>

#include <netdb.h>

#include <ucp/api/ucp.h>

#include <communication/CommunicationInterface/messageReceiver.hpp>

namespace ral {
namespace communication {

template <class Callable>
static inline void CheckError(const bool condition,
                              const std::string &message,
                              Callable &&callable) {
  if (condition) {
    std::forward<Callable>(callable)();
    std::cerr << message << std::endl;
    throw std::runtime_error(message);
  }
}

static inline void CheckError(const bool condition,
                              const std::string &message) {
  CheckError(condition, message, []() {});
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

class UcpWorkerAddress {
public:
  ucp_address_t *address;
  std::size_t length;
};

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

class AddressExchanger {
public:
  const UcpWorkerAddress Exchange(const UcpWorkerAddress &ucpWorkerAddress) {

    std::uint8_t *data = new std::uint8_t[ucpWorkerAddress.length];

    UcpWorkerAddress peerUcpWorkerAddress{
        reinterpret_cast<ucp_address_t *>(data),
        std::numeric_limits<decltype(ucpWorkerAddress.length)>::max()};

    try {
      Exchange(&peerUcpWorkerAddress.length,
               fd(),
               &ucpWorkerAddress.length,
               sizeof(ucpWorkerAddress.length));

      Exchange(peerUcpWorkerAddress.address,
               fd(),
               ucpWorkerAddress.address,
               ucpWorkerAddress.length);
    } catch (...) {
      delete[] data;
      throw;
    }

    return peerUcpWorkerAddress;
  }

  static std::unique_ptr<AddressExchanger>
  MakeForSender(const std::uint16_t port);

  static std::unique_ptr<AddressExchanger>
  MakeForReceiver(const std::uint16_t port, const char *hostname);

  virtual int fd() = 0;

private:
  static inline void Exchange(void *peerData,
                              const int fd,
                              const void *localData,
                              const std::size_t length) {
    int ret = send(fd, localData, length, 0);
    CheckError(ret != static_cast<int>(length), "send");
    ret = recv(fd, peerData, length, MSG_WAITALL);
    CheckError(ret != static_cast<int>(length), "recv");
  }
};

class AddressExchangerForSender : public AddressExchanger {
public:
  ~AddressExchangerForSender() {
		closeCurrentConnection();
		CheckError(close(lsock_), "close sender");
	}

  AddressExchangerForSender(const std::uint16_t port) {
    struct sockaddr_in inaddr;

    lsock_ = -1;
    dsock_ = -1;
    int optval = 1;
    int ret;

    lsock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock_ < 0) {
      std::cerr << "open server socket" << std::endl;
      throw std::runtime_error("open server socket");
    }

    optval = 1;
    ret = setsockopt(lsock_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (ret < 0) {
      std::cerr << "server setsockopt()" << std::endl;
      close(lsock_);
      throw std::runtime_error("server setsockopt()");
    }

    inaddr.sin_family = AF_INET;
    inaddr.sin_port = htons(port);
    inaddr.sin_addr.s_addr = INADDR_ANY;
    std::memset(inaddr.sin_zero, 0, sizeof(inaddr.sin_zero));
    ret = bind(lsock_, (struct sockaddr *) &inaddr, sizeof(inaddr));
    if (ret < 0) {
      close(lsock_);
      throw std::runtime_error("bind server");
    }

    ret = listen(lsock_, SOMAXCONN);
    if (ret < 0) {
      close(lsock_);
      throw std::runtime_error("listen server");
    }


    // dsock_ = accept(lsock_, NULL, NULL);
    // if (dsock_ < 0) {
    //   std::cout << "accept server" << std::endl;
    //   close(lsock_);
    //   throw std::runtime_error("accept server");
    // }

    // close(lsock_);

    // CheckError(dsock_ < 0, "server_connect");
  }

	char *get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen)
	{
			switch(sa->sa_family) {
					case AF_INET:
							inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
											s, maxlen);
							break;

					case AF_INET6:
							inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
											s, maxlen);
							break;

					default:
							strncpy(s, "Unknown AF", maxlen);
							return NULL;
			}

			return s;
	}

	bool acceptConnection() {
		struct sockaddr address;
    unsigned int addrlen = sizeof(address);
    dsock_ = accept(lsock_, &address, (socklen_t*)&addrlen);
    if (dsock_ < 0) {
      close(lsock_);
      throw std::runtime_error("accept server");
    }

    CheckError(dsock_ < 0, "server_connect");

		char str_buffer[INET6_ADDRSTRLEN];
		get_ip_str(&address, str_buffer, INET6_ADDRSTRLEN);

		return true;
	}

	void closeCurrentConnection(){
		if (dsock_ != -1) {
			CheckError(close(dsock_), "close sender fd");
			dsock_ = -1;
		}
	}

  int fd() final { return dsock_; }

private:
  int dsock_;
	int lsock_;
};

class AddressExchangerForReceiver : public AddressExchanger {
public:
  ~AddressExchangerForReceiver() {
		CheckError(close(fd()), "close receiver");
	}

  AddressExchangerForReceiver(const std::uint16_t port, const char *hostname) {
    struct sockaddr_in conn_addr;
    struct hostent *he;
    int connfd;
    int ret;

    connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
      const std::string message = "open client socket";
      throw std::runtime_error(message);
    }

    he = gethostbyname(hostname);
    if (he == NULL || he->h_addr_list == NULL) {
      const std::string message = "found a host";
      close(connfd);
      throw std::runtime_error(message);
    }

    conn_addr.sin_family = he->h_addrtype;
    conn_addr.sin_port = htons(port);

    std::memcpy(&conn_addr.sin_addr, he->h_addr_list[0], he->h_length);
    std::memset(conn_addr.sin_zero, 0, sizeof(conn_addr.sin_zero));


    int num_attempts = 50;
    int attempt = 0;
    while (attempt < num_attempts){
        ret = connect(connfd, (struct sockaddr *) &conn_addr, sizeof(conn_addr));
        if (ret < 0) {
            attempt++;
		std::this_thread::sleep_for (std::chrono::seconds(1));
        } else {
            break;
        }
        if (attempt == num_attempts){
            std::cout<<"could not connect to client"<<std::endl;
            const std::string message = "could not connect to client";
            close(connfd);
            throw std::runtime_error(message);
        }
    }

    CheckError(connfd < 0, "server_connect");
    connfd_ = connfd;
  }

  int fd() final { return connfd_; }

private:
  int connfd_;
};

std::unique_ptr<AddressExchanger>
AddressExchanger::MakeForSender(const std::uint16_t port) {
  return std::make_unique<AddressExchangerForSender>(port);
}

std::unique_ptr<AddressExchanger>
AddressExchanger::MakeForReceiver(const std::uint16_t port,
                                  const char *hostname) {
  return std::make_unique<AddressExchangerForReceiver>(port, hostname);
}

static void request_init(void *request)
{
  struct comm::ucx_request *req = (comm::ucx_request *)request;
  req->completed = 0;
  req->uid = -1;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ucp_context_h CreateUcpContext() {
  ucp_config_t *config;
  ucs_status_t status = ucp_config_read(NULL, NULL, &config);
  CheckError(status != UCS_OK, "ucp_config_read");

  ucp_params_t ucp_params;
  std::memset(&ucp_params, 0, sizeof(ucp_params));
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES |
                          UCP_PARAM_FIELD_REQUEST_SIZE |
                          UCP_PARAM_FIELD_REQUEST_INIT;
  ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP;
  ucp_params.request_size = sizeof(comm::ucx_request);
  ucp_params.request_init = request_init;
  ucp_params.mt_workers_shared = 1;

  ucp_context_h ucp_context;
  status = ucp_init(&ucp_params, config, &ucp_context);

  const bool hasPrintUcpConfig = false;
  if (hasPrintUcpConfig) {
    ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
  }

  ucp_config_release(config);
  CheckError(status != UCS_OK, "ucp_init");

  return ucp_context;
}

ucp_worker_h CreatetUcpWorker(ucp_context_h ucp_context) {
  ucp_worker_params_t worker_params;
  std::memset(&worker_params, 0, sizeof(worker_params));
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  worker_params.thread_mode = UCS_THREAD_MODE_MULTI;  // UCS_THREAD_MODE_SINGLE;

  ucp_worker_h ucp_worker;
  ucs_status_t status =
      ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
  CheckError(status != UCS_OK, "ucp_worker_create", [&ucp_context]() {
    ucp_cleanup(ucp_context);
  });

  return ucp_worker;
}

UcpWorkerAddress GetUcpWorkerAddress(ucp_worker_h ucp_worker) {
  UcpWorkerAddress ucpWorkerAddress;

  ucs_status_t status = ucp_worker_get_address(
      ucp_worker, &ucpWorkerAddress.address, &ucpWorkerAddress.length);
  CheckError(status != UCS_OK,
             "ucp_worker_get_address",
             [&ucp_worker /*, &ucp_context*/]() {
               ucp_worker_destroy(ucp_worker);
               // ucp_cleanup(ucp_context);
             });

  return ucpWorkerAddress;
}

static class ErrorHandling {
public:
  ucp_err_handling_mode_t ucp_err_mode;
  int failure;
} err_handling_opt;

static void failure_handler(void *arg, ucp_ep_h, ucs_status_t status) {
  ucs_status_t *arg_status = static_cast<ucs_status_t *>(arg);
  std::cout << '[' << std::hex << std::this_thread::get_id()
            << "] failure handler called with status " << status << std::endl;
  *arg_status = status;
}

ucp_ep_h CreateUcpEp(ucp_worker_h ucp_worker,
                     const UcpWorkerAddress &ucpWorkerAddress) {
  static ucs_status_t current_status = UCS_OK;
  ucp_ep_params_t ep_params;
  ep_params.field_mask =
      UCP_EP_PARAM_FIELD_REMOTE_ADDRESS | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
      UCP_EP_PARAM_FIELD_ERR_HANDLER | UCP_EP_PARAM_FIELD_USER_DATA;
  ep_params.address = ucpWorkerAddress.address;
  ep_params.err_mode = err_handling_opt.ucp_err_mode;
  ep_params.err_handler.cb = failure_handler;
  ep_params.err_handler.arg = NULL;
  ep_params.user_data = &current_status;

  ucp_ep_h ucp_ep;
  ucs_status_t status = ucp_ep_create(ucp_worker, &ep_params, &ucp_ep);
  CheckError(status != UCS_OK, "ucp_ep_create");

  return ucp_ep;
}

}  // namespace messages
}  // namespace communication