#include "compilerinfra/Runtime.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>

static bool failed(const char *error) {
  if (!error)
    return false;
  std::cerr << error << '\n';
  ci_free(error);
  return true;
}
int main(int argc, char **argv) {
  std::map<std::string, std::string> args;
  bool layout = false, text = false, explain = false, doctor = false;
  for (int i = 1; i < argc; ++i) {
    std::string opt = argv[i];
    if (opt == "--layout" || opt == "--inspect")
      layout = true;
    else if (opt == "--doctor")
      doctor = true;
    else if (opt == "--text")
      text = true;
    else if (opt == "--explain")
      explain = true;
    else if ((opt == "--exit" || opt == "--after" || opt == "--through" ||
              opt == "--configuration" || opt == "--input" ||
              opt == "--output" || opt == "--root-sha256") &&
             i + 1 < argc)
      args[opt] = argv[++i];
    else {
      std::cerr << "unknown or incomplete argument " << opt << '\n';
      return 2;
    }
  }
  ci_compiler *raw = nullptr;
  if (failed(ci_open(args.count("--configuration")
                         ? args["--configuration"].c_str()
                         : nullptr,
                     &raw)))
    return 1;
  std::unique_ptr<ci_compiler, decltype(&ci_close)> c(raw, ci_close);
  ci_buffer output{}, events{};
  if (layout || doctor) {
    if (failed(doctor ? ci_doctor(c.get(), &output)
                      : ci_describe(c.get(), &output)))
      return 1;
  } else {
    if (!args.count("--exit")) {
      std::cerr << "expected --layout or --exit NAME\n";
      return 2;
    }
    std::ifstream file;
    if (args.count("--input")) {
      file.open(args["--input"], std::ios::binary);
      if (!file) {
        std::cerr << "cannot read " << args["--input"] << '\n';
        return 1;
      }
    }
    std::istream &in =
        file.is_open() ? static_cast<std::istream &>(file) : std::cin;
    std::string data{std::istreambuf_iterator<char>(in), {}};
    auto get = [&](const char *k) {
      return args.count(k) ? args[k].c_str() : nullptr;
    };
    auto error =
        explain
            ? ci_compile_trace(c.get(), get("--exit"), data.data(), data.size(),
                               get("--after"), get("--through"), text,
                               get("--root-sha256"), &output, &events)
            : ci_compile_plain(c.get(), get("--exit"), data.data(), data.size(),
                               get("--after"), get("--through"), text, &output,
                               &events);
    if (failed(error)) {
      if (events.data)
        std::cerr.write(static_cast<char *>(events.data), events.size) << '\n';
      ci_free(events.data);
      return 1;
    }
  }
  if (events.data)
    std::cerr.write(static_cast<char *>(events.data), events.size) << '\n';
  ci_free(events.data);
  std::ofstream file;
  if (args.count("--output"))
    file.open(args["--output"], std::ios::binary);
  std::ostream &out =
      args.count("--output") ? static_cast<std::ostream &>(file) : std::cout;
  out.write(static_cast<char *>(output.data), output.size);
  if (layout)
    out << '\n';
  ci_free(output.data);
  return out ? 0 : 1;
}
