#include <cstdio>

namespace kvsd {
const char* version();
}

int main() {
  std::printf("%s\n", kvsd::version());
  return 0;
}
