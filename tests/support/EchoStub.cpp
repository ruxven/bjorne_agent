// A stub that echoes each line back unchanged.
//
// The process tests drive the real child process transport with it. Neither
// shell would do the job: cmd.exe prints a banner and treats every input line
// as a command, and a POSIX shell adds its own prompt and quoting. A stub that
// only copies stdin to stdout keeps the transport the only thing under test.

#include <iostream>
#include <string>

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::cout << line << '\n';
    std::cout.flush();
  }
  return 0;
}
