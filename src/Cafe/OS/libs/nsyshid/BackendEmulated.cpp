#include "BackendEmulated.h"

#include "Dimensions.h"
#include "Infinity.h"
#include "Skylander.h"
#include "config/CemuConfig.h"

namespace nsyshid::backend::emulated
{
	BackendEmulated::BackendEmulated()
	{
		cemuLog_logDebug(LogType::Force, "nsyshid::BackendEmulated: emulated backend initialised");
	}

	BackendEmulated::~BackendEmulated() = default;

	bool BackendEmulated::IsInitialisedOk()
	{
		return true;
	}

	void BackendEmulated::RefreshSkylanderPortal()
	{
		auto portal = FindDevice([](const std::shared_ptr<Device>& device) {
			return device->m_vendorId == 0x1430 && device->m_productId == 0x0150;
		});

		if (!GetConfig().emulated_usb_devices.emulate_skylander_portal)
		{
			if (portal)
				DetachDevice(portal);
			return;
		}

		if (!portal)
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Skylanders Portal");
			AttachDevice(std::make_shared<SkylanderPortalDevice>());
		}
	}

	void BackendEmulated::AttachVisibleDevices()
	{
		RefreshSkylanderPortal();
		if (GetConfig().emulated_usb_devices.emulate_infinity_base && !FindDeviceById(0x0E6F, 0x0129))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Base");
			// Add Infinity Base
			auto device = std::make_shared<InfinityBaseDevice>();
			AttachDevice(device);
		}
		if (GetConfig().emulated_usb_devices.emulate_dimensions_toypad && !FindDeviceById(0x0E6F, 0x0241))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Toypad");
			// Add Dimensions Toypad
			auto device = std::make_shared<DimensionsToypadDevice>();
			AttachDevice(device);
		}
	}
} // namespace nsyshid::backend::emulated
