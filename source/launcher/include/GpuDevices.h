#ifndef LAUNCHER_INCLUDE_GPU_DEVICES_H_
#define LAUNCHER_INCLUDE_GPU_DEVICES_H_

#include <QStringList>

// Names of the Vulkan 1.4 devices of this host, the ones the emulator can run on. Empty when
// Vulkan can't be asked. The device list is read once: Vulkan does not report hot-plug here.
QStringList GpuDeviceNames();

#endif /* LAUNCHER_INCLUDE_GPU_DEVICES_H_ */
