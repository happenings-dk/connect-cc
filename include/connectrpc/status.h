#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_STATUS_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_STATUS_H_

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace connectrpc {
enum class Code {
  kOk = 0,
  kCanceled,
  kUnknown,
  kInvalidArgument,
  kDeadlineExceeded,
  kNotFound,
  kAlreadyExists,
  kPermissionDenied,
  kResourceExhausted,
  kFailedPrecondition,
  kAborted,
  kOutOfRange,
  kUnimplemented,
  kInternal,
  kUnavailable,
  kDataLoss,
  kUnauthenticated
};
struct ErrorDetail {
  std::string type;
  std::string value;
};
struct Status {
  Code code = Code::kOk;
  std::string message;
  std::vector<ErrorDetail> details;
  bool ok() const { return code == Code::kOk; }
};
// A result contains either a value or a non-OK status. Inspect ok() before
// value().
template <typename T>
class Result {
 public:
  explicit Result(T value) : data_(std::move(value)) {}
  Result()
      : data_(Status{Code::kUnknown, "operation did not return a value", {}}) {}
  explicit Result(Status status) : data_(Normalize(std::move(status))) {}
  bool ok() const { return std::holds_alternative<T>(data_); }
  const T* value() const { return std::get_if<T>(&data_); }
  T* value() { return std::get_if<T>(&data_); }
  Status status() const {
    const Status* status = std::get_if<Status>(&data_);
    return status ? *status : Status{};
  }

 private:
  static Status Normalize(Status status) {
    if (status.ok()) return {Code::kInternal, "result has no value", {}};
    return status;
  }
  std::variant<T, Status> data_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_STATUS_H_
