/**
 * Copyright (C) 2016-2017 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "hal.h"

#include <dlfcn.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace hal = xrt::hal;
namespace hal2 = xrt::hal2;
namespace bfs = boost::filesystem;
//namespace pmd = xrt::pmd;

namespace {

static const char*
emptyOrValue(const char* cstr)
{
  return cstr ? cstr : "";
}

static void
directoryOrError(const bfs::path& path)
{
  if (!bfs::is_directory(path))
    throw std::runtime_error("No such directory '" + path.string() + "'");
}

XRT_UNUSED
static const char*
getPlatform()
{
#if defined(__aarch64__)
  return "aarch64";
#elif defined(__arm__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#elif defined(__powerpc64__)
  return "ppc64le";
#else
# error("No driver directory for platform")
#endif
}

static std::string&
propeFunc()
{
  static std::string sPropeFunc = "xclProbe";
  return sPropeFunc;
}

static std::string&
versionFunc()
{
  static std::string sVersionFunc = "xclVersion";
  return sVersionFunc;
}

static boost::filesystem::path&
dllExt()
{
  static boost::filesystem::path sDllExt(".so");
  return sDllExt;
}

inline bool
isDLL(const bfs::path& path)
{
  return (bfs::exists(path)
          && bfs::is_regular_file(path)
          && path.extension()==dllExt());
}

static bool
isEmulationMode()
{
  static bool val = (std::getenv("XCL_EMULATION_MODE") != nullptr);
  return val;
}

// Open the HAL implementation dll and construct a hal::device for
// each board detected by the implementation
static void
createHalDevices(hal::device_list& devices, const std::string& dll, unsigned int count=0)
{
  auto delHandle = [](void* handle) {
    dlclose(handle);
  };
  typedef std::unique_ptr<void,decltype(delHandle)> handle_type;

  auto handle = handle_type(dlopen(dll.c_str(), RTLD_LAZY | RTLD_GLOBAL),delHandle);
  if (!handle)
    throw std::runtime_error("Failed to open HAL driver '" + dll + "'\n" + dlerror());

  typedef unsigned int (* probeFuncType)();

  auto probeFunc = (probeFuncType)dlsym(handle.get(), propeFunc().c_str());
  if (!probeFunc)
    return;

  unsigned pmdCount = 0;

  if (count || (count = probeFunc()) || pmdCount) {
  // Version of HAL
    typedef unsigned int (* versionFuncType)();
    auto vFunc = (versionFuncType)dlsym(handle.get(), versionFunc().c_str());
    auto version = 1;
    if (vFunc)
      version = vFunc();

    if (version==1) {
      throw std::runtime_error("Legacy HAL version " + std::to_string(version) + " not supported");
    }
    else if (version==2) {
      hal2::createDevices(devices,dll,handle.release(),count);
    }
    else
      throw std::runtime_error("HAL version " + std::to_string(version) + " not supported");
  }
}

} // namespace

namespace xrt { namespace hal {

device::
device()
{
}

device::
~device()
{
}

hal::device_list
loadDevices()
{
  hal::device_list devices;

  // xrt
  bfs::path xrt(emptyOrValue(getenv("XILINX_XRT")));
  if (!xrt.empty() && !isEmulationMode()) {
    directoryOrError(xrt);
    bfs::path p(xrt / "lib/libxrt_core.so");
    if (isDLL(p))
      createHalDevices(devices,p.string());
  }

  if (devices.empty()) { // if failed libxrt_core load, try libxrt_aws
    bfs::path p(xrt / "lib/libxrt_aws.so");
    if (isDLL(p))
      createHalDevices(devices,p.string());
  }

  if (!xrt.empty() && isEmulationMode()) {
    directoryOrError(xrt);

    auto hw_em_driver_path = xrt::config::get_hw_em_driver();
    if (hw_em_driver_path == "null") {
      bfs::path p(xrt / "lib/libxrt_hwemu.so");
      if (isDLL(p))
        hw_em_driver_path = p.string();
    }

    if (isDLL(hw_em_driver_path))
      createHalDevices(devices,hw_em_driver_path);
  }

  if (!xrt.empty() && isEmulationMode()) {
    directoryOrError(xrt);

    auto sw_em_driver_path = xrt::config::get_sw_em_driver();
    if (sw_em_driver_path == "null") {
      bfs::path p(xrt / "lib/libxrt_swemu.so");
      if (isDLL(p))
        sw_em_driver_path = p.string();
    }

    if (isDLL(sw_em_driver_path))
      createHalDevices(devices,sw_em_driver_path);
  }

  if (xrt.empty())
    throw std::runtime_error("XILINX_XRT must be set");

  return devices;
}


// Call to load_xdp comes from two places, but the dll should be loaded only once.
// It is called from function_logger once per application run if app_debug or profile is enabled.
// It is called from device once per xclbin load, if xclbin has debug_data in it.
void
load_xdp()
{
  struct xdp_once_loader
  {
    xdp_once_loader()
    {
      bfs::path xrt(emptyOrValue(getenv("XILINX_XRT")));
      bfs::path libname ("liboclxdp.so");
      if (xrt.empty()) {
        throw std::runtime_error("Library " + libname.string() + " not found! XILINX_XRT not set");
      }
      bfs::path p(xrt / "lib");
      directoryOrError(p);
      p /= libname;
      if (!isDLL(p)) {
        throw std::runtime_error("Library " + p.string() + " not found!");
      }
      auto handle = dlopen(p.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
      if (!handle)
        throw std::runtime_error("Failed to open XDP library '" + p.string() + "'\n" + dlerror());

      typedef void (* xdpInitType)();

      const std::string s = "initXDPLib";
      auto initFunc = (xdpInitType)dlsym(handle, s.c_str());
      if (!initFunc)
        throw std::runtime_error("Failed to initialize XDP library, '" + s +"' symbol not found.\n" + dlerror());

      initFunc();
    }
  };

  // 'magic static' is thread safe per C++11
  static xdp_once_loader xdp_loaded;
}

}} // hal,xcl
