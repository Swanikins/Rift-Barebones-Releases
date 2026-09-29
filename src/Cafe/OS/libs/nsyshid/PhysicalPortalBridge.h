// Adapted for Rift from rhsts/cemu-hybrid-skylander-portal (hybrid-portal).
// SPDX-License-Identifier: MPL-2.0
// Rift changes: acknowledged writes, UID guards, bounded queue, asynchronous discovery.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>

#include "Common/precompiled.h"

namespace nsyshid
{



























	class PhysicalPortalBridge
	{
	  public:
		static constexpr uint16 PORTAL_VID = 0x1430;
		static constexpr uint16 PORTAL_PID = 0x0150;
		static constexpr uint8 SLOT_COUNT = 16;
		static constexpr uint8 BLOCK_COUNT = 0x40;
		static constexpr uint8 BLOCK_SIZE = 0x10;
		static constexpr uint32 FIGURE_SIZE = BLOCK_COUNT * BLOCK_SIZE;



		using AddCallback = std::function<void(uint8 portalIndex, const std::array<uint8, FIGURE_SIZE>& data)>;
		using RemoveCallback = std::function<void(uint8 portalIndex)>;

		PhysicalPortalBridge() = default;
		~PhysicalPortalBridge();

		PhysicalPortalBridge(const PhysicalPortalBridge&) = delete;
		PhysicalPortalBridge& operator=(const PhysicalPortalBridge&) = delete;

		void SetCallbacks(AddCallback onAdd, RemoveCallback onRemove);



		bool Start();
		void Stop();


		bool IsConnected() const { return m_connected.load(); }


		void SetColor(uint8 r, uint8 g, uint8 b);
		using WriteCallback = std::function<void(bool)>;
		void QueueWrite(uint8 portalIndex, uint8 block, uint32 serial, const uint8* data16, WriteCallback done);
		void QueueAudio(const uint8* data, uint32 len);


		void QueueCommand(const uint8* data, uint32 len);

	  private:
		struct OutCommand
		{
			std::array<uint8, 32> bytes{};
			uint32 serial{};
			WriteCallback done;
		};

		void FinishWrite(bool success);
		bool BeginWriteAttempt();
		void RetryOrFinishWrite();
		void ThreadMain();
		void AudioThreadMain();
		void InitHandshake();
		void DrainFor(uint32 ms);
		void Step();
		void HandleIncoming(const uint8* buf, uint32 len);
		void HandleStatus(const uint8* buf, uint32 len);
		void HandleQueryResponse(const uint8* buf, uint32 len);
		void RequestBlock(uint8 portalIndex, uint8 block);



		void HandleDeviceLoss();


		bool TryReopen();
		bool AnyCaching() const;
		int NextMissingBlock(uint8 portalIndex) const;


		bool OpenDevice(bool logFailure = true);
		void CloseDevice();
		bool SendControl(const uint8* data, uint32 len);
		bool SendAudio(const uint8* data, uint32 len);


		int ReadInterrupt(uint8* buf, uint32 len, int timeoutMs);

		AddCallback m_onAdd;
		RemoveCallback m_onRemove;

		std::thread m_thread;
		std::thread m_audioThread;
		std::atomic<bool> m_running{false};
		std::atomic<bool> m_connected{false};
		uint32 m_openFailureCount = 0;


		void* m_ctx = nullptr;
		void* m_handle = nullptr;
		std::shared_mutex m_deviceMutex;

		std::mutex m_outboxMutex;
		std::condition_variable m_audioCv;
		std::queue<OutCommand> m_outbox;
		std::queue<OutCommand> m_audioOutbox;
		std::queue<OutCommand> m_cancelled;
		OutCommand m_pendingWrite;
		bool m_writing = false;
		bool m_writeVerifyActive = false;
		uint8 m_writeAttempts = 0;
		uint64 m_writeVerifyAt = 0;
		uint64 m_writeVerifyDeadline = 0;


		OutCommand m_lastLedCmd;
		bool m_hasLastLedCmd = false;
		OutCommand m_lastSpeakerCmd;
		bool m_hasLastSpeakerCmd = false;
		bool m_speakerCmdPending = false;
		bool m_speakerReady = false;


		std::array<bool, SLOT_COUNT> m_present{};
		std::array<bool, SLOT_COUNT> m_caching{};
		std::array<std::array<uint8, FIGURE_SIZE>, SLOT_COUNT> m_cacheData{};
		std::array<std::array<bool, BLOCK_COUNT>, SLOT_COUNT> m_cacheGot{};


		std::array<uint64, SLOT_COUNT> m_cacheStartMs{};




		bool m_queryActive = false;
		uint8 m_querySlot = 0;
		uint8 m_queryBlock = 0;
		uint64 m_queryDeadlineMs = 0;
		uint8 m_queryRetries = 0;


		std::atomic<bool> m_deviceLost{false};
		uint32 m_readErrorStreak = 0;



		std::array<uint8, 5> m_lastStatusLogged{0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	};
}
