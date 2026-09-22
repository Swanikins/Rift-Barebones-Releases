// Adapted for Rift from rhsts/cemu-hybrid-skylander-portal (hybrid-portal).
// SPDX-License-Identifier: MPL-2.0
// Rift changes: acknowledged writes, UID guards, bounded queue, asynchronous discovery.
#include "PhysicalPortalBridge.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#ifdef HAS_LIBUSB
#include <libusb.h>
#endif

namespace nsyshid
{
	namespace
	{
		uint64 NowMs()
		{
			return (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
					   std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		// HID Set_Report (control OUT) parameters used to send commands to the portal.
		constexpr uint8 CTRL_REQUEST_TYPE = 0x21;
		constexpr uint8 CTRL_REQUEST = 0x09;
		constexpr uint16 CTRL_VALUE = 0x0200;
		constexpr uint16 CTRL_INDEX = 0x0000;
		constexpr uint8 EP_INTERRUPT_IN = 0x81;
		constexpr uint32 PACKET_SIZE = 32;		 // Skylander command/response packet size
		constexpr uint8 CACHE_MAX_RETRIES = 3;
		constexpr uint64 QUERY_TIMEOUT_MS = 250; // wait this long for a block-read reply before retrying
		constexpr uint64 CACHE_MAX_MS = 6000;	 // give up reading a whole figure after this long
		constexpr uint64 HANDSHAKE_PACE_MS = 60; // gap between paced startup commands
		constexpr uint32 REOPEN_RETRY_MS = 1000; // how often to look for the portal after an unplug
		// Transient (non-unplug) read errors in an unbroken streak before the device is
		// treated as lost anyway. Each erroring read is paced ~2ms, so this is roughly half
		// a second of solid failures - a single glitch never drops figures.
		constexpr uint32 READ_ERROR_STREAK_LIMIT = 250;

		// Render up to `len` bytes of `buf` as a hex string for diagnostic logging.
		std::string HexDump(const uint8* buf, uint32 len)
		{
			static const char* k = "0123456789ABCDEF";
			std::string s;
			s.reserve(len * 3);
			for (uint32 i = 0; i < len; i++)
			{
				s.push_back(k[buf[i] >> 4]);
				s.push_back(k[buf[i] & 0xF]);
				s.push_back(' ');
			}
			return s;
		}

		// A real Skylander's first block holds a non-zero NUID. An all-zero (or near-zero)
		// figure means we cached garbage / an incomplete read - never hand that to the game.
		bool IsPlausibleFigure(const std::array<uint8, PhysicalPortalBridge::FIGURE_SIZE>& data)
		{
			// NUID (first 4 bytes) must be non-zero.
			if ((data[0] | data[1] | data[2] | data[3]) == 0)
				return false;
			// Require a reasonable amount of non-zero payload overall (real figures are
			// densely populated; a mostly-zero buffer is a failed read).
			uint32 nonZero = 0;
			for (uint8 b : data)
				if (b != 0)
					nonZero++;
			return nonZero >= 64;
		}
	} // namespace

	PhysicalPortalBridge::~PhysicalPortalBridge()
	{
		Stop();
	}

	void PhysicalPortalBridge::SetCallbacks(AddCallback onAdd, RemoveCallback onRemove)
	{
		m_onAdd = std::move(onAdd);
		m_onRemove = std::move(onRemove);
	}

	void PhysicalPortalBridge::SetColor(uint8 r, uint8 g, uint8 b)
	{
		// Same path as the game's own LED commands so colour coalescing lives in one place.
		const uint8 cmd[4] = {'C', r, g, b};
		QueueCommand(cmd, sizeof(cmd));
	}

	void PhysicalPortalBridge::QueueCommand(const uint8* data, uint32 len)
	{
		if (!data || !len || (data[0] != 'C' && data[0] != 'J' && data[0] != 'L')) return;
		OutCommand cmd; // zero-filled
		const uint32 n = std::min<uint32>(len, static_cast<uint32>(cmd.bytes.size()));
		memcpy(cmd.bytes.data(), data, n);
		std::lock_guard lock(m_outboxMutex);
		if (cmd.bytes[0] == 'C')
		{
			// Remember the latest colour so it can be restored after a reconnect.
			m_lastLedCmd = cmd;
			m_hasLastLedCmd = true;
			// Coalesce consecutive colour updates: only the latest colour matters, so replace
			// a pending 'C' at the tail instead of stacking them. This bounds the queue when a
			// game streams LED animation (Swap Force) and especially while forwarding is
			// paused during a figure read. Never reorders relative to queued writes ('W').
			if (!m_outbox.empty() && m_outbox.back().bytes[0] == 'C')
			{
				m_outbox.back() = cmd;
				return;
			}
		}
		if (m_outbox.size() < 128) m_outbox.push(cmd);
	}

	void PhysicalPortalBridge::QueueWrite(uint8 portalIndex, uint8 block, uint32 serial, const uint8* data16, WriteCallback done)
	{
		OutCommand cmd;
		cmd.bytes[0] = 'W'; cmd.bytes[1] = portalIndex; cmd.bytes[2] = block;
		memcpy(&cmd.bytes[3], data16, BLOCK_SIZE);
		cmd.serial = serial;
		cmd.done = std::move(done);
		{
			std::lock_guard lock(m_outboxMutex);
			if (m_running.load() && m_connected.load() && m_outbox.size() < 128)
			{
				m_outbox.push(std::move(cmd));
				return;
			}
		}
		cmd.done(false);
	}

	void PhysicalPortalBridge::FinishWrite(bool success)
	{
		if (!m_writing) return;
		m_writing = false;
		auto done = std::move(m_pendingWrite.done);
		if (done) done(success);
	}

	void PhysicalPortalBridge::RequestBlock(uint8 portalIndex, uint8 block)
	{
		OutCommand cmd;
		cmd.bytes[0] = 'Q';
		cmd.bytes[1] = 0x10 | (portalIndex & 0x0F); // high nibble 0x10 selects "read" (hardware-validated)
		cmd.bytes[2] = block;
		// Send the read query DIRECTLY rather than through m_outbox. The outbox carries the game's
		// LED ('C') / write ('W') commands, which some games (e.g. Swap Force) stream continuously
		// during gameplay. A query queued behind that flood would not reach the portal before its
		// QUERY_TIMEOUT_MS deadline (which starts the moment the query is issued), so block reads
		// would time out and the whole figure read would loop forever without completing. Block
		// reads must hit the wire immediately. Called on the bridge thread (Step), so this is safe.
		SendControl(cmd.bytes.data(), static_cast<uint32>(cmd.bytes.size()));
	}

#ifdef HAS_LIBUSB
	bool PhysicalPortalBridge::OpenDevice(bool logFailure)
	{
		libusb_context* ctx = nullptr;
		const int initRc = libusb_init(&ctx);
		if (initRc != 0)
		{
			cemuLog_log(LogType::Force, "PhysicalPortalBridge::OpenDevice: libusb_init failed ({})", initRc);
			return false;
		}
		libusb_device_handle* handle =
			libusb_open_device_with_vid_pid(ctx, PORTAL_VID, PORTAL_PID);
		if (!handle)
		{
			if (logFailure)
				cemuLog_log(LogType::Force,
								 "PhysicalPortalBridge::OpenDevice: portal {:04X}:{:04X} not found / not openable "
								 "(needs WinUSB driver via Zadig)",
								 PORTAL_VID, PORTAL_PID);
			libusb_exit(ctx);
			return false;
		}
		// Linux: detach the kernel HID driver so we can claim the interface. No-op elsewhere.
		libusb_set_auto_detach_kernel_driver(handle, 1);
		const int claimRc = libusb_claim_interface(handle, 0);
		if (claimRc != 0)
		{
			if (logFailure)
				cemuLog_log(LogType::Force,
								 "PhysicalPortalBridge::OpenDevice: claim_interface(0) failed ({})", claimRc);
			libusb_close(handle);
			libusb_exit(ctx);
			return false;
		}
		m_ctx = ctx;
		m_handle = handle;
		return true;
	}

	void PhysicalPortalBridge::CloseDevice()
	{
		if (m_handle)
		{
			libusb_release_interface(static_cast<libusb_device_handle*>(m_handle), 0);
			libusb_close(static_cast<libusb_device_handle*>(m_handle));
			m_handle = nullptr;
		}
		if (m_ctx)
		{
			libusb_exit(static_cast<libusb_context*>(m_ctx));
			m_ctx = nullptr;
		}
	}

	bool PhysicalPortalBridge::SendControl(const uint8* data, uint32 len)
	{
		if (!m_handle)
			return false;
		const int r = libusb_control_transfer(
			static_cast<libusb_device_handle*>(m_handle), CTRL_REQUEST_TYPE, CTRL_REQUEST,
			CTRL_VALUE, CTRL_INDEX, const_cast<uint8*>(data), static_cast<uint16>(len), 1000);
		if (r == LIBUSB_ERROR_NO_DEVICE)
			m_deviceLost = true; // unplug noticed on the send path
		return r == static_cast<int>(len);
	}

	int PhysicalPortalBridge::ReadInterrupt(uint8* buf, uint32 len, int timeoutMs)
	{
		if (!m_handle)
			return -2;
		int transferred = 0;
		const int r = libusb_interrupt_transfer(static_cast<libusb_device_handle*>(m_handle),
												EP_INTERRUPT_IN, buf, static_cast<int>(len),
												&transferred, timeoutMs);
		if (r == 0)
		{
			m_readErrorStreak = 0;
			return transferred;
		}
		if (r == LIBUSB_ERROR_TIMEOUT)
		{
			m_readErrorStreak = 0;
			return 0;
		}
		if (r == LIBUSB_ERROR_NO_DEVICE || ++m_readErrorStreak >= READ_ERROR_STREAK_LIMIT)
		{
			m_deviceLost = true;
			return -2;
		}
		// Transient error: unlike a timeout this returns instantly, so pace it here to keep
		// the worker loop from spinning hot while the device misbehaves.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return -1;
	}
#else
	bool PhysicalPortalBridge::OpenDevice(bool) { return false; }
	void PhysicalPortalBridge::CloseDevice() {}
	bool PhysicalPortalBridge::SendControl(const uint8*, uint32) { return false; }
	int PhysicalPortalBridge::ReadInterrupt(uint8*, uint32, int) { return -2; }
#endif // HAS_LIBUSB

	bool PhysicalPortalBridge::Start()
	{
		if (m_running.load())
			return true;
		// Fresh session: forget any per-slot / query / outbox state from a previous run.
		m_present.fill(false);
		m_caching.fill(false);
		for (auto& got : m_cacheGot)
			got.fill(false);
		m_queryActive = false;
		m_deviceLost = false;
		m_readErrorStreak = 0;
		m_lastStatusLogged.fill(0xFF);
		{
			std::lock_guard lock(m_outboxMutex);
			while (!m_outbox.empty())
				m_outbox.pop();
		}

		// The paced startup handshake (R / A) runs on the worker thread - see InitHandshake.
		m_connected.store(false);
		m_running.store(true);
		m_thread = std::thread(&PhysicalPortalBridge::ThreadMain, this);
		cemuLog_log(LogType::Force, "Rift hybrid: watching for a compatible USB portal");
		return true;
	}

	void PhysicalPortalBridge::Stop()
	{
		m_running.store(false);
		if (m_thread.joinable())
			m_thread.join();
		CloseDevice();
		m_connected.store(false);
	}

	void PhysicalPortalBridge::ThreadMain()
	{
		while (m_running.load())
		{
			if (m_deviceLost)
				HandleDeviceLoss();
			if (!m_connected.load())
			{
				if (!TryReopen())
					continue; // still unplugged; TryReopen paced the retry
			}
			Step();
		}
		HandleDeviceLoss();
	}

	void PhysicalPortalBridge::HandleDeviceLoss()
	{
		cemuLog_log(LogType::Force,
					"nsyshid::PhysicalPortalBridge: real portal disconnected - dropping its figures, "
					"watching for it to return");
		m_queryActive = false;
		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			m_caching[p] = false;
			m_cacheGot[p].fill(false);
			m_cacheData[p].fill(0);
			if (m_present[p])
			{
				m_present[p] = false;
				if (m_onRemove)
					m_onRemove(p);
			}
		}
		{
			std::lock_guard lock(m_outboxMutex);
			std::queue<OutCommand> pending;
			pending.swap(m_outbox);
			// Callbacks are drained outside the queue lock below.
			m_cancelled.swap(pending);
		}
		FinishWrite(false);
		while (!m_cancelled.empty())
		{
			auto cmd = std::move(m_cancelled.front()); m_cancelled.pop();
			if (cmd.done) cmd.done(false);
		}
		CloseDevice();
		m_connected.store(false);
		m_deviceLost = false;
		m_readErrorStreak = 0;
	}

	bool PhysicalPortalBridge::TryReopen()
	{
		if (!OpenDevice(/*logFailure*/ false))
		{
			// Portal still absent: wait out the retry interval in slices so Stop() stays snappy.
			for (uint32 waited = 0; waited < REOPEN_RETRY_MS && m_running.load(); waited += 50)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			return false;
		}
		m_connected.store(true);
		cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: real portal reconnected");
		InitHandshake();
		// The portal rebooted with its LED off; restore the game's current colour. Figures
		// still on it re-add themselves via the status pushes that follow the handshake.
		OutCommand led;
		bool hasLed = false;
		{
			std::lock_guard lock(m_outboxMutex);
			if (m_hasLastLedCmd)
			{
				led = m_lastLedCmd;
				hasLed = true;
			}
		}
		if (hasLed)
			SendControl(led.bytes.data(), static_cast<uint32>(led.bytes.size()));
		return true;
	}

	// Read and dispatch any interrupt packets that arrive within `ms`. Used to (a) pace the
	// startup commands and (b) consume the portal's R/A replies.
	void PhysicalPortalBridge::DrainFor(uint32 ms)
	{
		const uint64 until = NowMs() + ms;
		while (m_running.load() && NowMs() < until)
		{
			uint8 buf[64] = {};
			const int n = ReadInterrupt(buf, sizeof(buf), 10);
			if (n > 0)
				HandleIncoming(buf, static_cast<uint32>(n));
			else if (n == -2)
				std::this_thread::sleep_for(std::chrono::milliseconds(2)); // dead handle returns instantly
		}
	}

	// The real portal drops commands sent back-to-back, so the startup sequence is paced:
	// reset ('R'), then activate ('A 01'). A short drain after each command lets the portal
	// process it and reply. The LED is left to the game: its C/J/L commands are forwarded to
	// the real portal, and the current colour is pushed on (re)connect so it matches at once.
	void PhysicalPortalBridge::InitHandshake()
	{
		uint8 pkt[PACKET_SIZE] = {};

		pkt[0] = 'R';
		SendControl(pkt, PACKET_SIZE);
		DrainFor(HANDSHAKE_PACE_MS);

		std::memset(pkt, 0, sizeof(pkt));
		pkt[0] = 'A';
		pkt[1] = 0x01;
		SendControl(pkt, PACKET_SIZE);
		DrainFor(HANDSHAKE_PACE_MS);

		cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: handshake complete (R/A sent)");
	}

	bool PhysicalPortalBridge::AnyCaching() const
	{
		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			if (m_caching[p])
				return true;
		}
		return false;
	}

	int PhysicalPortalBridge::NextMissingBlock(uint8 portalIndex) const
	{
		for (uint8 b = 0; b < BLOCK_COUNT; b++)
		{
			if (!m_cacheGot[portalIndex][b])
				return b;
		}
		return -1;
	}

	void PhysicalPortalBridge::Step()
	{
		// 1. Send at most ONE queued game command (LED / write) per step - the portal drops
		//    commands sent in bursts, so everything to the portal is paced one-per-step.
		//    While a figure is being read, PAUSE these: the game's commands share the portal with
		//    the block-read queries, and an LED-heavy game (Swap Force) would otherwise starve the
		//    read. A read takes ~1-2s; LED/write forwarding resumes the moment it finishes. Stale
		//    colour updates are coalesced in QueueCommand so the queue does not balloon meanwhile.
		if (m_writing && NowMs() >= m_writeDeadline) FinishWrite(false);
		if (!AnyCaching() && !m_writing)
		{
			OutCommand cmd;
			bool have = false;
			{
				std::lock_guard lock(m_outboxMutex);
				if (!m_outbox.empty()) { cmd = std::move(m_outbox.front()); m_outbox.pop(); have = true; }
			}
			if (have && cmd.bytes[0] == 'W')
			{
				const uint8 p = cmd.bytes[1];
				uint32 serial = 0;
				if (p < SLOT_COUNT) memcpy(&serial, m_cacheData[p].data(), 4);
				if (p >= SLOT_COUNT || !m_present[p] || serial != cmd.serial || cmd.bytes[2] >= BLOCK_COUNT)
				{ if (cmd.done) cmd.done(false); }
				else
				{
					m_pendingWrite = std::move(cmd); m_writing = true;
					m_writeDeadline = NowMs() + 750;
					if (!SendControl(m_pendingWrite.bytes.data(), 32)) FinishWrite(false);
				}
			}
			else if (have) SendControl(cmd.bytes.data(), 32);
		}

		// 2. If no block-read is outstanding, issue the next one for whichever figure is being
		//    cached (one at a time - this is the key to reliable reads).
		if (!m_queryActive && !m_writing)
		{
			for (uint8 p = 0; p < SLOT_COUNT; p++)
			{
				if (!m_caching[p])
					continue;
				const int nextBlock = NextMissingBlock(p);
				if (nextBlock < 0)
					continue; // fully cached; completion handled in step 5
				m_querySlot = p;
				m_queryBlock = static_cast<uint8>(nextBlock);
				m_queryRetries = 0;
				m_queryActive = true;
				RequestBlock(p, m_queryBlock);
				// Deadline set AFTER the (blocking) send so the reply always gets the full
				// window, even if the control transfer itself stalled for a while.
				m_queryDeadlineMs = NowMs() + QUERY_TIMEOUT_MS;
				break;
			}
		}

		// 3. Read one interrupt-IN packet (status push or query reply). A 'Q' reply clears
		//    m_queryActive (see HandleQueryResponse) so the next block can be requested.
		uint8 buf[64] = {};
		const int n = ReadInterrupt(buf, sizeof(buf), 10);
		if (n > 0)
			HandleIncoming(buf, static_cast<uint32>(n));
		if (m_deviceLost)
			return; // ThreadMain drops the figures and starts watching for a reconnect

		// 4. Retry / give up on the outstanding query if it timed out.
		const uint64 now = NowMs();
		if (m_queryActive && now > m_queryDeadlineMs)
		{
			if (m_queryRetries < CACHE_MAX_RETRIES)
			{
				m_queryRetries++;
				RequestBlock(m_querySlot, m_queryBlock);
				m_queryDeadlineMs = NowMs() + QUERY_TIMEOUT_MS;
			}
			else
			{
				// Could not read this block. Abort the figure WITHOUT injecting anything; a later
				// status push will resume the read cleanly (already-cached blocks are kept).
				const uint8 p = m_querySlot;
				m_queryActive = false;
				if (m_caching[p])
				{
					m_caching[p] = false;
					cemuLog_log(LogType::Force,
								"nsyshid::PhysicalPortalBridge: figure {} read incomplete at block {} - aborted",
								p, m_queryBlock);
				}
			}
		}

		// 4b. Bound the whole-figure read. A block can be answered "present but not readable"
		//     indefinitely (figure half-on the portal, damaged tag, etc.); the per-block timeout
		//     never fires in that case because replies keep arriving. Abort once the figure has
		//     been caching too long; the next status push resumes the read cleanly.
		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			if (!m_caching[p])
				continue;
			if (now - m_cacheStartMs[p] > CACHE_MAX_MS)
			{
				m_caching[p] = false;
				if (m_queryActive && m_querySlot == p)
					m_queryActive = false;
				cemuLog_log(LogType::Force,
							"nsyshid::PhysicalPortalBridge: figure {} read timed out after {}ms - aborted",
							p, CACHE_MAX_MS);
			}
		}

		// 5. Deliver any figure whose blocks are now all cached.
		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			if (!m_caching[p])
				continue;
			if (NextMissingBlock(p) >= 0)
				continue; // still incomplete

			m_caching[p] = false;
			if (m_queryActive && m_querySlot == p)
				m_queryActive = false;

			// Never hand a failed/garbled read to the game (that is what produced the in-game
			// "a toy has a problem" error).
			if (IsPlausibleFigure(m_cacheData[p]))
			{
				m_present[p] = true;
				cemuLog_log(LogType::Force,
							"nsyshid::PhysicalPortalBridge: figure {} read OK (id {:02X}{:02X}{:02X}{:02X})",
							p, m_cacheData[p][0], m_cacheData[p][1], m_cacheData[p][2], m_cacheData[p][3]);
				if (m_onAdd)
					m_onAdd(p, m_cacheData[p]);
			}
			else
			{
				cemuLog_log(LogType::Force,
							"nsyshid::PhysicalPortalBridge: slot {} cached data implausible - discarded "
							"(first block: {})",
							p, HexDump(m_cacheData[p].data(), BLOCK_SIZE));
			}
		}
	}

	void PhysicalPortalBridge::HandleIncoming(const uint8* buf, uint32 len)
	{
		if (len < 1)
			return;
		switch (buf[0])
		{
		case 'W':
			if (len >= 3 && m_writing && (buf[1] & 15) == m_pendingWrite.bytes[1] && buf[2] == m_pendingWrite.bytes[2])
				FinishWrite((buf[1] & 0x10) != 0);
			break;
		case 'S': // status push (0x53)
			HandleStatus(buf, len);
			break;
		case 'Q': // query reply
			HandleQueryResponse(buf, len);
			break;
		default:
			break; // 'W' ack and others: nothing to do
		}
	}

	void PhysicalPortalBridge::HandleStatus(const uint8* buf, uint32 len)
	{
		if (len < 5)
			return;
		// Log the status only when the status WORD (bytes 1..4) actually changes - byte 5 is a
		// free-running counter, so comparing it would log every single packet.
		if (std::memcmp(buf, m_lastStatusLogged.data(), m_lastStatusLogged.size()) != 0)
		{
			std::memcpy(m_lastStatusLogged.data(), buf, m_lastStatusLogged.size());
			cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: status changed {}",
							 HexDump(buf, 5));
		}
		// Status word: 2 bits per slot index, low bit = present, slot 0 in the lowest bits
		// (mirrors SkylanderUSB::GetStatus; layout validated against real hardware).
		const uint32 status = static_cast<uint32>(buf[1]) |
							  (static_cast<uint32>(buf[2]) << 8) |
							  (static_cast<uint32>(buf[3]) << 16) |
							  (static_cast<uint32>(buf[4]) << 24);
		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			const bool present = ((status >> (p * 2)) & 0x1) != 0;
			if (present)
			{
				if (!m_present[p] && !m_caching[p])
				{
					// Begin (or RESUME) caching. Step() issues the block reads one at a time.
					// We deliberately do NOT clear m_cacheGot here: if a previous attempt aborted on
					// a single flaky block, the figure is still on the portal, so we resume and fetch
					// only the blocks we are still missing instead of re-reading all 64 from scratch
					// (which made reads very slow). The cache is cleared on actual removal below, so a
					// fresh figure always starts clean. m_cacheStartMs is reset so each attempt gets a
					// fresh whole-figure time budget.
					uint8 got = 0;
					for (uint8 b = 0; b < BLOCK_COUNT; b++)
						if (m_cacheGot[p][b])
							got++;
					m_caching[p] = true;
					m_cacheStartMs[p] = NowMs();
					if (got == 0)
						cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: figure arrived on slot {} - reading", p);
					else
						cemuLog_log(LogType::Force,
									"nsyshid::PhysicalPortalBridge: figure read resuming on slot {} ({}/{} blocks cached)",
									p, got, BLOCK_COUNT);
				}
			}
			else // not present
			{
				if (m_writing && m_pendingWrite.bytes[1] == p) FinishWrite(false);
				if (m_caching[p])
				{
					// Removed mid-read: abort cleanly.
					m_caching[p] = false;
					if (m_queryActive && m_querySlot == p)
						m_queryActive = false;
				}
				// Figure is off the portal: discard any cached blocks so the NEXT figure placed here
				// starts a clean read (this is the only place the cache is reset).
				m_cacheGot[p].fill(false);
				m_cacheData[p].fill(0);
				if (m_present[p])
				{
					m_present[p] = false;
					if (m_onRemove)
						m_onRemove(p);
				}
			}
		}
	}

	void PhysicalPortalBridge::HandleQueryResponse(const uint8* buf, uint32 len)
	{
		if (len < 3u + BLOCK_SIZE)
			return;
		const uint8 idxByte = buf[1];
		const uint8 p = idxByte & 0xF; // reply index byte is 0x10|idx on success, idx on failure
		const uint8 block = buf[2];
		if (p >= SLOT_COUNT || block >= BLOCK_COUNT)
			return;
		if (!m_caching[p] || !m_queryActive || m_querySlot != p || m_queryBlock != block)
			return;
		// 0x10 in the index byte means the read succeeded (figure present & block returned).
		// If it is missing, the figure is present but not readable YET - normal while a figure
		// is still settling onto the portal. Leave the block un-got and clear m_queryActive so
		// Step() re-issues it promptly (each re-issue is naturally ~10ms-paced by ReadInterrupt,
		// so this is fast without flooding). The whole-figure CACHE_MAX_MS guard in Step() bounds
		// this so it can never spin forever on a block that never becomes readable.
		if ((idxByte & 0x10) == 0)
		{
			if (m_queryActive && m_querySlot == p && m_queryBlock == block)
				m_queryActive = false; // allow prompt re-issue
			return;
		}
		memcpy(m_cacheData[p].data() + (block * BLOCK_SIZE), &buf[3], BLOCK_SIZE);
		m_cacheGot[p][block] = true;
		if (m_queryActive && m_querySlot == p && m_queryBlock == block)
			m_queryActive = false; // reply received -> Step issues the next block
	}
} // namespace nsyshid
