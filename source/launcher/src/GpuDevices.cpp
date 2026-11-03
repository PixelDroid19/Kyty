#include "GpuDevices.h"

#include <QtGui/qtguiglobal.h>

#if QT_CONFIG(vulkan)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif

namespace {

QStringList QueryGpuDeviceNames()
{
	QStringList names;
#if QT_CONFIG(vulkan)
	QVulkanInstance instance;
	if (!instance.create())
	{
		return names;
	}
	auto*    functions = instance.functions();
	uint32_t count     = 0;
	if (functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr) != VK_SUCCESS)
	{
		return names;
	}
	QVector<VkPhysicalDevice> devices(count);
	// VK_INCOMPLETE still fills the first count handles; the list may shrink between the two calls.
	const auto result = functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data());
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
	{
		return names;
	}
	devices.resize(count);
	for (auto* device: devices)
	{
		VkPhysicalDeviceProperties properties {};
		functions->vkGetPhysicalDeviceProperties(device, &properties);
		if (properties.apiVersion >= VK_API_VERSION_1_4)
		{
			names << QString::fromUtf8(properties.deviceName);
		}
	}
#endif
	return names;
}

} // namespace

QStringList GpuDeviceNames()
{
	static const QStringList names = QueryGpuDeviceNames();
	return names;
}
