#!/usr/bin/env bash

#  This script can be used to install the Nvidia GPU Deployment Kit
#  (GDK) on a Mesos development machine. The purpose of the GDK is to
#  provide stubs for compiling code against Nvidia's latest drivers. It
#  includes all the necessary header files (nvml.h) and library files
#  (libnvidia-ml.so) necessary to build Mesos with Nvidia GPU support.
#  However, it does not actually contain any driver code, and it can't
#  be used to interact with any GPUs installed on a system. For that,
#  you will have to find the latest released drivers themselves.
#
#  We provide this script only for convenience, so that developers
#  working on non-GPU equipped systems can build code with Nvidia GPU
#  support. It is a simple wrapper around the official GDK installer
#  script provided by Nvidia, which can be downloaded here:
#
#    https://developer.nvidia.com/gpu-deployment-kit
#
#  The script itself takes a few parameters. Run it as
#  'support/install-nvidia-gdk.sh --help' to see its usage semantics.
#
#  Unfortunately, the official GDK installer script does not provide a
#  means of uninstalling very easily. Because of this, we *highly*
#  encourage installing the GDK into a self-contained directory (e.g.
#  '/opt/nvidia-gdk'), so that uninstalling is as easy as removing that
#  single directory. If you chose to update the ldcache by passing the
#  --update-ldcache flag at installation time, you will also have to
#  remove the ${NVIDIA_GDK_CONF_FILE} file, and rerun 'sudo
#  ldconfig' to fully uninstall the GDK.

# The default install path of the Nvidia GDK.
NVIDIA_GDK_INSTALL_PATH="/opt/nvidia-gdk"

# Default value for deciding whether to run ldconfig after the installation or not.
UPDATE_LDCACHE="false"

# The location of the nvidia-gdk specific ld.so.conf file when running
# ldconfig after the installation. Its parent directory of
# /etc/ld.so.conf.d is standard across ubuntu, centos, and debian.
NVIDIA_GDK_CONF_FILE="/etc/ld.so.conf.d/nvidia-gdk.conf"

# The URL to the gdk installer.
NVIDIA_GDK_INSTALLER_URL="http://developer.download.nvidia.com/compute/cuda/7.5/Prod/gdk/gdk_linux_amd64_352_79_release.run"

# The checksum of the gdk installer.
NVIDIA_GDK_INSTALLER_SUM="3fa9d17cd57119d82d4088e5cfbfcad960f12e3384e3e1a7566aeb2441e54ce4"

# Define the scripts usage semantics.
usage()
{
cat << EOF
Usage:
  $(basename ${0}) (-h | --help)
  $(basename ${0}) [ --install-dir=<path> --update-ldcache ]

Options:
  -h --help         Show this screen.
  --install-dir     The location to install the Nvidia GDK.
                    [default: ${NVIDIA_GDK_INSTALL_PATH}]
  --update-ldcache  Install a custom ld.so.conf script and run
                    ldconfig to update the ldcache after installation.
                    The 'sudo' command is used to invoke this.
                    [default: false]

Description:
  This script can be used to install the Nvidia GPU Deployment Kit
  (GDK) on a Mesos development machine. The purpose of the GDK is to
  provide stubs for compiling code against Nvidia's latest drivers. It
  includes all the necessary header files (nvml.h) and library files
  (libnvidia-ml.so) necessary to build Mesos with Nvidia GPU support.
  However, it does not actually contain any driver code, and it can't
  be used to interact with any GPUs installed on a system. For that,
  you will have to find the latest released drivers themselves.

  Unfortunately, the official GDK installer script does not provide a
  means of uninstalling very easily. Because of this, we *highly*
  encourage installing the GDK into a self-contained directory (e.g.
  '/opt/nvidia-gdk'), so that uninstalling is as easy as removing that
  single directory. If you chose to update the ldcache by passing the
  --update-ldcache flag at installation time, you will also have to
  remove the '${NVIDIA_GDK_CONF_FILE}' file and rerun 'sudo
  ldconfig' to fully uninstall the GDK.
EOF
}

# Parse any incoming arguments.
for i in "${@}"; do
  case ${i} in
    -h|--help)
        usage
        exit 0
      ;;
    --install-dir=*)
        NVIDIA_GDK_INSTALL_PATH="${i#*=}"
        shift
      ;;
    --update-ldcache)
        UPDATE_LDCACHE="true"
        shift
      ;;
    *)
        echo "Unknown option: ${i}"
        echo ""
        usage
        exit 1
      ;;
  esac
done


# Before we do anything, verify that we are running on Linux.
if [ "$(uname)" != "Linux" ]; then
  echo "Error: The Nvidia GDK is only supported on Linux."
  exit 1;
fi

# Now verify that the install-dir specified actually exists and that
# we have access to it. If it doesn't exist, attempt to create it.
if [ ! -d "${NVIDIA_GDK_INSTALL_PATH}" ]; then
  mkdir -p "${NVIDIA_GDK_INSTALL_PATH}"
fi
if [ ! -d "${NVIDIA_GDK_INSTALL_PATH}" ] ||
   [ ! -w "${NVIDIA_GDK_INSTALL_PATH}" ]; then
  echo "Error: Inaccessible path: ${NVIDIA_GDK_INSTALL_PATH}"
  echo "       The installation directory is either inaccessable or"
  echo "       cannot be created because you do not have the proper"
  echo "       permissions to access it. Please select a different"
  echo "       location, or rerun this command as a user with the"
  echo "       proper permissions, e.g. using 'sudo'."
  exit 1;
fi

# Create a place for storing temporary files and make sure this
# directory gets deleted whenever this script exits.
NVIDIA_TEMP_DIR=$(mktemp -d)
trap "rm -rf ${NVIDIA_TEMP_DIR}" EXIT

# Download and run the Nvidia GDK installer.
# Verify the installer via it's checksum after download.
NVIDIA_GDK_INSTALLER_FILE="${NVIDIA_TEMP_DIR}/gdk"

wget -O ${NVIDIA_GDK_INSTALLER_FILE} -q ${NVIDIA_GDK_INSTALLER_URL}
RESULT="${?}"

if [ ${RESULT} != "0" ]; then
  echo "Failed to download the installer!"
  echo "Error running wget, please check your internet connection:"
  echo "  \$ wget ${NVIDIA_GDK_INSTALLER_URL}"
  exit ${RESULT}
fi

echo "${NVIDIA_GDK_INSTALLER_SUM}  ${NVIDIA_GDK_INSTALLER_FILE}" \
    | sha256sum -c --strict -
RESULT="${?}"

if [ ${RESULT} != "0" ]; then
  echo "Failed verifying the checksum of the downloaded Nvidia GDK installer!"
  echo "Please report this error to dev@mesos.apache.org."
  exit ${RESULT}
fi

# Remove a symbolic link so that running this script is idempotent.
# Without removing this symbolic link, the GDK installer script errors
# out with the line:
#   'ln: failed to create symbolic link <path_to_file>: File exists'
# Since we know the next thing we will do is run the installer,
# manually removing this file should be harmless.
rm -rf ${NVIDIA_GDK_INSTALL_PATH}/usr/bin/nvvs

# Run the installer.
# We pass the '--silent' flag here to make the GDK installer happy.
# Without it, it errors out with the line:
#  'The installer must be run in silent mode to use command-line options.'
chmod +x "${NVIDIA_GDK_INSTALLER_FILE}"
${NVIDIA_GDK_INSTALLER_FILE} --installdir="${NVIDIA_GDK_INSTALL_PATH}" --silent

# Optionally update the ld cache with the Nvidia GDK library path.
if [ "${UPDATE_LDCACHE}" = "true" ]; then
  sudo bash -c "cat > ${NVIDIA_GDK_CONF_FILE} << EOF
# nvidia-gdk default configuration
${NVIDIA_GDK_INSTALL_PATH}/usr/src/gdk/nvml/lib/
EOF"

  sudo ldconfig
  echo "Wrote '${NVIDIA_GDK_CONF_FILE}' and ran ldconfig"
fi
