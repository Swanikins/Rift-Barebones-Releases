#include "nsyshid.h"
#include "Backend.h"
#include "BackendEmulated.h"
#include "BackendLibusb.h"

namespace nsyshid::backend
{
	static std::weak_ptr<backend::libusb::BackendLibusb> s_backendLibusb;
	static std::weak_ptr<backend::emulated::BackendEmulated> s_backendEmulated;
	static std::mutex s_skylanderPortalSwitchMutex;

	void AttachDefaultBackends()
	{
		// add libusb backend
		{
			auto backendLibusb = std::make_shared<backend::libusb::BackendLibusb>();
			if (backendLibusb->IsInitialisedOk())
			{
				AttachBackend(backendLibusb);
				s_backendLibusb = backendLibusb;
			}
		}
	   // add emulated backend
		{
			auto backendEmulated = std::make_shared<backend::emulated::BackendEmulated>();
			if (backendEmulated->IsInitialisedOk())
			{
				AttachBackend(backendEmulated);
				s_backendEmulated = backendEmulated;
			}
		}
	}

	void SetSkylanderPortalEmulation(bool emulatePortal)
	{
		std::lock_guard<std::mutex> switchLock(s_skylanderPortalSwitchMutex);
		cemuLog_log(LogType::Force, "Skylanders portal: switching to {} portal",
			emulatePortal ? "virtual" : "physical");

		// Detach the old portal before exposing the replacement. This ordering is
		// important for games that poll HID very frequently via the portal
		// stability graphic pack.
		if (emulatePortal)
		{
			if (auto backend = s_backendLibusb.lock())
				backend->RefreshSkylanderPortal();

			// Device removal callbacks are queued onto the emulated CPU. Reusing the
			// HID slot immediately can make the queued removal refer to the newly
			// attached portal instead. Let the removal reach the game first.
			std::this_thread::sleep_for(std::chrono::milliseconds(250));

			if (auto backend = s_backendEmulated.lock())
				backend->RefreshSkylanderPortal();
		}
		else
		{
			if (auto backend = s_backendEmulated.lock())
				backend->RefreshSkylanderPortal();

			std::this_thread::sleep_for(std::chrono::milliseconds(250));

			if (auto backend = s_backendLibusb.lock())
			{
				try
				{
					backend->RefreshSkylanderPortal();
				}
				catch (const std::exception& exception)
				{
					cemuLog_log(LogType::Force,
						"Skylanders portal: physical portal attach failed: {}", exception.what());
				}
				catch (...)
				{
					cemuLog_log(LogType::Force,
						"Skylanders portal: physical portal attach failed with an unknown exception");
				}
			}
		}

		cemuLog_log(LogType::Force, "Skylanders portal: switch completed");
	}
} // namespace nsyshid::backend
