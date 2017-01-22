// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <linux/limits.h>

#include <string>
#include <vector>

#include <mesos/docker/spec.hpp>

#include <process/owned.hpp>

#include <stout/adaptor.hpp>
#include <stout/elf.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/mkdir.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/rmdir.hpp>
#include <stout/os/shell.hpp>

#include "linux/fs.hpp"
#include "linux/ldcache.hpp"

#include "slave/containerizer/mesos/isolators/gpu/nvml.hpp"
#include "slave/containerizer/mesos/isolators/gpu/volume.hpp"

using docker::spec::v1::ImageManifest;

using process::Owned;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// Much of the logic in this file (including the contents of the
// `BINARIES` and `LIBRARIES` arrays below) is borrowed from the
// nvidia-docker-plugin (https://github.com/NVIDIA/nvidia-docker).
// Copyright (c) 2015-2016, NVIDIA CORPORATION. All rights reserved.

static constexpr char HOST_VOLUME_PATH_PREFIX[] =
  "/var/run/mesos/isolators/gpu/nvidia_";
static constexpr char CONTAINER_VOLUME_PATH[] =
  "/usr/local/nvidia";


static constexpr const char* BINARIES[] = {
  // "nvidia-modprobe",      // Kernel module loader.
  // "nvidia-settings",      // X server settings.
  // "nvidia-xconfig",       // X xorg.conf editor.
  "nvidia-cuda-mps-control", // Multi process service CLI.
  "nvidia-cuda-mps-server",  // Multi process service server.
  "nvidia-debugdump",        // GPU coredump utility.
  "nvidia-persistenced",     // Persistence mode utility.
  "nvidia-smi",              // System management interface.
};


static constexpr const char* LIBRARIES[] = {
  // ------- X11 -------

  // "libnvidia-cfg.so",  // GPU configuration.
  // "libnvidia-gtk2.so", // GTK2.
  // "libnvidia-gtk3.so", // GTK3.
  // "libnvidia-wfb.so",  // Wrapped software rendering module for X server.
  // "libglx.so",         // GLX extension module for X server.

  // ----- Compute -----

  "libnvidia-ml.so",              // Management library.
  "libcuda.so",                   // CUDA driver library.
  "libnvidia-ptxjitcompiler.so",  // PTX-SASS JIT compiler.
  "libnvidia-fatbinaryloader.so", // fatbin loader.
  "libnvidia-opencl.so",          // NVIDIA OpenCL ICD.
  "libnvidia-compiler.so",        // NVVM-PTX compiler for OpenCL.
  // "libOpenCL.so",              // OpenCL ICD loader.

  // ------ Video ------

  "libvdpau_nvidia.so",  // NVIDIA VDPAU ICD.
  "libnvidia-encode.so", // Video encoder.
  "libnvcuvid.so",       // Video decoder.
  "libnvidia-fbc.so",    // Framebuffer capture.
  "libnvidia-ifr.so",    // OpenGL framebuffer capture.

  // ----- Graphic -----

  // In an ideal world we would only mount nvidia_* vendor specific
  // libraries and install ICD loaders inside a container. However,
  // for backwards compatibility we need to mount everything. This
  // will hopefully change once GLVND is well established.

  "libGL.so",         // OpenGL/GLX legacy _or_ compatibility wrapper (GLVND).
  "libGLX.so",        // GLX ICD loader (GLVND).
  "libOpenGL.so",     // OpenGL ICD loader (GLVND).
  "libGLESv1_CM.so",  // OpenGL ES v1 legacy _or_ ICD loader (GLVND).
  "libGLESv2.so",     // OpenGL ES v2 legacy _or_ ICD loader (GLVND).
  "libEGL.so",        // EGL ICD loader.
  "libGLdispatch.so", // OpenGL dispatch (GLVND).

  "libGLX_nvidia.so",         // OpenGL/GLX ICD (GLVND).
  "libEGL_nvidia.so",         // EGL ICD (GLVND).
  "libGLESv2_nvidia.so",      // OpenGL ES v2 ICD (GLVND).
  "libGLESv1_CM_nvidia.so",   // OpenGL ES v1 ICD (GLVND).
  "libnvidia-eglcore.so",     // EGL core.
  "libnvidia-egl-wayland.so", // EGL wayland extensions.
  "libnvidia-glcore.so",      // OpenGL core.
  "libnvidia-tls.so",         // Thread local storage.
  "libnvidia-glsi.so",        // OpenGL system interaction.
};


static Try<bool> isBlacklisted(
    const string& library,
    const Owned<elf::File>& elf)
{
  // Blacklist EGL/OpenGL libraries issued by other vendors.
  if (library == "libEGL.so" ||
      library == "libGLESv1_CM.so" ||
      library == "libGLESv2.so" ||
      library == "libGL.so") {
    Try<vector<string>> dependencies =
      elf->get_dynamic_strings(elf::DynamicTag::NEEDED);

    if (dependencies.isError()) {
      return Error("Failed reading external dependencies in ELF file"
                   " '" + library + "': " + dependencies.error());
    }

    foreach (const string& dependency, dependencies.get()) {
      if (dependency == "libGLdispatch.so" ||
          strings::startsWith(dependency, "libnvidia-gl") ||
          strings::startsWith(dependency, "libnvidia-egl")) {
        return false;
      }
    }

    return true;
  }

  // Blacklist TLS libraries using the old ABI (i.e. those != 2.3.99).
  if (library == "libnvidia-tls.so") {
    Result<Version> abi = elf->get_abi_version();
    if (!abi.isSome()) {
      return Error(
          "Failed to read ELF ABI version:"
          " " + (abi.isError() ? abi.error() : "No ABI version found"));
    }

    if (abi.get() != Version(2, 3, 99)) {
      return true;
    }
  }

  return false;
}


const string& NvidiaVolume::HOST_PATH() const
{
  return hostPath;
}


const string& NvidiaVolume::CONTAINER_PATH() const
{
  return containerPath;
}


Try<NvidiaVolume> NvidiaVolume::create()
{
  if (geteuid() != 0) {
    return Error("NvidiaVolume::create() requires root privileges");
  }

  // Append the Nvidia driver version to the name of the volume.
  Try<Nothing> initialized = nvml::initialize();
  if (initialized.isError()) {
    return Error("Failed to nvml::initialize: " + initialized.error());
  }

  Try<string> version = nvml::systemGetDriverVersion();
  if (version.isError()) {
    return Error("Failed to nvml::systemGetDriverVersion: " + version.error());
  }

  // Create the volume on the host.
  string hostPath = HOST_VOLUME_PATH_PREFIX + version.get();
  if (!os::exists(hostPath)) {
    Try<Nothing> mkdir = os::mkdir(hostPath);
    if (mkdir.isError()) {
      return Error("Failed to os::mkdir '" + hostPath + "': " + mkdir.error());
    }
  }

  // If the filesystem where we are creating this volume has the
  // `noexec` bit set, we will not be able to execute any of the
  // nvidia binaries we place in the volume (e.g. `nvidia-smi`). To
  // fix this, we mount a `tmpfs` over the volume `hostPath` without
  // the `noexec` bit set. See MESOS-5923 for more information.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  Result<string> realpath = os::realpath(hostPath);
  if (!realpath.isSome()) {
    return Error("Failed to os::realpath '" + hostPath + "':"
                 " " + (realpath.isError()
                        ? realpath.error()
                        : "No such file or directory"));
  }

  // Do a reverse search through the list of mounted filesystems to
  // find the filesystem that is mounted with the longest overlapping
  // path to our `hostPath` (which may include the `hostPath` itself).
  // Only mount a new `tmpfs` over the `hostPath` if the filesysem we
  // find is marked as `noexec`.
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table->entries)) {
    if (strings::startsWith(realpath.get(), entry.target)) {
      if (strings::contains(entry.vfsOptions, "noexec")) {
        Try<Nothing> mnt = fs::mount(
            "tmpfs", hostPath, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755");

        if (mnt.isError()) {
          return Error("Failed to mount '" + hostPath + "': " + mnt.error());
        }
      }

      break;
    }
  }

  // Create some directories in the volume if they don't yet exist.
  string directories[] = {"bin", "lib", "lib64" };
  foreach (const string& directory, directories) {
    string path = path::join(hostPath, directory);

    if (!os::exists(path)) {
      Try<Nothing> mkdir = os::mkdir(path);
      if (mkdir.isError()) {
        return Error("Failed to os::mkdir '" + path + "': " + mkdir.error());
      }
    }
  }

  // Fill in the `/bin` directory with BINARIES.
  foreach (const string& binary, BINARIES) {
    string path = path::join(hostPath, "bin", binary);

    if (!os::exists(path)) {
      string command = "which " + binary;
      Try<string> which = os::shell(command);

      if (which.isSome()) {
        which = strings::trim(which.get());

        Result<string> realpath = os::realpath(which.get());
        if (!realpath.isSome()) {
          return Error("Failed to os::realpath '" + which.get() + "':"
                       " " + (realpath.isError()
                              ? realpath.error()
                              : "No such file or directory"));
        }

        command = "cp " + realpath.get() + " " + path;
        Try<string> cp = os::shell(command);
        if (cp.isError()) {
          return Error("Failed to os::shell '" + command + "': " + cp.error());
        }
      }
    }
  }

  // Fill in the `/lib*` directories with LIBRARIES. Process all
  // versions of a library that match `lib*.so*` in the ldcache.
  Try<vector<ldcache::Entry>> cache = ldcache::parse();
  if (cache.isError()) {
    return Error("Failed to ldcache::parse: " + cache.error());
  }

  foreach (const string& library, LIBRARIES) {
    foreach (const ldcache::Entry& entry, cache.get()) {
      if (strings::startsWith(entry.name, library)) {
        // Copy the fully resolved `entry.path` (i.e. the path of the
        // library after following all symlinks) into either the
        // `/lib` folder if it is 32-bit or `/lib64` if it is 64 bit.
        Result<string> realpath = os::realpath(entry.path);
        if (!realpath.isSome()) {
          return Error("Failed to os::realpath '" + entry.path + "':"
                       " " + (realpath.isError()
                              ? realpath.error()
                              : "No such file or directory"));
        }

        Try<elf::File*> load = elf::File::load(realpath.get());
        if (load.isError()) {
          return Error("Failed to elf::File::load '" + realpath.get() + "':"
                       " " + load.error());
        }

        Owned<elf::File> file(load.get());

        // If the library is blacklisted, skip it.
        Try<bool> blacklisted = isBlacklisted(library, file);
        if (blacklisted.isError()) {
          return Error("Failed to check blacklist: " + blacklisted.error());
        }

        if (blacklisted.get()) {
          continue;
        }

        Option<string> libraryDirectory = None();

        Try<elf::Class> c = file->get_class();
        if (c.isError()) {
          return Error("Failed to get ELF class for '" + entry.name + "':"
                       " " + c.error());
        }

        if (c.get() == elf::CLASS32) {
          libraryDirectory = "lib";
        } else if (c.get() == elf::CLASS64) {
          libraryDirectory = "lib64";
        } else {
          return Error("Unknown ELF class: " + stringify(c.get()));
        }

        CHECK_SOME(libraryDirectory);

        string libraryPath = path::join(
            hostPath,
            libraryDirectory.get(),
            Path(realpath.get()).basename());

        if (!os::exists(libraryPath)) {
          string command = "cp " + realpath.get() + " " + libraryPath;
          Try<string> cp = os::shell(command);
          if (cp.isError()) {
            return Error("Failed to os::shell '" + command + "':"
                         " " + cp.error());
          }
        }

        // Set up symlinks between `entry.name` and the fully resolved
        // path we just copied. This preserves the list of libraries
        // we have on our host system in the mounted volume. If
        // `entry.path` and the fully resolved path are the same, we
        // don't make a symlink.
        string symlinkPath =
          path::join(hostPath, libraryDirectory.get(), entry.name);

        if (!os::exists(symlinkPath)) {
          Try<Nothing> symlink =
            ::fs::symlink(Path(realpath.get()).basename(), symlinkPath);
          if (symlink.isError()) {
            return Error("Failed to fs::symlink"
                         " '" + symlinkPath + "'"
                         " -> '" + Path(realpath.get()).basename() + "':"
                         " " + symlink.error());
          }
        }

        // GLVND requires an extra symlink for indirect GLX support.
        // This is a temproary workaround and won't be needed once we
        // have an indirect GLX vendor neutral library.
        //
        // TODO(klueska): Including this symlink was borrowed
        // from the `nvidia-docker-plugin` code. Remove this
        // symlink when `nvidia-docker-plugin` does the same.
        if (library == "libGLX_nvidia.so") {
          string libraryName = strings::replace(
              entry.name, "GLX_nvidia", "GLX_indirect");

          string symlinkPath =
            path::join(hostPath, libraryDirectory.get(), libraryName);

          if (!os::exists(symlinkPath)) {
            Try<Nothing> symlink =
              ::fs::symlink(Path(realpath.get()).basename(), symlinkPath);
            if (symlink.isError()) {
              return Error("Failed to fs::symlink"
                           " '" + symlinkPath + "'"
                           " -> '" + Path(realpath.get()).basename() + "':"
                           " " + symlink.error());
            }
          }
        }
      }
    }
  }

  // Return the actual volume object with the fully
  // resolved host path and the container path set.
  return NvidiaVolume(hostPath, CONTAINER_VOLUME_PATH);
}


// We use the the `com.nvidia.volumes.needed` label from
// nvidia-docker to decide if we should inject the volume or not:
//
// https://github.com/NVIDIA/nvidia-docker/wiki/Image-inspection
bool NvidiaVolume::shouldInject(const ImageManifest& manifest) const
{
  foreach (const docker::spec::v1::Label& label, manifest.config().labels()) {
    if (label.key() == "com.nvidia.volumes.needed") {
      // The label value is used as the name of the volume that
      // nvidia-docker-plugin registers with Docker. We therefore
      // don't need to use it as we simply pass the host path
      // of the volume directly.
      return true;
    }
  }

  return false;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
