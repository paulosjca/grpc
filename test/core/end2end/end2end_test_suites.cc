// Copyright 2023 gRPC authors.
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

#include <grpc/compression.h>
#include <grpc/credentials.h>
#include <grpc/grpc.h>
#include <grpc/grpc_posix.h>
#include <grpc/grpc_security.h>
#include <grpc/grpc_security_constants.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/slice.h>
#include <grpc/status.h>
#include <grpc/support/time.h>
#include <inttypes.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/meta/type_traits.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "src/core/ext/transport/chaotic_good/client/chaotic_good_connector.h"
#include "src/core/ext/transport/chaotic_good/server/chaotic_good_server.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/lib/security/credentials/fake/fake_credentials.h"
#include "src/core/util/env.h"
#include "src/core/util/host_port.h"
#include "src/core/util/no_destruct.h"
#include "src/core/util/sync.h"
#include "test/core/end2end/end2end_tests.h"
#include "test/core/end2end/fixtures/h2_oauth2_common.h"
#include "test/core/end2end/fixtures/h2_ssl_cred_reload_fixture.h"
#include "test/core/end2end/fixtures/h2_ssl_tls_common.h"
#include "test/core/end2end/fixtures/h2_tls_common.h"
#include "test/core/end2end/fixtures/http_proxy_fixture.h"
#include "test/core/end2end/fixtures/inproc_fixture.h"
#include "test/core/end2end/fixtures/local_util.h"
#include "test/core/end2end/fixtures/proxy.h"
#include "test/core/end2end/fixtures/secure_fixture.h"
#include "test/core/end2end/fixtures/sockpair_fixture.h"
#include "test/core/test_util/port.h"
#include "test/core/test_util/test_config.h"
#include "test/core/test_util/tls_utils.h"

// IWYU pragma: no_include <unistd.h>

#ifdef GRPC_POSIX_SOCKET
#include <fcntl.h>

#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#endif

#ifdef GRPC_POSIX_WAKEUP_FD
#include "src/core/lib/iomgr/wakeup_fd_posix.h"
#endif

#define CA_CERT_PATH "src/core/tsi/test_creds/ca.pem"
#define SERVER_CERT_PATH "src/core/tsi/test_creds/server1.pem"
#define SERVER_KEY_PATH "src/core/tsi/test_creds/server1.key"

namespace grpc_core {

namespace {

uint64_t Rand() {
  struct State {
    Mutex mu;
    absl::BitGen gen ABSL_GUARDED_BY(mu);
  };
  static State* const state = new State;
  MutexLock lock(&state->mu);
  return absl::Uniform<uint64_t>(state->gen);
}

std::atomic<uint64_t> unique{Rand()};

void ProcessAuthFailure(void* state, grpc_auth_context* /*ctx*/,
                        const grpc_metadata* /*md*/, size_t /*md_count*/,
                        grpc_process_auth_metadata_done_cb cb,
                        void* user_data) {
  CHECK_EQ(state, nullptr);
  cb(user_data, nullptr, 0, nullptr, 0, GRPC_STATUS_UNAUTHENTICATED, nullptr);
}

void AddFailAuthCheckIfNeeded(const ChannelArgs& args,
                              grpc_server_credentials* creds) {
  if (args.Contains(FAIL_AUTH_CHECK_SERVER_ARG_NAME)) {
    grpc_auth_metadata_processor processor = {ProcessAuthFailure, nullptr,
                                              nullptr};
    grpc_server_credentials_set_auth_metadata_processor(creds, processor);
  }
}

}  // namespace

class CensusFixture : public CoreTestFixture {
 private:
  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    auto* server = grpc_server_create(
        args.Set(GRPC_ARG_ENABLE_CENSUS, true).ToC().get(), nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    CHECK(grpc_server_add_http2_port(server, localaddr_.c_str(), server_creds));
    grpc_server_credentials_release(server_creds);
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }
  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    auto* creds = grpc_insecure_credentials_create();
    auto* client =
        grpc_channel_create(localaddr_.c_str(), creds,
                            args.Set(GRPC_ARG_ENABLE_CENSUS, true).ToC().get());
    grpc_channel_credentials_release(creds);
    return client;
  }
  const std::string localaddr_ =
      JoinHostPort("localhost", grpc_pick_unused_port_or_die());
};

class CompressionFixture : public CoreTestFixture {
 private:
  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    auto* server = grpc_server_create(
        args.SetIfUnset(GRPC_COMPRESSION_CHANNEL_DEFAULT_ALGORITHM,
                        GRPC_COMPRESS_GZIP)
            .ToC()
            .get(),
        nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    CHECK(grpc_server_add_http2_port(server, localaddr_.c_str(), server_creds));
    grpc_server_credentials_release(server_creds);
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }
  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    auto* client = grpc_channel_create(
        localaddr_.c_str(), creds,
        args.SetIfUnset(GRPC_COMPRESSION_CHANNEL_DEFAULT_ALGORITHM,
                        GRPC_COMPRESS_GZIP)
            .ToC()
            .get());
    grpc_channel_credentials_release(creds);
    return client;
  }

  std::string localaddr_ =
      JoinHostPort("localhost", grpc_pick_unused_port_or_die());
};

class FakesecFixture : public SecureFixture {
 private:
  grpc_channel_credentials* MakeClientCreds(const ChannelArgs&) override {
    return grpc_fake_transport_security_credentials_create();
  }
  grpc_server_credentials* MakeServerCreds(const ChannelArgs& args) override {
    grpc_server_credentials* fake_ts_creds =
        grpc_fake_transport_security_server_credentials_create();
    AddFailAuthCheckIfNeeded(args, fake_ts_creds);
    return fake_ts_creds;
  }
};

class InsecureCredsFixture : public InsecureFixture {
 private:
  grpc_server_credentials* MakeServerCreds(const ChannelArgs& args) override {
    auto* creds = grpc_insecure_server_credentials_create();
    AddFailAuthCheckIfNeeded(args, creds);
    return creds;
  }
};

class SockpairWithMinstackFixture : public SockpairFixture {
 public:
  using SockpairFixture::SockpairFixture;

 private:
  ChannelArgs MutateClientArgs(ChannelArgs args) override {
    return args.Set(GRPC_ARG_MINIMAL_STACK, true);
  }
  ChannelArgs MutateServerArgs(ChannelArgs args) override {
    return args.Set(GRPC_ARG_MINIMAL_STACK, true);
  }
};

class Sockpair1Byte : public SockpairFixture {
 public:
  Sockpair1Byte()
      : SockpairFixture(ChannelArgs()
                            .Set(GRPC_ARG_TCP_READ_CHUNK_SIZE, 1)
                            .Set(GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE, 1)
                            .Set(GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE, 1)) {
    g_fixture_slowdown_factor = 2;
  }
  ~Sockpair1Byte() override { g_fixture_slowdown_factor = 1; }

 private:
  ChannelArgs MutateClientArgs(ChannelArgs args) override {
    return args.Set(GRPC_ARG_MINIMAL_STACK, true);
  }
  ChannelArgs MutateServerArgs(ChannelArgs args) override {
    return args.Set(GRPC_ARG_MINIMAL_STACK, true);
  }
};

#ifdef GRPC_POSIX_SOCKET

class FdFixture : public CoreTestFixture {
 public:
  FdFixture() { create_sockets(fd_pair_); }

 private:
  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    ExecCtx exec_ctx;
    auto* server = grpc_server_create(args.ToC().get(), nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    pre_server_start(server);
    grpc_server_start(server);
    grpc_server_credentials* creds = grpc_insecure_server_credentials_create();
    grpc_server_add_channel_from_fd(server, fd_pair_[1], creds);
    grpc_server_credentials_release(creds);
    return server;
  }
  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    ExecCtx exec_ctx;
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    auto* client = grpc_channel_create_from_fd("fixture_client", fd_pair_[0],
                                               creds, args.ToC().get());
    grpc_channel_credentials_release(creds);
    return client;
  }

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

  int fd_pair_[2];
};
#endif

class NoRetryFixture : public InsecureFixture {
 private:
  ChannelArgs MutateClientArgs(ChannelArgs args) override {
    return args.Set(GRPC_ARG_ENABLE_RETRIES, false);
  }
};

class HttpProxyFilter : public CoreTestFixture {
 public:
  explicit HttpProxyFilter(const ChannelArgs& client_args)
      : proxy_(grpc_end2end_http_proxy_create(client_args.ToC().get())) {}
  ~HttpProxyFilter() override { grpc_end2end_http_proxy_destroy(proxy_); }

 private:
  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    auto* server = grpc_server_create(args.ToC().get(), nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    CHECK(
        grpc_server_add_http2_port(server, server_addr_.c_str(), server_creds));
    grpc_server_credentials_release(server_creds);
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }

  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    // If testing for proxy auth, add credentials to proxy uri
    std::optional<std::string> proxy_auth_str =
        args.GetOwnedString(GRPC_ARG_HTTP_PROXY_AUTH_CREDS);
    std::string proxy_uri;
    if (!proxy_auth_str.has_value()) {
      proxy_uri = absl::StrFormat(
          "http://%s", grpc_end2end_http_proxy_get_proxy_name(proxy_));
    } else {
      proxy_uri =
          absl::StrFormat("http://%s@%s", proxy_auth_str->c_str(),
                          grpc_end2end_http_proxy_get_proxy_name(proxy_));
    }
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    auto* client = grpc_channel_create(
        server_addr_.c_str(), creds,
        args.Set(GRPC_ARG_HTTP_PROXY, proxy_uri).ToC().get());
    grpc_channel_credentials_release(creds);
    CHECK(client);
    return client;
  }

  std::string server_addr_ =
      JoinHostPort("localhost", grpc_pick_unused_port_or_die());
  grpc_end2end_http_proxy* proxy_;
};

class ProxyFixture : public CoreTestFixture {
 public:
  ProxyFixture(const ChannelArgs& client_args, const ChannelArgs& server_args)
      : proxy_(grpc_end2end_proxy_create(&proxy_def_, client_args.ToC().get(),
                                         server_args.ToC().get())) {}
  ~ProxyFixture() override { grpc_end2end_proxy_destroy(proxy_); }

 private:
  static grpc_server* CreateProxyServer(const char* port,
                                        const grpc_channel_args* server_args) {
    grpc_server* s = grpc_server_create(server_args, nullptr);
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    CHECK(grpc_server_add_http2_port(s, port, server_creds));
    grpc_server_credentials_release(server_creds);
    return s;
  }

  static grpc_channel* CreateProxyClient(const char* target,
                                         const grpc_channel_args* client_args) {
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    grpc_channel* channel = grpc_channel_create(target, creds, client_args);
    grpc_channel_credentials_release(creds);
    return channel;
  }

  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    auto* server = grpc_server_create(args.ToC().get(), nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    CHECK(grpc_server_add_http2_port(
        server, grpc_end2end_proxy_get_server_port(proxy_), server_creds));
    grpc_server_credentials_release(server_creds);
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }

  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    auto* client = grpc_channel_create(
        grpc_end2end_proxy_get_client_target(proxy_), creds, args.ToC().get());
    grpc_channel_credentials_release(creds);
    CHECK(client);
    return client;
  }
  const grpc_end2end_proxy_def proxy_def_ = {CreateProxyServer,
                                             CreateProxyClient};
  grpc_end2end_proxy* proxy_;
};

class SslProxyFixture : public CoreTestFixture {
 public:
  SslProxyFixture(const ChannelArgs& client_args,
                  const ChannelArgs& server_args)
      : proxy_(grpc_end2end_proxy_create(&proxy_def_, client_args.ToC().get(),
                                         server_args.ToC().get())) {}
  ~SslProxyFixture() override { grpc_end2end_proxy_destroy(proxy_); }

 private:
  static grpc_server* CreateProxyServer(const char* port,
                                        const grpc_channel_args* server_args) {
    grpc_server* s = grpc_server_create(server_args, nullptr);
    std::string server_cert = testing::GetFileContents(SERVER_CERT_PATH);
    std::string server_key = testing::GetFileContents(SERVER_KEY_PATH);
    grpc_ssl_pem_key_cert_pair pem_key_cert_pair = {server_key.c_str(),
                                                    server_cert.c_str()};
    grpc_server_credentials* ssl_creds = grpc_ssl_server_credentials_create(
        nullptr, &pem_key_cert_pair, 1, 0, nullptr);
    CHECK(grpc_server_add_http2_port(s, port, ssl_creds));
    grpc_server_credentials_release(ssl_creds);
    return s;
  }

  static grpc_channel* CreateProxyClient(const char* target,
                                         const grpc_channel_args* client_args) {
    grpc_channel* channel;
    grpc_channel_credentials* ssl_creds =
        grpc_ssl_credentials_create(nullptr, nullptr, nullptr, nullptr);
    grpc_arg ssl_name_override = {
        GRPC_ARG_STRING,
        const_cast<char*>(GRPC_SSL_TARGET_NAME_OVERRIDE_ARG),
        {const_cast<char*>("foo.test.google.fr")}};
    const grpc_channel_args* new_client_args =
        grpc_channel_args_copy_and_add(client_args, &ssl_name_override, 1);
    channel = grpc_channel_create(target, ssl_creds, new_client_args);
    grpc_channel_credentials_release(ssl_creds);
    {
      ExecCtx exec_ctx;
      grpc_channel_args_destroy(new_client_args);
    }
    return channel;
  }

  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    std::string server_cert = testing::GetFileContents(SERVER_CERT_PATH);
    std::string server_key = testing::GetFileContents(SERVER_KEY_PATH);
    grpc_ssl_pem_key_cert_pair pem_key_cert_pair = {server_key.c_str(),
                                                    server_cert.c_str()};
    grpc_server_credentials* ssl_creds = grpc_ssl_server_credentials_create(
        nullptr, &pem_key_cert_pair, 1, 0, nullptr);
    if (args.Contains(FAIL_AUTH_CHECK_SERVER_ARG_NAME)) {
      grpc_auth_metadata_processor processor = {ProcessAuthFailure, nullptr,
                                                nullptr};
      grpc_server_credentials_set_auth_metadata_processor(ssl_creds, processor);
    }

    auto* server = grpc_server_create(args.ToC().get(), nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    CHECK(grpc_server_add_http2_port(
        server, grpc_end2end_proxy_get_server_port(proxy_), ssl_creds));
    grpc_server_credentials_release(ssl_creds);
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }

  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    grpc_channel_credentials* ssl_creds =
        grpc_ssl_credentials_create(nullptr, nullptr, nullptr, nullptr);
    auto* client = grpc_channel_create(
        grpc_end2end_proxy_get_client_target(proxy_), ssl_creds,
        args.Set(GRPC_SSL_TARGET_NAME_OVERRIDE_ARG, "foo.test.google.fr")
            .ToC()
            .get());
    CHECK_NE(client, nullptr);
    grpc_channel_credentials_release(ssl_creds);
    return client;
  }
  const grpc_end2end_proxy_def proxy_def_ = {CreateProxyServer,
                                             CreateProxyClient};
  grpc_end2end_proxy* proxy_;
};

class FixtureWithTracing final : public CoreTestFixture {
 public:
  explicit FixtureWithTracing(std::unique_ptr<CoreTestFixture> fixture)
      : fixture_(std::move(fixture)) {
    // g_fixture_slowdown_factor = 10;
    EXPECT_FALSE(grpc_tracer_set_enabled("doesnt-exist", 0));
    EXPECT_TRUE(grpc_tracer_set_enabled("http", 1));
    EXPECT_TRUE(grpc_tracer_set_enabled("all", 1));
  }
  ~FixtureWithTracing() override {
    saved_trace_flags_.Restore();
    // g_fixture_slowdown_factor = 1;
  }

  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    return fixture_->MakeServer(args, cq, pre_server_start);
  }

  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue* cq) override {
    return fixture_->MakeClient(args, cq);
  }

 private:
  SavedTraceFlags saved_trace_flags_;
  std::unique_ptr<CoreTestFixture> fixture_;
};

class ChaoticGoodFixture : public CoreTestFixture {
 public:
  explicit ChaoticGoodFixture(int data_connections = 1, int chunk_size = 0,
                              std::string localaddr = JoinHostPort(
                                  "localhost", grpc_pick_unused_port_or_die()))
      : data_connections_(data_connections),
        chunk_size_(chunk_size),
        localaddr_(std::move(localaddr)) {}

 protected:
  const std::string& localaddr() const { return localaddr_; }

 private:
  grpc_server* MakeServer(
      const ChannelArgs& args, grpc_completion_queue* cq,
      absl::AnyInvocable<void(grpc_server*)>& pre_server_start) override {
    auto* server = grpc_server_create(
        args.Set(GRPC_ARG_CHAOTIC_GOOD_DATA_CONNECTIONS, data_connections_)
            .Set(GRPC_ARG_CHAOTIC_GOOD_MAX_RECV_CHUNK_SIZE, chunk_size_)
            .Set(GRPC_ARG_CHAOTIC_GOOD_MAX_SEND_CHUNK_SIZE, chunk_size_)
            .ToC()
            .get(),
        nullptr);
    grpc_server_register_completion_queue(server, cq, nullptr);
    CHECK(grpc_server_add_chaotic_good_port(server, localaddr_.c_str()));
    pre_server_start(server);
    grpc_server_start(server);
    return server;
  }

  grpc_channel* MakeClient(const ChannelArgs& args,
                           grpc_completion_queue*) override {
    auto* client = grpc_chaotic_good_channel_create(
        localaddr_.c_str(),
        args.Set(GRPC_ARG_CHAOTIC_GOOD_MAX_RECV_CHUNK_SIZE, chunk_size_)
            .Set(GRPC_ARG_CHAOTIC_GOOD_MAX_SEND_CHUNK_SIZE, chunk_size_)
            .SetIfUnset(GRPC_ARG_ENABLE_RETRIES, IsRetryInCallv3Enabled())
            .ToC()
            .get());
    return client;
  }

  int data_connections_;
  int chunk_size_;
  std::string localaddr_;
};

class ChaoticGoodSingleConnectionFixture final : public ChaoticGoodFixture {
 public:
  ChaoticGoodSingleConnectionFixture() : ChaoticGoodFixture(1) {}
};

class ChaoticGoodManyConnectionFixture final : public ChaoticGoodFixture {
 public:
  ChaoticGoodManyConnectionFixture() : ChaoticGoodFixture(16) {}
};

class ChaoticGoodOneByteChunkFixture final : public ChaoticGoodFixture {
 public:
  ChaoticGoodOneByteChunkFixture() : ChaoticGoodFixture(1, 1) {}
};

#ifdef GRPC_POSIX_WAKEUP_FD
class InsecureFixtureWithPipeForWakeupFd : public InsecureFixture {
 public:
  InsecureFixtureWithPipeForWakeupFd()
      : old_value_(std::exchange(grpc_allow_specialized_wakeup_fd, 0)) {}

  ~InsecureFixtureWithPipeForWakeupFd() override {
    grpc_allow_specialized_wakeup_fd = old_value_;
  }

 private:
  const int old_value_;
};
#endif

// Returns the temp directory to create uds in this test.
std::string GetTempDir() {
#ifdef GPR_WINDOWS
  // Windows temp dir usually exceeds uds max path length,
  // so we create a short dir for this test.
  // TODO: find a better solution.
  std::string temp_dir = "C:/tmp/";
  if (CreateDirectoryA(temp_dir.c_str(), NULL) == 0 &&
      ERROR_ALREADY_EXISTS != GetLastError()) {
    Crash(absl::StrCat("Could not create temp dir: ", temp_dir));
  }
  return temp_dir;
#else
  return "/tmp/";
#endif  // GPR_WINDOWS
}

const std::string temp_dir = GetTempDir();

std::vector<CoreTestConfiguration> DefaultConfigs() {
  return std::vector<CoreTestConfiguration>{
#ifdef GRPC_POSIX_SOCKET
      CoreTestConfiguration{"Chttp2Fd",
                            FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ |
                                FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
                            nullptr,
                            [](const ChannelArgs&, const ChannelArgs&) {
                              return std::make_unique<FdFixture>();
                            }},
#endif
      CoreTestConfiguration{
          "Chttp2FakeSecurityFullstack",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS_LEVEL_INSECURE |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<FakesecFixture>();
          }},
      CoreTestConfiguration{
          "Chttp2Fullstack",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2, nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<InsecureFixture>();
          }},
      CoreTestConfiguration{"Chttp2FullstackCompression",
                            FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                                FEATURE_MASK_IS_HTTP2 |
                                FEATURE_MASK_DO_NOT_GTEST,
                            nullptr,
                            [](const ChannelArgs&, const ChannelArgs&) {
                              return std::make_unique<CompressionFixture>();
                            }},
#ifdef GPR_LINUX
      CoreTestConfiguration{
          "Chttp2FullstackLocalAbstractUdsPercentEncoded",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
            return std::make_unique<LocalTestFixture>(
                absl::StrFormat(
                    "unix-abstract:grpc_fullstack_test.%%00.%d.%" PRId64
                    ".%" PRId32 ".%" PRId64 ".%" PRId64,
                    getpid(), now.tv_sec, now.tv_nsec,
                    unique.fetch_add(1, std::memory_order_relaxed), Rand()),
                UDS);
          }},
#endif

      CoreTestConfiguration{"Chttp2FullstackLocalIpv4",
                            FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                                FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
                                FEATURE_MASK_IS_HTTP2 |
                                FEATURE_MASK_DO_NOT_FUZZ |
                                FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
                                FEATURE_MASK_IS_LOCAL_TCP_CREDS,
                            nullptr,
                            [](const ChannelArgs& /*client_args*/,
                               const ChannelArgs& /*server_args*/) {
                              int port = grpc_pick_unused_port_or_die();
                              return std::make_unique<LocalTestFixture>(
                                  JoinHostPort("127.0.0.1", port), LOCAL_TCP);
                            }},
      CoreTestConfiguration{"Chttp2FullstackLocalIpv6",
                            FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                                FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
                                FEATURE_MASK_IS_HTTP2 |
                                FEATURE_MASK_DO_NOT_FUZZ |
                                FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
                                FEATURE_MASK_IS_LOCAL_TCP_CREDS,
                            nullptr,
                            [](const ChannelArgs& /*client_args*/,
                               const ChannelArgs& /*server_args*/) {
                              int port = grpc_pick_unused_port_or_die();
                              return std::make_unique<LocalTestFixture>(
                                  JoinHostPort("[::1]", port), LOCAL_TCP);
                            }},
#ifdef GRPC_HAVE_UNIX_SOCKET
      CoreTestConfiguration{
          "Chttp2FullstackLocalUdsPercentEncoded",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
            return std::make_unique<LocalTestFixture>(
                absl::StrFormat("unix:%s"
                                "grpc_fullstack_test.%%25.%d.%" PRId64
                                ".%" PRId32 ".%" PRId64 ".%" PRId64,
                                temp_dir, getpid(), now.tv_sec, now.tv_nsec,
                                unique.fetch_add(1, std::memory_order_relaxed),
                                Rand()),
                UDS);
          }},
      CoreTestConfiguration{
          "Chttp2FullstackLocalUds",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            gpr_timespec now = gpr_now(GPR_CLOCK_REALTIME);
            return std::make_unique<LocalTestFixture>(
                absl::StrFormat("unix:%s"
                                "grpc_fullstack_test.%d.%" PRId64 ".%" PRId32
                                ".%" PRId64 ".%" PRId64,
                                temp_dir, getpid(), now.tv_sec, now.tv_nsec,
                                unique.fetch_add(1, std::memory_order_relaxed),
                                Rand()),
                UDS);
          }},
#endif
      CoreTestConfiguration{
          "Chttp2FullstackNoRetry",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DOES_NOT_SUPPORT_RETRY | FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<NoRetryFixture>();
          }},
      CoreTestConfiguration{"Chttp2FullstackWithCensus",
                            FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                                FEATURE_MASK_IS_HTTP2 |
                                FEATURE_MASK_DO_NOT_GTEST,
                            nullptr,
                            [](const ChannelArgs&, const ChannelArgs&) {
                              return std::make_unique<CensusFixture>();
                            }},
      CoreTestConfiguration{
          "Chttp2FullstackWithProxy",
          FEATURE_MASK_SUPPORTS_REQUEST_PROXYING |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_FUZZ,
          nullptr,
          [](const ChannelArgs& client_args, const ChannelArgs& server_args) {
            return std::make_unique<ProxyFixture>(client_args, server_args);
          }},
      CoreTestConfiguration{
          "Chttp2HttpProxy",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_FUZZ,
          nullptr,
          [](const ChannelArgs& client_args, const ChannelArgs&) {
            return std::make_unique<HttpProxyFilter>(client_args);
          }},
      CoreTestConfiguration{
          "Chttp2SslProxy",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_SECURE |
              FEATURE_MASK_SUPPORTS_REQUEST_PROXYING |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ,
          "foo.test.google.fr",
          [](const ChannelArgs& client_args, const ChannelArgs& server_args) {
            return std::make_unique<SslProxyFixture>(client_args, server_args);
          }},
      CoreTestConfiguration{
          "Chttp2InsecureCredentials",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS_LEVEL_INSECURE |
              FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
              FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<InsecureCredsFixture>();
          },
      },
      CoreTestConfiguration{
          "Chttp2SimpleSslWithOauth2FullstackTls12",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
              FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<Oauth2Fixture>(grpc_tls_version::TLS1_2);
          }},
      CoreTestConfiguration{
          "Chttp2SimpleSslWithOauth2FullstackTls13",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<Oauth2Fixture>(grpc_tls_version::TLS1_3);
          }},
      CoreTestConfiguration{
          "Chttp2SimplSslFullstackTls12",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
              FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SslTlsFixture>(grpc_tls_version::TLS1_2);
          }},
      CoreTestConfiguration{
          "Chttp2SimplSslFullstackTls13",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_DOES_NOT_SUPPORT_CLIENT_HANDSHAKE_COMPLETE_FIRST |
              FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SslTlsFixture>(grpc_tls_version::TLS1_3);
          }},
      CoreTestConfiguration{"Chttp2SocketPair",
                            FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_DO_NOT_FUZZ |
                                FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
                            nullptr,
                            [](const ChannelArgs&, const ChannelArgs&) {
                              return std::make_unique<SockpairFixture>(
                                  ChannelArgs());
                            }},
      CoreTestConfiguration{
          "Chttp2SocketPair1ByteAtATime",
          FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_1BYTE_AT_A_TIME |
              FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SockpairFixture>(
                ChannelArgs()
                    .Set(GRPC_ARG_TCP_READ_CHUNK_SIZE, 1)
                    .Set(GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE, 1)
                    .Set(GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE, 1));
          }},
      CoreTestConfiguration{
          "Chttp2SocketPairMinstack",
          FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_IS_MINSTACK |
              FEATURE_MASK_DO_NOT_FUZZ,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SockpairWithMinstackFixture>(ChannelArgs());
          }},
      CoreTestConfiguration{
          "Inproc",
          FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
              FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<InprocFixture>(false);
          },
      },
      CoreTestConfiguration{
          "InprocWithPromises",
          FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
              FEATURE_MASK_IS_CALL_V3 | FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<InprocFixture>(true);
          },
      },
      CoreTestConfiguration{
          "Chttp2SslCredReloadTls12",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS |
              FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SslCredReloadFixture>(TLS1_2);
          }},
      CoreTestConfiguration{
          "Chttp2SslCredReloadTls13",
          FEATURE_MASK_IS_SECURE | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS |
              FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_DOES_NOT_SUPPORT_CLIENT_HANDSHAKE_COMPLETE_FIRST |
              FEATURE_MASK_DO_NOT_GTEST,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<SslCredReloadFixture>(TLS1_3);
          }},
      CoreTestConfiguration{
          // client: certificate watcher provider + async external verifier
          // server: certificate watcher provider + async external verifier
          // extra: TLS 1.3
          "Chttp2CertWatcherProviderAsyncVerifierTls13",
          kH2TLSFeatureMask | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<TlsFixture>(
                SecurityPrimitives::TlsVersion::V_13,
                SecurityPrimitives::ProviderType::FILE_PROVIDER,
                SecurityPrimitives::VerifierType::EXTERNAL_ASYNC_VERIFIER);
          },
      },
      CoreTestConfiguration{
          // client: certificate watcher provider + hostname verifier
          // server: certificate watcher provider + sync external verifier
          // extra: TLS 1.2
          "Chttp2CertWatcherProviderSyncVerifierTls12",
          kH2TLSFeatureMask | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<TlsFixture>(
                SecurityPrimitives::TlsVersion::V_12,
                SecurityPrimitives::ProviderType::FILE_PROVIDER,
                SecurityPrimitives::VerifierType::HOSTNAME_VERIFIER);
          },
      },
      CoreTestConfiguration{
          // client: static data provider + sync external verifier
          // server: static data provider + sync external verifier
          // extra: TLS 1.2
          "Chttp2SimpleSslFullstack",
          kH2TLSFeatureMask,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<TlsFixture>(
                SecurityPrimitives::TlsVersion::V_12,
                SecurityPrimitives::ProviderType::STATIC_PROVIDER,
                SecurityPrimitives::VerifierType::EXTERNAL_SYNC_VERIFIER);
          },
      },
      CoreTestConfiguration{
          // client: static data provider + async external verifier
          // server: static data provider + async external verifier
          // extra: TLS 1.3
          "Chttp2StaticProviderAsyncVerifierTls13",
          kH2TLSFeatureMask | FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          "foo.test.google.fr",
          [](const ChannelArgs&, const ChannelArgs&) {
            return std::make_unique<TlsFixture>(
                SecurityPrimitives::TlsVersion::V_13,
                SecurityPrimitives::ProviderType::STATIC_PROVIDER,
                SecurityPrimitives::VerifierType::EXTERNAL_ASYNC_VERIFIER);
          },
      },
#ifdef GPR_LINUX
      CoreTestConfiguration{
          "Chttp2FullstackUdsAbstractNamespace",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            gpr_timespec now = gpr_now(GPR_CLOCK_REALTIME);
            return std::make_unique<InsecureFixture>(absl::StrFormat(
                "unix-abstract:grpc_fullstack_test.%d.%" PRId64 ".%" PRId32
                ".%" PRId64,
                getpid(), now.tv_sec, now.tv_nsec,
                unique.fetch_add(1, std::memory_order_relaxed)));
          }},
#endif
#ifdef GRPC_HAVE_UNIX_SOCKET
      CoreTestConfiguration{
          "Chttp2FullstackUds",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_FUZZ,
          nullptr,
          [](const ChannelArgs&, const ChannelArgs&) {
            gpr_timespec now = gpr_now(GPR_CLOCK_REALTIME);
            return std::make_unique<InsecureFixture>(absl::StrFormat(
                "unix:%s"
                "grpc_fullstack_test.%d.%" PRId64 ".%" PRId32 ".%" PRId64
                ".%" PRId64,
                temp_dir, getpid(), now.tv_sec, now.tv_nsec,
                unique.fetch_add(1, std::memory_order_relaxed), Rand()));
          }},
#endif
// TODO(ctiller): these got inadvertently disabled when the project
// switched to Bazel in 2016, and have not been re-enabled since and are now
// quite broken. We should re-enable them however, as they provide defense in
// depth that enabling tracers is safe. When doing so, we'll need to re-enable
// the windows setvbuf statement in main().
#if 0
    CoreTestConfiguration{
        "Chttp2SocketPairWithTrace",
        FEATURE_MASK_IS_HTTP2 | FEATURE_MASK_ENABLES_TRACES, nullptr,
        [](const ChannelArgs&, const ChannelArgs&) {
          return std::make_unique<FixtureWithTracing>(
              std::make_unique<SockpairFixture>(ChannelArgs()));
        }},
    CoreTestConfiguration{"Chttp2FullstackWithTrace",
                          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                              FEATURE_MASK_IS_HTTP2 |
                              FEATURE_MASK_ENABLES_TRACES,
                          nullptr,
                          [](const ChannelArgs& /*client_args*/,
                             const ChannelArgs& /*server_args*/) {
                            return std::make_unique<FixtureWithTracing>(
                                std::make_unique<InsecureFixture>());
                          }},
#endif
#ifdef GRPC_POSIX_WAKEUP_FD
      CoreTestConfiguration{
          "Chttp2FullstackWithPipeWakeup",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_IS_HTTP2 |
              FEATURE_MASK_DO_NOT_FUZZ |
              FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<InsecureFixtureWithPipeForWakeupFd>();
          }},
#endif
#ifndef GPR_WINDOWS
      CoreTestConfiguration{"ChaoticGoodFullStack",
                            FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
                                FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
                                FEATURE_MASK_IS_CALL_V3,
                            nullptr,
                            [](const ChannelArgs& /*client_args*/,
                               const ChannelArgs& /*server_args*/) {
                              return std::make_unique<ChaoticGoodFixture>();
                            }},
      CoreTestConfiguration{
          "ChaoticGoodManyConnections",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_DOES_NOT_SUPPORT_RETRY |
              FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
              FEATURE_MASK_IS_CALL_V3,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<ChaoticGoodManyConnectionFixture>();
          }},
      CoreTestConfiguration{
          "ChaoticGoodSingleConnection",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL |
              FEATURE_MASK_DOES_NOT_SUPPORT_RETRY |
              FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
              FEATURE_MASK_IS_CALL_V3 | FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<ChaoticGoodSingleConnectionFixture>();
          }},
      CoreTestConfiguration{
          "ChaoticGoodOneByteChunk",
          FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL | FEATURE_MASK_1BYTE_AT_A_TIME |
              FEATURE_MASK_DOES_NOT_SUPPORT_RETRY |
              FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING |
              FEATURE_MASK_IS_CALL_V3 | FEATURE_MASK_DO_NOT_GTEST,
          nullptr,
          [](const ChannelArgs& /*client_args*/,
             const ChannelArgs& /*server_args*/) {
            return std::make_unique<ChaoticGoodOneByteChunkFixture>();
          }},
#endif  // GPR_WINDOWS
  };
}

std::vector<CoreTestConfiguration> AllConfigs() {
  std::vector<CoreTestConfiguration> configs = DefaultConfigs();
  for (const auto& config : configs) {
    // Setting both no gtest && no fuzz == no config -- better to delete it
    CHECK_NE(config.feature_mask &
                 (FEATURE_MASK_DO_NOT_FUZZ | FEATURE_MASK_DO_NOT_GTEST),
             static_cast<uint32_t>(FEATURE_MASK_DO_NOT_FUZZ |
                                   FEATURE_MASK_DO_NOT_GTEST))
        << "Config specified with no fuzz, no gtest: " << config.name;
  }
  std::sort(configs.begin(), configs.end(),
            [](const CoreTestConfiguration& a, const CoreTestConfiguration& b) {
              return strcmp(a.name, b.name) < 0;
            });
  return configs;
}

static NoDestruct<std::vector<CoreTestConfiguration>> kConfigs(AllConfigs());

const CoreTestConfiguration* CoreTestConfigurationNamed(
    absl::string_view name) {
  for (const CoreTestConfiguration& config : *kConfigs) {
    if (config.name == name) return &config;
  }
  return nullptr;
}

// A ConfigQuery queries a database a set of test configurations
// that match some criteria.
class ConfigQuery {
 public:
  explicit ConfigQuery(bool fuzzing) {
    if (GetEnv("GRPC_CI_EXPERIMENTS").has_value()) {
      exclude_features_ |= FEATURE_MASK_EXCLUDE_FROM_EXPERIMENT_RUNS;
    }
    if (fuzzing) {
      exclude_features_ |= FEATURE_MASK_DO_NOT_FUZZ;
    } else {
      exclude_features_ |= FEATURE_MASK_DO_NOT_GTEST;
    }
  }
  ConfigQuery(const ConfigQuery&) = delete;
  ConfigQuery& operator=(const ConfigQuery&) = delete;
  // Enforce that the returned configurations have the given features.
  ConfigQuery& EnforceFeatures(uint32_t features) {
    enforce_features_ |= features;
    return *this;
  }
  // Enforce that the returned configurations do not have the given features.
  ConfigQuery& ExcludeFeatures(uint32_t features) {
    exclude_features_ |= features;
    return *this;
  }
  // Enforce that the returned configurations have the given name (regex).
  ConfigQuery& AllowName(const std::string& name) {
    allowed_names_.emplace_back(
        std::regex(name, std::regex_constants::ECMAScript));
    return *this;
  }
  // Enforce that the returned configurations do not have the given name
  // (regex).
  ConfigQuery& ExcludeName(const std::string& name) {
    excluded_names_.emplace_back(
        std::regex(name, std::regex_constants::ECMAScript));
    return *this;
  }

  auto Run() const {
    std::vector<const CoreTestConfiguration*> out;
    for (const CoreTestConfiguration& config : *kConfigs) {
      if ((config.feature_mask & enforce_features_) == enforce_features_ &&
          (config.feature_mask & exclude_features_) == 0) {
        bool allowed = allowed_names_.empty();
        for (const std::regex& re : allowed_names_) {
          if (std::regex_match(config.name, re)) {
            allowed = true;
            break;
          }
        }
        for (const std::regex& re : excluded_names_) {
          if (std::regex_match(config.name, re)) {
            allowed = false;
            break;
          }
        }
        if (allowed) {
          out.push_back(&config);
        }
      }
    }
    return out;
  }

 private:
  uint32_t enforce_features_ = 0;
  uint32_t exclude_features_ = 0;
  std::vector<std::regex> allowed_names_;
  std::vector<std::regex> excluded_names_;
};

CORE_END2END_TEST_SUITE(CoreEnd2endTests, ConfigQuery(fuzzing).Run());

CORE_END2END_TEST_SUITE(
    SecureEnd2endTests,
    ConfigQuery(fuzzing).EnforceFeatures(FEATURE_MASK_IS_SECURE).Run());

CORE_END2END_TEST_SUITE(CoreLargeSendTests,
                        ConfigQuery(fuzzing)
                            .ExcludeFeatures(FEATURE_MASK_1BYTE_AT_A_TIME |
                                             FEATURE_MASK_ENABLES_TRACES)
                            .Run());

CORE_END2END_TEST_SUITE(
    CoreDeadlineTests,
    ConfigQuery(fuzzing).ExcludeFeatures(FEATURE_MASK_IS_MINSTACK).Run());

CORE_END2END_TEST_SUITE(
    CoreDeadlineSingleHopTests,
    ConfigQuery(fuzzing)
        .ExcludeFeatures(FEATURE_MASK_SUPPORTS_REQUEST_PROXYING |
                         FEATURE_MASK_IS_MINSTACK)
        .Run());

CORE_END2END_TEST_SUITE(
    CoreClientChannelTests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL)
        .Run());

CORE_END2END_TEST_SUITE(
    Http2SingleHopTests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(FEATURE_MASK_IS_HTTP2)
        .ExcludeFeatures(FEATURE_MASK_SUPPORTS_REQUEST_PROXYING |
                         FEATURE_MASK_ENABLES_TRACES)
        .Run());

CORE_END2END_TEST_SUITE(
    Http2FullstackSingleHopTests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(FEATURE_MASK_IS_HTTP2)
        .EnforceFeatures(FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL)
        .ExcludeFeatures(FEATURE_MASK_SUPPORTS_REQUEST_PROXYING)
        .Run());

CORE_END2END_TEST_SUITE(
    RetryTests, ConfigQuery(fuzzing)
                    .EnforceFeatures(FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL)
                    .ExcludeFeatures(FEATURE_MASK_DOES_NOT_SUPPORT_RETRY)
                    .Run());

CORE_END2END_TEST_SUITE(
    WriteBufferingTests,
    ConfigQuery(fuzzing)
        .ExcludeFeatures(FEATURE_MASK_DOES_NOT_SUPPORT_WRITE_BUFFERING)
        .Run());

CORE_END2END_TEST_SUITE(
    Http2Tests,
    ConfigQuery(fuzzing).EnforceFeatures(FEATURE_MASK_IS_HTTP2).Run());

CORE_END2END_TEST_SUITE(
    RetryHttp2Tests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(FEATURE_MASK_IS_HTTP2 |
                         FEATURE_MASK_SUPPORTS_CLIENT_CHANNEL)
        .ExcludeFeatures(FEATURE_MASK_DOES_NOT_SUPPORT_RETRY |
                         FEATURE_MASK_SUPPORTS_REQUEST_PROXYING)
        .Run());

CORE_END2END_TEST_SUITE(
    ResourceQuotaTests,
    ConfigQuery(fuzzing)
        .ExcludeFeatures(FEATURE_MASK_SUPPORTS_REQUEST_PROXYING |
                         FEATURE_MASK_1BYTE_AT_A_TIME)
        .ExcludeName("Chttp2.*Uds.*")
        .ExcludeName("Chttp2HttpProxy")
        .Run());

CORE_END2END_TEST_SUITE(
    PerCallCredsTests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS)
        .Run());

CORE_END2END_TEST_SUITE(
    PerCallCredsOnInsecureTests,
    ConfigQuery(fuzzing)
        .EnforceFeatures(
            FEATURE_MASK_SUPPORTS_PER_CALL_CREDENTIALS_LEVEL_INSECURE)
        .Run());

CORE_END2END_TEST_SUITE(
    NoLoggingTests,
    ConfigQuery(fuzzing).ExcludeFeatures(FEATURE_MASK_ENABLES_TRACES).Run());

CORE_END2END_TEST_SUITE(
    ProxyAuthTests, ConfigQuery(fuzzing).AllowName("Chttp2HttpProxy").Run());

void EnsureSuitesLinked() {}

}  // namespace grpc_core
