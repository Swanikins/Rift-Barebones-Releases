#include "nsyshid.h"
#include "Backend.h"

namespace nsyshid::backend::emulated
{
	class BackendEmulated : public nsyshid::Backend {
	  public:
		BackendEmulated();
		~BackendEmulated();

		bool IsInitialisedOk() override;

		void RefreshSkylanderPortal();

	  protected:
		void AttachVisibleDevices() override;
	};
} // namespace nsyshid::backend::emulated
