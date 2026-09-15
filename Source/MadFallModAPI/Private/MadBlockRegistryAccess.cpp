// Copyright MadFall. All Rights Reserved.

#include "IMadBlockRegistry.h"

#include "MadFallModAPI.h"

#include <atomic>

namespace MadFall
{
	namespace
	{
		// Set once by MadFallCore at PreDefault and cleared at shutdown. Reads
		// happen from worker threads (the serializer resolves ids off the game
		// thread), so publication has to be ordered rather than a plain store.
		std::atomic<IMadBlockRegistry*> GBlockRegistry{ nullptr };
	}

	IMadBlockRegistry* GetBlockRegistry()
	{
		return GBlockRegistry.load(std::memory_order_acquire);
	}

	void SetBlockRegistry(IMadBlockRegistry* Registry)
	{
		IMadBlockRegistry* const Previous = GBlockRegistry.exchange(Registry, std::memory_order_release);

		if (Registry != nullptr && Previous != nullptr && Previous != Registry)
		{
			// Two registries alive at once means two runtime-id spaces, and a
			// voxel resolved through one would be meaningless to the other.
			UE_LOG(LogMadFallMods, Error,
				TEXT("A second block registry was installed while one was already active. ")
				TEXT("Runtime block IDs are now ambiguous."));
		}
	}
}
