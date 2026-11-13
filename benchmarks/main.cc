#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "connectrpc/callback.h"
#include "connectrpc/http_connection.h"
#include "connectrpc/rpc.h"
#include "echo.grpc.pb.h"
#include "google/protobuf/wrappers.pb.h"
#include "grpc/impl/compression_types.h"
#include "grpcpp/grpcpp.h"

namespace {
using Clock = std::chrono::steady_clock;
using Message = google::protobuf::BytesValue;
using GrpcService = connectcpp::benchmark::v1::EchoService;
constexpr char kProcedure[] = "/connectcpp.benchmark.v1.EchoService/Echo";

struct Parameters {
  std::string backend;
  int port = 0;
  int bytes = 0;
  int concurrency = 1;
  int iterations = 1000;
  int warmup = 100;
};
struct Measurements {
  std::vector<double> latency_us;
  int errors = 0;
  double seconds = 0;
};

bool ParseInt(std::string_view text, int minimum, int maximum, int& result) {
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         result >= minimum && result <= maximum;
}

// Each response is checked before it counts as a completed RPC. Payload bytes
// are identical in both directions; the schema adds the same protobuf overhead.
Message MakeRequest(int bytes) {
  Message request;
  std::string payload(bytes, '\0');
  for (int i = 0; i < bytes; ++i) payload[i] = static_cast<char>(i % 251);
  request.set_value(std::move(payload));
  return request;
}

double Microseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}

bool PrintMeasurements(const Parameters& parameters, Measurements result) {
  const size_t expected =
      static_cast<size_t>(parameters.concurrency) * parameters.iterations;
  if (result.errors != 0 || result.latency_us.size() != expected ||
      result.seconds <= 0) {
    std::cerr << "Invalid run: errors=" << result.errors
              << " completed=" << result.latency_us.size()
              << " expected=" << expected << '\n';
    return false;
  }
  std::sort(result.latency_us.begin(), result.latency_us.end());
  auto percentile = [&](double fraction) {
    size_t index = static_cast<size_t>(std::ceil(expected * fraction)) - 1;
    return result.latency_us[index];
  };
  std::cout.precision(10);
  std::cout << parameters.backend << ',' << parameters.bytes << ','
            << parameters.concurrency << ',' << parameters.iterations << ','
            << parameters.warmup << ',' << expected << ',' << result.errors
            << ',' << result.seconds << ',' << expected / result.seconds << ','
            << percentile(0.5) << ',' << percentile(0.95) << ','
            << percentile(0.99) << '\n';
  return true;
}

class NativeService final : public GrpcService::CallbackService {
 public:
  grpc::ServerUnaryReactor* Echo(grpc::CallbackServerContext* context,
                                 const Message* request,
                                 Message* response) override {
    *response = *request;
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

int NativeServer() {
  NativeService service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || port == 0) return 1;
  std::cout << port << std::endl;
  server->Wait();
  return 0;
}

asio::awaitable<connectrpc::Status> AcceptConnections(
    asio::ip::tcp::acceptor& acceptor, const connectrpc::Server& router) {
  for (;;) {
    auto [error, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (error)
      co_return connectrpc::Status{
          connectrpc::Code::kUnavailable, error.message(), {}};
    socket.set_option(asio::ip::tcp::no_delay(true), error);
    if (error)
      co_return connectrpc::Status{
          connectrpc::Code::kUnavailable, error.message(), {}};
    auto connection = connectrpc::HttpConnection::Create(
        std::move(socket), connectrpc::HttpConnection::Role::kServer,
        connectrpc::HttpVersion::kHttp2);
    if (!connection.ok()) co_return connection.status();
    connectrpc::Async(
        acceptor.get_executor(),
        connectrpc::ServeHttpConnection(
            *connection.value(),
            [&router](std::shared_ptr<connectrpc::ByteStream> stream) {
              return router.HandleHttpStream(std::move(stream));
            }),
        [](connectrpc::Status) {});
  }
}

int ConnectServer() {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::acceptor acceptor(loop);
  acceptor.open(asio::ip::tcp::v4(), error);
  if (!error) acceptor.bind({asio::ip::address_v4::loopback(), 0}, error);
  if (!error) acceptor.listen(16, error);
  if (error) return 1;
  auto endpoint = acceptor.local_endpoint(error);
  if (error) return 1;
  connectrpc::Server router;
  auto registered = router.RegisterUnary<Message, Message>(
      kProcedure,
      [](connectrpc::Request<Message> request)
          -> asio::awaitable<connectrpc::Response<Message>> {
        // Copy just as the native gRPC service does, including response
        // storage.
        co_return connectrpc::Response<Message>{{}, request.message, {}, {}};
      });
  if (!registered.ok()) return 1;
  connectrpc::Async(loop.get_executor(), AcceptConnections(acceptor, router),
                    [&](connectrpc::Status status) {
                      std::cerr << status.message << '\n';
                      loop.stop();
                    });
  std::cout << endpoint.port() << std::endl;
  loop.run();
  return 1;
}

asio::awaitable<connectrpc::Status> ConnectLane(
    const connectrpc::Client& client, const Message& request,
    connectrpc::Protocol protocol, int iterations, Measurements& result) {
  for (int i = 0; i < iterations; ++i) {
    const auto start = Clock::now();
    connectrpc::CallOptions options;
    options.protocol = protocol;
    options.transport.deadline = start + std::chrono::seconds(10);
    auto response =
        co_await client.Unary<Message, Message>(kProcedure, request, options);
    double latency = Microseconds(start);
    if (!response.status.ok() || !response.message ||
        response.message->value() != request.value()) {
      ++result.errors;
      std::cerr << "RPC failed: " << response.status.message << '\n';
      co_return connectrpc::Status{
          connectrpc::Code::kUnknown, "benchmark RPC failed", {}};
    }
    result.latency_us.push_back(latency);
  }
  co_return connectrpc::Status{};
}

Measurements ConnectPhase(asio::io_context& loop,
                          const connectrpc::Client& client,
                          const Message& request, const Parameters& parameters,
                          int iterations) {
  Measurements result;
  result.latency_us.reserve(static_cast<size_t>(parameters.concurrency) *
                            iterations);
  int remaining = parameters.concurrency;
  auto protocol = parameters.backend == "connect"
                      ? connectrpc::Protocol::kConnect
                      : connectrpc::Protocol::kGrpc;
  const auto start = Clock::now();
  for (int i = 0; i < parameters.concurrency; ++i) {
    connectrpc::Async(
        loop.get_executor(),
        ConnectLane(client, request, protocol, iterations, result),
        [&](connectrpc::Status) { --remaining; });
  }
  while (remaining > 0 && loop.run_one() != 0) {
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  if (remaining != 0) ++result.errors;
  return result;
}

int ConnectClient(const Parameters& parameters) {
  asio::io_context loop;
  asio::error_code error;
  asio::ip::tcp::socket socket(loop);
  socket.connect({asio::ip::address_v4::loopback(),
                  static_cast<uint16_t>(parameters.port)},
                 error);
  if (!error) socket.set_option(asio::ip::tcp::no_delay(true), error);
  if (error) {
    std::cerr << error.message() << '\n';
    return 1;
  }
  auto created = connectrpc::HttpConnection::Create(
      std::move(socket), connectrpc::HttpConnection::Role::kClient,
      connectrpc::HttpVersion::kHttp2);
  if (!created.ok()) return 1;
  auto connection = *created.value();
  connectrpc::Client client(
      {}, [connection](connectrpc::Metadata headers,
                       connectrpc::TransportOptions options) {
        // The runtime normally advertises gzip. This transport adapter requests
        // identity responses so both implementations measure the same workload.
        for (auto& [name, value] : headers) {
          if (name == "accept-encoding" || name == "grpc-accept-encoding")
            value = "identity";
        }
        headers.emplace(headers.begin(), ":authority", "localhost");
        headers.emplace(headers.begin(), ":scheme", "http");
        return connection->Open(std::move(headers), options.deadline);
      });
  connectrpc::Async(loop.get_executor(), connection->Run(),
                    [](connectrpc::Status) {});
  Message request = MakeRequest(parameters.bytes);
  Measurements warmup =
      ConnectPhase(loop, client, request, parameters, parameters.warmup);
  bool succeeded = warmup.errors == 0;
  if (succeeded) {
    succeeded = PrintMeasurements(
        parameters,
        ConnectPhase(loop, client, request, parameters, parameters.iterations));
  }
  connectrpc::Async(loop.get_executor(), connection->Cancel(),
                    [](connectrpc::Status) {});
  loop.run();
  return succeeded ? 0 : 1;
}

struct NativeLane {
  std::unique_ptr<grpc::ClientContext> context;
  std::unique_ptr<grpc::ClientAsyncResponseReader<Message>> reader;
  Message response;
  grpc::Status status;
  Clock::time_point start;
  int completed = 0;
};

void StartNative(GrpcService::Stub& stub, grpc::CompletionQueue& queue,
                 const Message& request, NativeLane& lane) {
  lane.start = Clock::now();
  lane.reader.reset();
  lane.context = std::make_unique<grpc::ClientContext>();
  lane.context->set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(10));
  lane.response.Clear();
  lane.status = grpc::Status{};
  lane.reader = stub.AsyncEcho(lane.context.get(), request, &queue);
  lane.reader->Finish(&lane.response, &lane.status, &lane);
}

Measurements NativePhase(GrpcService::Stub& stub, const Message& request,
                         int concurrency, int iterations) {
  Measurements result;
  result.latency_us.reserve(static_cast<size_t>(concurrency) * iterations);
  if (iterations == 0) return result;
  grpc::CompletionQueue queue;
  std::vector<NativeLane> lanes(concurrency);
  const auto start = Clock::now();
  for (NativeLane& lane : lanes) StartNative(stub, queue, request, lane);
  int remaining = concurrency;
  void* tag = nullptr;
  bool ok = false;
  while (remaining > 0 && queue.Next(&tag, &ok)) {
    auto& lane = *static_cast<NativeLane*>(tag);
    double latency = Microseconds(lane.start);
    if (!ok || !lane.status.ok() || lane.response.value() != request.value()) {
      ++result.errors;
      std::cerr << "RPC failed: " << lane.status.error_message() << '\n';
      --remaining;
      continue;
    }
    result.latency_us.push_back(latency);
    if (++lane.completed == iterations)
      --remaining;
    else
      StartNative(stub, queue, request, lane);
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  if (remaining != 0) ++result.errors;
  queue.Shutdown();
  while (queue.Next(&tag, &ok)) {
  }
  return result;
}

int NativeClient(const Parameters& parameters) {
  grpc::ChannelArguments arguments;
  arguments.SetInt("grpc.enable_retries", 0);
  arguments.SetInt(GRPC_COMPRESSION_CHANNEL_ENABLED_ALGORITHMS_BITSET,
                   1 << GRPC_COMPRESS_NONE);
  auto channel =
      grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(parameters.port),
                                grpc::InsecureChannelCredentials(), arguments);
  if (!channel->WaitForConnected(std::chrono::system_clock::now() +
                                 std::chrono::seconds(10)))
    return 1;
  auto stub = GrpcService::NewStub(channel);
  Message request = MakeRequest(parameters.bytes);
  Measurements warmup =
      NativePhase(*stub, request, parameters.concurrency, parameters.warmup);
  if (warmup.errors != 0) return 1;
  return PrintMeasurements(parameters,
                           NativePhase(*stub, request, parameters.concurrency,
                                       parameters.iterations))
             ? 0
             : 1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "server") {
    if (std::string_view(argv[2]) == "connectcpp") return ConnectServer();
    if (std::string_view(argv[2]) == "native") return NativeServer();
  }
  Parameters parameters;
  if (argc == 8 && std::string_view(argv[1]) == "client") {
    parameters.backend = argv[2];
    if (ParseInt(argv[3], 1, 65535, parameters.port) &&
        ParseInt(argv[4], 0, 1048576, parameters.bytes) &&
        ParseInt(argv[5], 1, 100, parameters.concurrency) &&
        ParseInt(argv[6], 1, 1000000, parameters.iterations) &&
        ParseInt(argv[7], 0, 10000, parameters.warmup) &&
        int64_t{parameters.concurrency} * parameters.iterations <= 2000000) {
      if (parameters.backend == "native") return NativeClient(parameters);
      if (parameters.backend == "connect" || parameters.backend == "grpc")
        return ConnectClient(parameters);
    }
  }
  std::cerr << "Usage: connectcpp_benchmark server connectcpp|native\n"
               "       connectcpp_benchmark client connect|grpc|native "
               "PORT BYTES CONCURRENCY ITERATIONS WARMUP\n";
  return 2;
}
