#include "compilerinfra/Development.h"
#include "llvm/Support/FormatVariadic.h"
#include <cerrno>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;
using namespace compilerinfra;

struct DevelopmentScope::Impl {
  int socket = -1;
  pid_t child = -1;
  ~Impl() {
    if (socket >= 0)
      ::close(socket);
    if (child > 0)
      while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
  }
};

DevelopmentScope::DevelopmentScope(std::unique_ptr<Impl> impl)
    : impl(std::move(impl)) {}
DevelopmentScope::~DevelopmentScope() = default;

llvm::Expected<std::unique_ptr<DevelopmentScope>>
DevelopmentScope::open(llvm::ArrayRef<std::string> argv) {
  if (argv.empty())
    return llvm::createStringError("development launcher required");
  int pair[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
    return llvm::errorCodeToError(llvm::errnoAsErrorCode());
  auto state = std::make_unique<Impl>();
  state->socket = pair[0];
  std::vector<char *> args;
  for (const auto &arg : argv)
    args.push_back(const_cast<char *>(arg.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int err = posix_spawn_file_actions_init(&actions);
  if (!err) {
    err = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
    if (!err)
      err = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
    if (!err)
      err = posix_spawnp(&state->child, args.front(), &actions, nullptr,
                         args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
  }
  ::close(pair[1]);
  if (err)
    return llvm::createStringError(
        std::error_code(err, std::generic_category()),
        "starting development scope");
  return std::unique_ptr<DevelopmentScope>(
      new DevelopmentScope(std::move(state)));
}

llvm::Expected<llvm::json::Value>
DevelopmentScope::call(llvm::StringRef name, llvm::json::Object arguments) {
  llvm::json::Value request(llvm::json::Object{
      {"action", name}, {"arguments", std::move(arguments)}});
  std::string line = llvm::formatv("{0}\n", request).str();
  llvm::StringRef remaining(line);
  while (!remaining.empty()) {
    ssize_t n =
        ::send(impl->socket, remaining.data(), remaining.size(), MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      return llvm::errorCodeToError(llvm::errnoAsErrorCode());
    remaining = remaining.drop_front(n);
  }
  line.clear();
  char buffer[8192];
  while (line.empty() || line.back() != '\n') {
    ssize_t n = ::read(impl->socket, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      return llvm::errorCodeToError(llvm::errnoAsErrorCode());
    if (!n)
      return llvm::createStringError(
          "development scope closed before replying to " + name);
    line.append(buffer, n);
  }
  auto parsed = llvm::json::parse(line);
  if (!parsed)
    return parsed.takeError();
  auto *reply = parsed->getAsObject();
  if (reply) {
    if (auto message = reply->getString("error"))
      return llvm::createStringError(*message);
    if (auto *result = reply->get("result"))
      return std::move(*result);
  }
  return llvm::createStringError("invalid response to development action " +
                                 name);
}
