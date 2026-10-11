#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "Usage: k230-shmem-read <device> <offset> <size>\n");
    return 2;
  }
  char* end = nullptr;
  errno = 0;
  const auto offset = std::strtoull(argv[2], &end, 0);
  if (errno || !*argv[2] || *end || *argv[2] == '-' || offset > INT64_MAX) return 2;
  const auto size = std::strtoull(argv[3], &end, 0);
  if (errno || !*argv[3] || *end || *argv[3] == '-' || !size || size > 16 * 1024 * 1024)
    return 2;
  const auto page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 1;
  const auto aligned = offset / page_size * page_size;
  const auto shift = offset - aligned;
  const auto length = static_cast<std::size_t>(shift + size);
  const auto fd = open(argv[1], O_RDONLY | O_SYNC);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  void* memory = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, aligned);
  close(fd);
  if (memory == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const auto* data = static_cast<const char*>(memory) + shift;
  std::size_t sent = 0;
  int status = 0;
  while (sent < size) {
    const auto written = write(STDOUT_FILENO, data + sent, size - sent);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      std::perror("write");
      status = 1;
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  munmap(memory, length);
  return status;
}
