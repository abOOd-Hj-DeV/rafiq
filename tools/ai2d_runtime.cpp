#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include <nncase/functional/ai2d/ai2d_builder.h>
#include <nncase/runtime/runtime_tensor.h>

namespace {
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::k230;
using nncase::F::k230::ai2d_builder;
using Key = std::array<std::uint32_t, 8>;
std::mutex contexts_mutex;

struct Schedule {
  runtime_tensor input, output;
  std::unique_ptr<ai2d_builder> builder;
  Key key;

  explicit Schedule(const Key& parameters) : key(parameters) {
    const auto [width, height, x, y, crop_width, crop_height, target, letterbox] = key;
    dims_t input_shape{1, 3, height, width}, output_shape{1, 3, target, target};
    input = hrt::create(dt_uint8, input_shape, hrt::pool_shared).expect("AI2D input allocation failed");
    output = hrt::create(dt_uint8, output_shape, hrt::pool_shared).expect("AI2D output allocation failed");
    ai2d_datatype_t datatype{ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, dt_uint8, dt_uint8};
    ai2d_crop_param_t crop{!letterbox, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                         static_cast<std::int32_t>(crop_width), static_cast<std::int32_t>(crop_height)};
    ai2d_shift_param_t shift{false, 0};
    ai2d_pad_param_t pad{false, {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
                        ai2d_pad_mode::constant, {114, 114, 114}};
    if (letterbox) {
      const double scale = std::min(static_cast<double>(target) / width, static_cast<double>(target) / height);
      const int rw = std::max(1, static_cast<int>(std::nearbyint(width * scale)));
      const int rh = std::max(1, static_cast<int>(std::nearbyint(height * scale)));
      const int left = (static_cast<int>(target) - rw) / 2;
      const int top = (static_cast<int>(target) - rh) / 2;
      pad = {true, {{0, 0}, {0, 0}, {top, static_cast<int>(target) - rh - top},
                   {left, static_cast<int>(target) - rw - left}}, ai2d_pad_mode::constant, {114, 114, 114}};
    }
    ai2d_resize_param_t resize{true, ai2d_interp_method::tf_bilinear,
        letterbox ? ai2d_interp_mode::half_pixel : ai2d_interp_mode::align_corner};
    ai2d_affine_param_t affine{false, ai2d_interp_method::tf_nearest, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0}};
    builder = ai2d_builder::create(input_shape, output_shape, datatype, crop, shift, pad, resize, affine);
    if (!builder) throw std::runtime_error("AI2D builder allocation failed");
    builder->build_schedule().expect("AI2D schedule build failed");
  }
};

struct Context {
  std::unique_ptr<Schedule> schedule;
  std::string error;
};
}

extern "C" {
std::uint32_t k230_ai2d_abi() { return 0x52414931; }

void* k230_ai2d_create() noexcept {
  try {
    std::lock_guard<std::mutex> lock(contexts_mutex);
    auto context = std::make_unique<Context>();
    return context.release();
  } catch (...) { return nullptr; }
}

void k230_ai2d_destroy(void* opaque) noexcept {
  if (!opaque) return;
  std::lock_guard<std::mutex> lock(contexts_mutex);
  delete static_cast<Context*>(opaque);
}

const char* k230_ai2d_error(void* opaque) noexcept {
  return opaque ? static_cast<Context*>(opaque)->error.c_str() : "AI2D context unavailable";
}

int k230_ai2d_run(void* opaque, const std::uint8_t* source, std::size_t source_size,
                  const std::uint32_t* parameters, std::uint8_t* destination,
                  std::size_t destination_size) noexcept {
  if (!opaque) return -1;
  auto& context = *static_cast<Context*>(opaque);
  try {
    std::lock_guard<std::mutex> lock(contexts_mutex);
    if (!source || !parameters || !destination) throw std::invalid_argument("null AI2D buffer");
    Key key;
    std::copy_n(parameters, key.size(), key.begin());
    const auto [width, height, x, y, crop_width, crop_height, target, letterbox] = key;
    if (!width || !height || width > 4096 || height > 4096 || target < 33 || target > 4096 ||
        !crop_width || !crop_height || crop_width > width || crop_height > height ||
        x > width - crop_width || y > height - crop_height || letterbox > 1 ||
        (letterbox && (x || y || crop_width != width || crop_height != height)) ||
        source_size != std::size_t(width) * height * 3 || source_size > 12 * 1024 * 1024 ||
        destination_size != std::size_t(target) * target * 3)
      throw std::invalid_argument("invalid AI2D shape, crop, or buffer length");
    if (!context.schedule || context.schedule->key != key)
      context.schedule = std::make_unique<Schedule>(key);
    auto& schedule = *context.schedule;
    {
      auto mapping = hrt::map(schedule.input, map_write).expect("AI2D input mapping failed");
      if (mapping.buffer().size() != source_size) throw std::runtime_error("AI2D input mapping size mismatch");
      std::memcpy(mapping.buffer().data(), source, source_size);
    }
    hrt::sync(schedule.input, sync_write_back, true).expect("AI2D input cache sync failed");
    schedule.builder->invoke(schedule.input, schedule.output).expect("AI2D hardware invoke failed");
    hrt::sync(schedule.output, sync_invalidate, true).expect("AI2D output cache sync failed");
    {
      auto mapping = hrt::map(schedule.output, map_read).expect("AI2D output mapping failed");
      if (mapping.buffer().size() != destination_size) throw std::runtime_error("AI2D output mapping size mismatch");
      std::memcpy(destination, mapping.buffer().data(), destination_size);
    }
    context.error.clear();
    return 0;
  } catch (const std::exception& error) {
    context.error = error.what();
  } catch (...) {
    context.error = "unknown AI2D runtime failure";
  }
  context.schedule.reset();
  return -1;
}
}
