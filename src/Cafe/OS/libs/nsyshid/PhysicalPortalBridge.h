// Adapted for Rift from rhsts/cemu-hybrid-skylander-portal (hybrid-portal).
// SPDX-License-Identifier: MPL-2.0
// Rift changes: acknowledged writes, UID guards, bounded queue, asynchronous discovery.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "Common/precompiled.h"

namespace nsyshid
{
	// Bridges Cemu's emulated Skylander portal to a REAL Portal of Power (1430:0150) over
	// libusb, so that figures physically placed on the real portal are merged into the
	// emulated 16-slot view alongside virtual (dump-backed) figures. This is the core of
	// "hybrid" mode (see HYBRID_PORTAL_INTEGRATION.md).
	//
	// It owns its OWN libusb context + worker thread, intentionally separate from
	// BackendLibusb (which is told to skip 1430:0150 while hybrid mode is active, so the
	// bridge is the device's exclusive owner). All libusb specifics live in the .cpp behind
	// HAS_LIBUSB; the handles here are opaque (void*) so this header pulls in no libusb
	// header and Skylander.cpp compiles even in builds without libusb.
	//
	// Protocol notes (validated against a real Trap Team Traptanium portal, 2026-05-30):
	//  - the portal DROPS commands sent back-to-back, so everything is paced: one outbox
	//    command per Step and at most ONE outstanding block query at a time
	//  - status push 'S': status word in bytes 1..4, 2 bits per slot, slot 0 in the lowest
	//    bits, low bit = present; byte 5 is a free-running counter
	//  - block read 'Q': the reply's index byte is 0x10|idx on success, bare idx while the
	//    figure is present but not readable yet (must retry)
	//  - block write 'W': bare slot index, then block, then 16 data bytes
	//
	// Threading model:
	//  - game/GUI threads only enqueue (SetColor / QueueWrite / QueueCommand, guarded by
	//    m_outboxMutex) and call Start/Stop
	//  - everything else (USB I/O, per-slot caching, callbacks) runs on the worker thread,
	//    so the per-slot state below needs no locking
	//  - the Add/Remove callbacks are invoked from the worker thread with no bridge lock
	//    held, so they may safely take SkylanderUSB's own locks
	class PhysicalPortalBridge
	{
	  public:
		static constexpr uint16 PORTAL_VID = 0x1430;
		static constexpr uint16 PORTAL_PID = 0x0150;
		static constexpr uint8 SLOT_COUNT = 16; // figure slots reported in the status word
		static constexpr uint8 BLOCK_COUNT = 0x40;
		static constexpr uint8 BLOCK_SIZE = 0x10;
		static constexpr uint32 FIGURE_SIZE = BLOCK_COUNT * BLOCK_SIZE; // 1024 bytes (== SKY_FIGURE_SIZE)

		// Invoked from the worker thread when a figure appears / disappears on the real
		// portal. portalIndex is the real portal's slot index (0..15); data is the full image.
		using AddCallback = std::function<void(uint8 portalIndex, const std::array<uint8, FIGURE_SIZE>& data)>;
		using RemoveCallback = std::function<void(uint8 portalIndex)>;

		PhysicalPortalBridge() = default;
		~PhysicalPortalBridge();

		PhysicalPortalBridge(const PhysicalPortalBridge&) = delete;
		PhysicalPortalBridge& operator=(const PhysicalPortalBridge&) = delete;

		void SetCallbacks(AddCallback onAdd, RemoveCallback onRemove);

		// Open the real portal + start the worker. Returns false when libusb is unavailable
		// or no portal is connected (the caller then simply stays purely emulated).
		bool Start();
		void Stop();
		// False while the portal is unplugged; the worker keeps watching for it and flips
		// this back once it reconnects.
		bool IsConnected() const { return m_connected.load(); }

		// Game-side actions forwarded to the real portal (thread-safe; drained by the worker).
		void SetColor(uint8 r, uint8 g, uint8 b);
		using WriteCallback = std::function<void(bool)>;
		void QueueWrite(uint8 portalIndex, uint8 block, uint32 serial, const uint8* data16, WriteCallback done);
		// Forward a raw portal command verbatim (used to mirror the game's LED commands
		// C/J/L so the real portal lights up exactly like the emulated one).
		void QueueCommand(const uint8* data, uint32 len);

	  private:
		struct OutCommand
		{
			std::array<uint8, 32> bytes{};
			uint32 serial{};
			WriteCallback done;
		};

		void FinishWrite(bool success);
		void ThreadMain();
		void InitHandshake();     // paced R / A startup sequence
		void DrainFor(uint32 ms); // read+dispatch incoming packets for a while
		void Step();
		void HandleIncoming(const uint8* buf, uint32 len);
		void HandleStatus(const uint8* buf, uint32 len);
		void HandleQueryResponse(const uint8* buf, uint32 len);
		void RequestBlock(uint8 portalIndex, uint8 block);
		// The portal vanished mid-session (unplug / driver hiccup): drop every physical
		// figure from the emulated view so the game is not stuck with a ghost figure, then
		// close the device and reset all per-slot state. Worker thread only.
		void HandleDeviceLoss();
		// One reconnect attempt; on failure sleeps out the retry interval (Stop-responsive).
		// On success re-runs the handshake and restores the game's last LED colour.
		bool TryReopen();
		bool AnyCaching() const;
		int NextMissingBlock(uint8 portalIndex) const; // lowest uncached block, -1 when complete

		// libusb wrappers (real implementations behind HAS_LIBUSB; stubs otherwise).
		bool OpenDevice(bool logFailure = true);
		void CloseDevice();
		bool SendControl(const uint8* data, uint32 len);
		// >0 bytes received, 0 timeout, -1 transient error (paced internally so the worker
		// cannot spin hot), -2 device gone (m_deviceLost has been set).
		int ReadInterrupt(uint8* buf, uint32 len, int timeoutMs);

		AddCallback m_onAdd;
		RemoveCallback m_onRemove;

		std::thread m_thread;
		std::atomic<bool> m_running{false};
		std::atomic<bool> m_connected{false};

		// Opaque libusb handles (cast to the real types only inside the .cpp).
		void* m_ctx = nullptr;
		void* m_handle = nullptr;

		std::mutex m_outboxMutex;
		std::queue<OutCommand> m_outbox;
		std::queue<OutCommand> m_cancelled;
		OutCommand m_pendingWrite;
		bool m_writing = false;
		uint64 m_writeDeadline = 0;
		// Latest LED colour command ('C') seen, replayed after a reconnect so the portal
		// does not come back dark. Guarded by m_outboxMutex.
		OutCommand m_lastLedCmd;
		bool m_hasLastLedCmd = false;

		// Presence + per-slot block caching. Touched only by the worker thread.
		std::array<bool, SLOT_COUNT> m_present{};
		std::array<bool, SLOT_COUNT> m_caching{};
		std::array<std::array<uint8, FIGURE_SIZE>, SLOT_COUNT> m_cacheData{};
		std::array<std::array<bool, BLOCK_COUNT>, SLOT_COUNT> m_cacheGot{};
		// When caching of a slot began (ms). Bounds the whole-figure read so a block that the
		// portal keeps reporting "present but not readable" can never spin forever.
		std::array<uint64, SLOT_COUNT> m_cacheStartMs{};

		// The real portal drops commands sent back-to-back, so block reads are paced: at most
		// ONE outstanding 'Q' query at a time, only advancing once its reply arrives (or it
		// times out and is retried). Worker-thread only.
		bool m_queryActive = false;
		uint8 m_querySlot = 0;
		uint8 m_queryBlock = 0;
		uint64 m_queryDeadlineMs = 0;
		uint8 m_queryRetries = 0;

		// Device-loss tracking. Worker-thread only (set inside the USB wrappers).
		bool m_deviceLost = false;
		uint32 m_readErrorStreak = 0;

		// Last status packet logged (diagnostics only); 0xFF sentinel so the first one
		// after every (re)connect is always logged.
		std::array<uint8, 5> m_lastStatusLogged{0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	};
} // namespace nsyshid
