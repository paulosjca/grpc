//
//
// Copyright 2016 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_POSIX_SOCKET_TCP

#include <errno.h>
#include <fcntl.h>
#include <grpc/support/alloc.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "src/core/lib/event_engine/channel_args_endpoint_config.h"
#include "src/core/lib/iomgr/endpoint_pair.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/tcp_posix.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/resource_quota/api.h"
#include "src/core/util/crash.h"
#include "src/core/util/string.h"

static void create_sockets(int sv[2]) {
  int flags;
  grpc_create_socketpair_if_unix(sv);
  flags = fcntl(sv[0], F_GETFL, 0);
  CHECK_EQ(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);
  flags = fcntl(sv[1], F_GETFL, 0);
  CHECK_EQ(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK), 0);
  CHECK(grpc_set_socket_no_sigpipe_if_possible(sv[0]) == absl::OkStatus());
  CHECK(grpc_set_socket_no_sigpipe_if_possible(sv[1]) == absl::OkStatus());
}

grpc_endpoint_pair grpc_iomgr_create_endpoint_pair(
    const char* name, const grpc_channel_args* args) {
  int sv[2];
  grpc_endpoint_pair p;
  create_sockets(sv);
  grpc_core::ExecCtx exec_ctx;
  std::string final_name = absl::StrCat(name, ":client");
  auto new_args = grpc_core::CoreConfiguration::Get()
                      .channel_args_preconditioning()
                      .PreconditionChannelArgs(args);
  p.client = grpc_tcp_create(
      grpc_fd_create(sv[1], final_name.c_str(), false),
      grpc_event_engine::experimental::ChannelArgsEndpointConfig(new_args),
      "socketpair-server");
  final_name = absl::StrCat(name, ":server");
  p.server = grpc_tcp_create(
      grpc_fd_create(sv[0], final_name.c_str(), false),
      grpc_event_engine::experimental::ChannelArgsEndpointConfig(new_args),
      "socketpair-client");
  return p;
}

#endif
