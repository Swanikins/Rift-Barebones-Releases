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


		constexpr uint8 CTRL_REQUEST_TYPE = 0x21;
		constexpr uint8 CTRL_REQUEST = 0x09;
		constexpr uint16 CTRL_VALUE = 0x0200;
		constexpr uint16 CTRL_INDEX = 0x0000;
		constexpr uint8 EP_INTERRUPT_IN = 0x81;
		constexpr uint8 EP_INTERRUPT_OUT = 0x02;
		constexpr uint32 PACKET_SIZE = 32;
		constexpr uint32 MAX_AUDIO_PACKETS = 64;
		constexpr uint8 CACHE_MAX_RETRIES = 3;
		constexpr uint64 QUERY_TIMEOUT_MS = 250;
		constexpr uint64 CACHE_MAX_MS = 6000;
		constexpr uint64 HANDSHAKE_PACE_MS = 60;
		constexpr uint32 REOPEN_RETRY_MS = 1000;
		constexpr uint64 WRITE_VERIFY_DELAY_MS = 100;
		constexpr uint64 WRITE_VERIFY_TIMEOUT_MS = 300;
		constexpr uint8 WRITE_MAX_ATTEMPTS = 3;



		constexpr uint32 READ_ERROR_STREAK_LIMIT = 250;


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



		bool IsPlausibleFigure(const std::array<uint8, PhysicalPortalBridge::FIGURE_SIZE>& data)
		{

			if ((data[0] | data[1] | data[2] | data[3]) == 0)
				return false;
			if (data[4] != static_cast<uint8>(data[0] ^ data[1] ^ data[2] ^ data[3]))
				return false;


			uint32 nonZero = 0;
			for (uint8 b : data)
				if (b != 0)
					nonZero++;
			return nonZero >= 64;
		}
	}

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

		const uint8 cmd[4] = {'C', r, g, b};
		QueueCommand(cmd, sizeof(cmd));
	}

	void PhysicalPortalBridge::QueueCommand(const uint8* data, uint32 len)
	{
		if (!data || !len || (data[0] != 'C' && data[0] != 'J' && data[0] != 'L' && data[0] != 'M')) return;
		OutCommand cmd;
		const uint32 n = std::min<uint32>(len, static_cast<uint32>(cmd.bytes.size()));
		memcpy(cmd.bytes.data(), data, n);
		std::lock_guard lock(m_outboxMutex);
		if (cmd.bytes[0] == 'M')
		{
			m_lastSpeakerCmd = cmd;
			m_hasLastSpeakerCmd = true;
			m_speakerCmdPending = true;
			m_speakerReady = false;
			if (n > 1 && cmd.bytes[1] == 0)
			{
				while (!m_audioOutbox.empty())
					m_audioOutbox.pop();
			}
			return;
		}
		if (cmd.bytes[0] == 'C')
		{

			m_lastLedCmd = cmd;
			m_hasLastLedCmd = true;




			if (!m_outbox.empty() && m_outbox.back().bytes[0] == 'C')
			{
				m_outbox.back() = cmd;
				return;
			}
		}
		if (m_outbox.size() < 128) m_outbox.push(cmd);
	}

	void PhysicalPortalBridge::QueueAudio(const uint8* data, uint32 len)
	{
		if (!data || !len || !m_running.load() || !m_connected.load())
			return;
		{
			std::lock_guard lock(m_outboxMutex);
			for (uint32 offset = 0; offset < len; offset += PACKET_SIZE)
			{
				OutCommand packet;
				const uint32 packetLength = std::min<uint32>(PACKET_SIZE, len - offset);
				memcpy(packet.bytes.data(), data + offset, packetLength);
				while (m_audioOutbox.size() >= MAX_AUDIO_PACKETS)
					m_audioOutbox.pop();
				m_audioOutbox.push(std::move(packet));
			}
		}
		m_audioCv.notify_one();
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
		m_writeVerifyActive = false;
		auto done = std::move(m_pendingWrite.done);
		if (done) done(success);
	}

	bool PhysicalPortalBridge::BeginWriteAttempt()
	{
		if (!m_writing)
			return false;
		m_writeAttempts++;
		m_writeVerifyActive = false;
		if (!SendControl(m_pendingWrite.bytes.data(), static_cast<uint32>(m_pendingWrite.bytes.size())))
			return false;
		m_writeVerifyAt = NowMs() + WRITE_VERIFY_DELAY_MS;
		return true;
	}

	void PhysicalPortalBridge::RetryOrFinishWrite()
	{
		if (!m_writing)
			return;
		m_writeVerifyActive = false;

		const uint8 portalIndex = m_pendingWrite.bytes[1];
		uint32 serial = 0;
		if (portalIndex < SLOT_COUNT)
			memcpy(&serial, m_cacheData[portalIndex].data(), sizeof(serial));
		if (portalIndex >= SLOT_COUNT || !m_present[portalIndex] || serial != m_pendingWrite.serial ||
			m_writeAttempts >= WRITE_MAX_ATTEMPTS || !BeginWriteAttempt())
		{
			FinishWrite(false);
		}
	}

	void PhysicalPortalBridge::RequestBlock(uint8 portalIndex, uint8 block)
	{
		OutCommand cmd;
		cmd.bytes[0] = 'Q';
		cmd.bytes[1] = 0x10 | (portalIndex & 0x0F);
		cmd.bytes[2] = block;






		SendControl(cmd.bytes.data(), static_cast<uint32>(cmd.bytes.size()));
	}

#ifdef HAS_LIBUSB
	bool PhysicalPortalBridge::OpenDevice(bool logFailure)
	{
		std::unique_lock deviceLock(m_deviceMutex);
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
		std::unique_lock deviceLock(m_deviceMutex);
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
		std::shared_lock deviceLock(m_deviceMutex);
		if (!m_handle)
			return false;
		const int r = libusb_control_transfer(
			static_cast<libusb_device_handle*>(m_handle), CTRL_REQUEST_TYPE, CTRL_REQUEST,
			CTRL_VALUE, CTRL_INDEX, const_cast<uint8*>(data), static_cast<uint16>(len), 1000);
		if (r == LIBUSB_ERROR_NO_DEVICE)
			m_deviceLost = true;
		return r == static_cast<int>(len);
	}

	int PhysicalPortalBridge::ReadInterrupt(uint8* buf, uint32 len, int timeoutMs)
	{
		std::shared_lock deviceLock(m_deviceMutex);
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


		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return -1;
	}

	bool PhysicalPortalBridge::SendAudio(const uint8* data, uint32 len)
	{
		std::shared_lock deviceLock(m_deviceMutex);
		if (!m_handle)
			return false;
		int transferred = 0;
		const int result = libusb_interrupt_transfer(static_cast<libusb_device_handle*>(m_handle),
			EP_INTERRUPT_OUT, const_cast<uint8*>(data), static_cast<int>(len), &transferred, 100);
		if (result == LIBUSB_ERROR_NO_DEVICE)
			m_deviceLost = true;
		return result == 0 && transferred == static_cast<int>(len);
	}
#else
	bool PhysicalPortalBridge::OpenDevice(bool) { return false; }
	void PhysicalPortalBridge::CloseDevice() {}
	bool PhysicalPortalBridge::SendControl(const uint8*, uint32) { return false; }
	bool PhysicalPortalBridge::SendAudio(const uint8*, uint32) { return false; }
	int PhysicalPortalBridge::ReadInterrupt(uint8*, uint32, int) { return -2; }
#endif

	bool PhysicalPortalBridge::Start()
	{
		if (m_running.load())
			return true;

		m_present.fill(false);
		m_caching.fill(false);
		for (auto& got : m_cacheGot)
			got.fill(false);
		m_queryActive = false;
		m_writing = false;
		m_writeVerifyActive = false;
		m_writeAttempts = 0;
		m_deviceLost = false;
		m_readErrorStreak = 0;
		m_lastStatusLogged.fill(0xFF);
		{
			std::lock_guard lock(m_outboxMutex);
			while (!m_outbox.empty())
				m_outbox.pop();
			while (!m_audioOutbox.empty())
				m_audioOutbox.pop();
			m_speakerCmdPending = false;
			m_speakerReady = false;
		}


		m_connected.store(false);
		m_openFailureCount = 0;
		m_running.store(true);
		m_audioThread = std::thread(&PhysicalPortalBridge::AudioThreadMain, this);
		m_thread = std::thread(&PhysicalPortalBridge::ThreadMain, this);
		cemuLog_log(LogType::Force, "Rift hybrid: watching for a compatible USB portal");
		return true;
	}

	void PhysicalPortalBridge::Stop()
	{
		m_running.store(false);
		m_connected.store(false);
		m_audioCv.notify_all();
		if (m_audioThread.joinable())
			m_audioThread.join();
		if (m_thread.joinable())
			m_thread.join();
		CloseDevice();
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
					continue;
			}
			Step();
		}
		HandleDeviceLoss();
	}

	void PhysicalPortalBridge::AudioThreadMain()
	{
		while (m_running.load())
		{
			OutCommand packet;
			{
				std::unique_lock lock(m_outboxMutex);
				m_audioCv.wait(lock, [&] {
					return !m_running.load() ||
						(m_connected.load() && m_speakerReady && !m_audioOutbox.empty());
				});
				if (!m_running.load())
					break;
				packet = std::move(m_audioOutbox.front());
				m_audioOutbox.pop();
			}
			SendAudio(packet.bytes.data(), static_cast<uint32>(packet.bytes.size()));
		}
	}

	void PhysicalPortalBridge::HandleDeviceLoss()
	{
		m_connected.store(false);
		if (m_deviceLost)
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
			while (!m_audioOutbox.empty())
				m_audioOutbox.pop();
			m_speakerReady = false;

			m_cancelled.swap(pending);
		}
		FinishWrite(false);
		while (!m_cancelled.empty())
		{
			auto cmd = std::move(m_cancelled.front()); m_cancelled.pop();
			if (cmd.done) cmd.done(false);
		}
		CloseDevice();
		m_deviceLost = false;
		m_readErrorStreak = 0;
	}

	bool PhysicalPortalBridge::TryReopen()
	{
		const bool logFailure = m_openFailureCount == 0 || (m_openFailureCount % 10) == 0;
		if (!OpenDevice(logFailure))
		{
			m_openFailureCount++;

			for (uint32 waited = 0; waited < REOPEN_RETRY_MS && m_running.load(); waited += 50)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			return false;
		}
		m_openFailureCount = 0;
		cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: real portal reconnected");
		InitHandshake();


		OutCommand led;
		bool hasLed = false;
		OutCommand speaker;
		bool hasSpeaker = false;
		{
			std::lock_guard lock(m_outboxMutex);
			if (m_hasLastLedCmd)
			{
				led = m_lastLedCmd;
				hasLed = true;
			}
			if (m_hasLastSpeakerCmd)
			{
				speaker = m_lastSpeakerCmd;
				hasSpeaker = true;
			}
		}
		if (hasLed)
			SendControl(led.bytes.data(), static_cast<uint32>(led.bytes.size()));
		const bool speakerSent = hasSpeaker &&
			SendControl(speaker.bytes.data(), static_cast<uint32>(speaker.bytes.size()));
		{
			std::lock_guard lock(m_outboxMutex);
			if (hasSpeaker && m_lastSpeakerCmd.bytes == speaker.bytes)
			{
				m_speakerCmdPending = !speakerSent;
				m_speakerReady = speakerSent && speaker.bytes[1] != 0;
			}
		}
		if (m_deviceLost.load())
			return false;
		m_connected.store(true);
		m_audioCv.notify_all();
		return true;
	}



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
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}





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
		OutCommand speaker;
		bool haveSpeaker = false;
		{
			std::lock_guard lock(m_outboxMutex);
			if (m_speakerCmdPending)
			{
				speaker = m_lastSpeakerCmd;
				haveSpeaker = true;
			}
		}
		if (haveSpeaker)
		{
			const bool speakerSent = SendControl(speaker.bytes.data(), static_cast<uint32>(speaker.bytes.size()));
			{
				std::lock_guard lock(m_outboxMutex);
				if (m_lastSpeakerCmd.bytes == speaker.bytes)
				{
					m_speakerCmdPending = !speakerSent;
					m_speakerReady = speakerSent && speaker.bytes[1] != 0;
				}
			}
			m_audioCv.notify_all();
		}






		const uint64 stepNow = NowMs();
		if (m_writing && !m_writeVerifyActive && stepNow >= m_writeVerifyAt)
		{
			RequestBlock(m_pendingWrite.bytes[1], m_pendingWrite.bytes[2]);
			m_writeVerifyActive = true;
			m_writeVerifyDeadline = NowMs() + WRITE_VERIFY_TIMEOUT_MS;
		}
		else if (m_writing && m_writeVerifyActive && stepNow >= m_writeVerifyDeadline)
		{
			RetryOrFinishWrite();
		}
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
					m_pendingWrite = std::move(cmd);
					m_writing = true;
					m_writeAttempts = 0;
					if (!BeginWriteAttempt()) FinishWrite(false);
				}
			}
			else if (have) SendControl(cmd.bytes.data(), 32);
		}



		if (!m_queryActive && !m_writing)
		{
			for (uint8 p = 0; p < SLOT_COUNT; p++)
			{
				if (!m_caching[p])
					continue;
				const int nextBlock = NextMissingBlock(p);
				if (nextBlock < 0)
					continue;
				m_querySlot = p;
				m_queryBlock = static_cast<uint8>(nextBlock);
				m_queryRetries = 0;
				m_queryActive = true;
				RequestBlock(p, m_queryBlock);


				m_queryDeadlineMs = NowMs() + QUERY_TIMEOUT_MS;
				break;
			}
		}



		uint8 buf[64] = {};
		const int n = ReadInterrupt(buf, sizeof(buf), 10);
		if (n > 0)
			HandleIncoming(buf, static_cast<uint32>(n));
		if (m_deviceLost)
			return;


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


		for (uint8 p = 0; p < SLOT_COUNT; p++)
		{
			if (!m_caching[p])
				continue;
			if (NextMissingBlock(p) >= 0)
				continue;

			m_caching[p] = false;
			if (m_queryActive && m_querySlot == p)
				m_queryActive = false;



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
			break;
		case 'S':
			HandleStatus(buf, len);
			break;
		case 'Q':
			HandleQueryResponse(buf, len);
			break;
		default:
			break;
		}
	}

	void PhysicalPortalBridge::HandleStatus(const uint8* buf, uint32 len)
	{
		if (len < 5)
			return;


		if (std::memcmp(buf, m_lastStatusLogged.data(), m_lastStatusLogged.size()) != 0)
		{
			std::memcpy(m_lastStatusLogged.data(), buf, m_lastStatusLogged.size());
			cemuLog_log(LogType::Force, "nsyshid::PhysicalPortalBridge: status changed {}",
							 HexDump(buf, 5));
		}


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
			else
			{
				if (m_writing && m_pendingWrite.bytes[1] == p) FinishWrite(false);
				if (m_caching[p])
				{

					m_caching[p] = false;
					if (m_queryActive && m_querySlot == p)
						m_queryActive = false;
				}


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
		const uint8 p = idxByte & 0xF;
		const uint8 block = buf[2];
		if (p >= SLOT_COUNT || block >= BLOCK_COUNT)
			return;

		if (m_writing && m_writeVerifyActive && p == m_pendingWrite.bytes[1] &&
			block == m_pendingWrite.bytes[2])
		{
			m_writeVerifyActive = false;
			if ((idxByte & 0x10) != 0 &&
				std::memcmp(&buf[3], &m_pendingWrite.bytes[3], BLOCK_SIZE) == 0)
			{
				memcpy(m_cacheData[p].data() + (block * BLOCK_SIZE), &buf[3], BLOCK_SIZE);
				FinishWrite(true);
			}
			else
			{
				RetryOrFinishWrite();
			}
			return;
		}
		if (!m_caching[p] || !m_queryActive || m_querySlot != p || m_queryBlock != block)
			return;






		if ((idxByte & 0x10) == 0)
		{
			if (m_queryActive && m_querySlot == p && m_queryBlock == block)
				m_queryActive = false;
			return;
		}
		memcpy(m_cacheData[p].data() + (block * BLOCK_SIZE), &buf[3], BLOCK_SIZE);
		m_cacheGot[p][block] = true;
		if (m_queryActive && m_querySlot == p && m_queryBlock == block)
			m_queryActive = false;
	}
}
