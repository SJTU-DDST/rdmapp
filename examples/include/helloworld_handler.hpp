#pragma once

#include "qp_acceptor.h"
#include "qp_connector.h"
#include <cppcoro/task.hpp>

cppcoro::task<void> client(rdmapp::qp_connector &connector,
                           std::string_view host, uint16_t port);
cppcoro::task<void> server(rdmapp::qp_acceptor &acceptor);
