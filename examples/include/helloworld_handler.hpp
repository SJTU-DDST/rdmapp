#pragma once

#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/awaitable.hpp>

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector);
asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor);
