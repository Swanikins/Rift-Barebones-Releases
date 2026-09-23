#include "SkylanderQuickMenu.h"

#include <imgui.h>
#include <wx/image.h>
#include <wx/log.h>
#include <condition_variable>
#include <deque>
#include <thread>
#include <tuple>
#if BOOST_OS_WINDOWS
#include <mmsystem.h>
#endif

#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/nsyshid/Backend.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Common/FileStream.h"
#include "config/CemuConfig.h"
#include "gui/SkylanderCatalog.h"
#include "imgui/imgui_extension.h"
#include "input/InputManager.h"
#include "input/emulated/EmulatedController.h"

namespace
{
	constexpr float kPi = 3.14159265359f;
	constexpr float kDrawerX = 300.0f;
	constexpr float kDrawerVisibleX = 440.0f;
	constexpr float kDrawerHorizontalScale = (1280.0f - kDrawerVisibleX) / (1280.0f - kDrawerX);
	constexpr float kFooterTop = 642.0f;
	constexpr int kLibraryColumns = 4;
	constexpr int kLibraryRows = 2;
	constexpr int kLibraryPageSize = kLibraryColumns * kLibraryRows;
	constexpr int kPortalCapacity = 16;
	constexpr int kVisiblePortalSlots = 7;
	constexpr int kOptionCount = 16;

	enum class RiftPage
	{
		Dashboard,
		Details,
		Forge,
		Options,
		LibraryOrder,
		DeleteConfirm
	};

	enum class FocusArea
	{
		Header,
		Portal,
		Search,
		ElementFilter,
		Library
	};

	enum class SearchTarget
	{
		None,
		Collection,
		Create
	};

	enum class SortMode : sint32
	{
		NameAscending,
		NameDescending,
		FavoritesFirst,
		NewestFiles,
		FavoritesOnly,
		Element,
		FigureType,
		Count
	};

	enum class ForgeSortMode : sint32
	{
		NameAscending,
		NameDescending,
		Element,
		FigureType,
		Count
	};

	enum class UiSound
	{
		Navigate,
		Confirm,
		Back,
		Open
	};

	struct RiftLayout
	{
		float scale{1.0f};
		ImVec2 origin{};
		float alpha{1.0f};
		float slideX{};
	};

	struct ArtworkTexture
	{
		enum class State
		{
			Loading,
			Missing,
			Ready
		};

		ImTextureID id{};
		ImVec2 size{};
		ImVec2 uvMinimum{};
		ImVec2 uvMaximum{1.0f, 1.0f};
		ImU32 accent{IM_COL32(71, 207, 255, 255)};
		State state{State::Missing};
	};

	struct PortalRow
	{
		int actualSlot{-1};
		uint16 id{};
		uint16 variant{};
		const skylander_ui::FigureDefinition* definition{};
		int extras{};
	};

	struct DetailState
	{
		bool fromLibrary{};
		int libraryIndex{-1};
		int actualSlot{-1};
		uint16 id{};
		uint16 variant{};
		std::string name;
		fs::path imagePath;
		fs::path filePath;
	};

	struct QuickMenuState
	{
		bool requestedOpen{};
		bool inputCaptureLatched{};
		bool openChordBlockedUntilRelease{};
		bool controlsNeutral{true};
		bool reloadCatalog{true};
		bool rebuildLibrary{true};
		bool backWasDown{};
		bool upWasDown{};
		bool downWasDown{};
		bool leftWasDown{};
		bool rightWasDown{};
		bool acceptWasDown{};
		bool removeWasDown{};
		bool detailsWasDown{};
		bool sortWasDown{};
		bool favoriteWasDown{};
		bool hapticActive{};
		int neutralInputFrames{};
		int ownerController{-1};
		float visibility{};
		float selectionFlash{};
		float portalModeFlash{};
		float placementAnimation{1.0f};
		float placementImpact{};
		float toastLife{};
		float pageVisibility{1.0f};
		int selectedLibrary{};
		int selectedElementFilter{};
		int selectedPortalRow{};
		int selectedDefinition{};
		int forgeSortMode{};
		int forgeTypeFilter{};
		int forgeElementFilter{};
		int forgeToolbarSelection{};
		int keyboardRow{};
		int keyboardColumn{};
		int selectedOption{};
		int selectedLibraryOption{};
		int headerSelection{};
		int placementTargetRow{};
		uint8 placementSlot{0xFF};
		RiftPage page{RiftPage::Dashboard};
		RiftPage renderedPage{RiftPage::Dashboard};
		FocusArea focus{FocusArea::Library};
		SearchTarget searchTarget{SearchTarget::None};
		bool forgeToolbarFocused{};
		ImVec2 selectedCardCenter{};
		std::array<ImVec2, kPortalCapacity> portalTargets{};
		std::array<float, kPortalCapacity + 1> portalCardX{};
		std::array<float, 12> elementFilterFocus{};
		std::array<float, kPortalCapacity + 1> portalCardWidth{};
		std::array<float, 4> headerFocusAnimations{};
		std::array<float, kPortalCapacity> portalFocusAnimations{};
		std::array<float, kOptionCount> optionFocusAnimations{};
		std::array<float, 5> libraryOptionFocusAnimations{};
		std::array<double, 4> nextNavigationRepeat{};
		fs::path placementArtwork;
		ImU32 placementAccent{IM_COL32(71, 207, 255, 255)};
		std::string toast;
		std::array<char, 96> searchText{};
		std::array<char, 96> forgeSearchText{};
		std::chrono::steady_clock::time_point hapticStop{};
		skylander_ui::SkylanderCatalog catalog;
		std::vector<skylander_ui::CollectionFigure> library;
		std::unordered_set<std::string> favorites;
		DetailState details;
	} s_menu;

	std::atomic_uint32_t s_toggleRequests{};
	std::atomic_bool s_resetRequested{};
	std::atomic_bool s_updateCheckRequested{};
	std::unordered_map<std::string, ArtworkTexture> s_artworkTextures;
	std::unordered_map<std::string, float> s_cardFocusAmounts;
	std::unordered_map<std::string, float> s_cardCarouselOffsets;
	std::unordered_map<std::string, float> s_forgeFocusAmounts;

	float Clamp01(float value)
	{
		return std::clamp(value, 0.0f, 1.0f);
	}

	float EaseOutCubic(float value)
	{
		const float inverse = 1.0f - value;
		return 1.0f - inverse * inverse * inverse;
	}

	float SmoothStep(float value)
	{
		value = Clamp01(value);
		return value * value * (3.0f - 2.0f * value);
	}

	float SmoothTowards(float current, float target, float speed)
	{
		const float response = 1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime);
		return current + (target - current) * response;
	}

	int MotionLevel()
	{
		return std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_motion_level.GetValue(), 0, 2);
	}

	int BloomLevel()
	{
		return std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_bloom_level.GetValue(), 0, 2);
	}

	bool MotionEnabled()
	{
		return MotionLevel() > 0;
	}

	float AnimateFocus(float current, float target)
	{
		if (!MotionEnabled())
			return target;
		const bool fadingOut = target < current;
		const float speed = MotionLevel() == 2 ? (fadingOut ? 11.5f : 8.5f) : (fadingOut ? 20.0f : 15.0f);
		return SmoothTowards(current, target, speed);
	}

	ImVec2 Point(const RiftLayout& layout, float x, float y)
	{
		const float mappedX = kDrawerVisibleX + (x + layout.slideX - kDrawerX) * kDrawerHorizontalScale;
		return {layout.origin.x + mappedX * layout.scale, layout.origin.y + y * layout.scale};
	}

	float DrawerWidth(const RiftLayout& layout, float width)
	{
		return width * kDrawerHorizontalScale * layout.scale;
	}

	ImU32 WithAlpha(ImU32 color, float alpha)
	{
		const uint32 sourceAlpha = (color >> 24) & 0xFF;
		const uint32 resultAlpha = static_cast<uint32>(sourceAlpha * Clamp01(alpha));
		return (color & 0x00FFFFFF) | (resultAlpha << 24);
	}

	ImU32 Lighten(ImU32 color, float amount)
	{
		const int r = color & 0xFF;
		const int g = (color >> 8) & 0xFF;
		const int b = (color >> 16) & 0xFF;
		return IM_COL32(
			std::clamp(static_cast<int>(r + (255 - r) * amount), 0, 255),
			std::clamp(static_cast<int>(g + (255 - g) * amount), 0, 255),
			std::clamp(static_cast<int>(b + (255 - b) * amount), 0, 255),
			255);
	}

	ImU32 BlendColor(ImU32 from, ImU32 to, float amount)
	{
		amount = Clamp01(amount);
		ImU32 result = 0;
		for (int shift = 0; shift < 32; shift += 8)
		{
			const float a = (from >> shift) & 255;
			const float b = (to >> shift) & 255;
			result |= static_cast<ImU32>(a + (b - a) * amount + 0.5f) << shift;
		}
		return result;
	}

	struct ThemePalette
	{
		ImU32 accent, secondary, background, surface, selectedSurface, text, muted, border;
	};

	ThemePalette CurrentTheme()
	{
		ThemePalette theme{};
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_theme.GetValue(), 0, 4))
		{
		case 1: theme = {IM_COL32(177, 105, 255, 255), IM_COL32(91, 216, 255, 255), IM_COL32(8, 5, 24, 255), IM_COL32(25, 16, 48, 255), IM_COL32(53, 29, 82, 255), IM_COL32(249, 244, 255, 255), IM_COL32(185, 164, 211, 255), IM_COL32(169, 125, 213, 255)}; break;
		case 2: theme = {IM_COL32(255, 185, 60, 255), IM_COL32(255, 91, 54, 255), IM_COL32(24, 8, 4, 255), IM_COL32(53, 22, 10, 255), IM_COL32(89, 38, 14, 255), IM_COL32(255, 248, 231, 255), IM_COL32(218, 176, 132, 255), IM_COL32(224, 137, 66, 255)}; break;
		case 3: theme = {IM_COL32(91, 231, 151, 255), IM_COL32(71, 207, 255, 255), IM_COL32(3, 19, 19, 255), IM_COL32(9, 43, 39, 255), IM_COL32(17, 70, 57, 255), IM_COL32(238, 255, 249, 255), IM_COL32(145, 204, 184, 255), IM_COL32(103, 193, 166, 255)}; break;
		case 4: theme = {IM_COL32(244, 99, 170, 255), IM_COL32(177, 105, 255, 255), IM_COL32(20, 5, 20, 255), IM_COL32(48, 13, 42, 255), IM_COL32(80, 22, 67, 255), IM_COL32(255, 241, 251, 255), IM_COL32(215, 157, 197, 255), IM_COL32(211, 105, 174, 255)}; break;
		default: theme = {IM_COL32(91, 216, 255, 255), IM_COL32(174, 119, 255, 255), IM_COL32(4, 7, 24, 255), IM_COL32(12, 20, 49, 255), IM_COL32(24, 59, 85, 255), IM_COL32(244, 249, 253, 255), IM_COL32(142, 177, 201, 255), IM_COL32(126, 165, 192, 255)}; break;
		}
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_accent.GetValue(), 0, 5))
		{
		case 1: theme.accent = IM_COL32(71, 207, 255, 255); break;
		case 2: theme.accent = IM_COL32(177, 105, 255, 255); break;
		case 3: theme.accent = IM_COL32(255, 190, 58, 255); break;
		case 4: theme.accent = IM_COL32(91, 231, 151, 255); break;
		case 5: theme.accent = IM_COL32(255, 92, 78, 255); break;
		default: break;
		}
		static ThemePalette displayed = theme;
		static int blendedFrame = -1;
		const int frame = ImGui::GetFrameCount();
		if (frame != blendedFrame)
		{
			const float response = 1.0f - std::exp(-5.2f * ImGui::GetIO().DeltaTime);
			auto blend = [&](ImU32 from, ImU32 to) {
				auto channel = [&](int shift) {
					const float a = static_cast<float>((from >> shift) & 0xFF);
					const float b = static_cast<float>((to >> shift) & 0xFF);
					return static_cast<uint32>(std::clamp(a + (b - a) * response, 0.0f, 255.0f));
				};
				return channel(0) | (channel(8) << 8) | (channel(16) << 16) | (channel(24) << 24);
			};
			displayed.accent = blend(displayed.accent, theme.accent);
			displayed.secondary = blend(displayed.secondary, theme.secondary);
			displayed.background = blend(displayed.background, theme.background);
			displayed.surface = blend(displayed.surface, theme.surface);
			displayed.selectedSurface = blend(displayed.selectedSurface, theme.selectedSurface);
			displayed.text = blend(displayed.text, theme.text);
			displayed.muted = blend(displayed.muted, theme.muted);
			displayed.border = blend(displayed.border, theme.border);
			blendedFrame = frame;
		}
		return displayed;
	}

	float CornerRadius(float requested)
	{
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_corner_style.GetValue(), 0, 2))
		{
		case 0: return 0.0f;
		case 2: return requested * 1.7f;
		default: return requested * 0.65f;
		}
	}

	std::string Lowercase(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return value;
	}

	std::string Uppercase(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return value;
	}

	std::string ShortName(std::string_view value, size_t length = 24)
	{
		return value.size() <= length ? std::string(value) : std::string(value.substr(0, length - 3)) + "...";
	}

	void PlayUiSound(UiSound sound)
	{
#if BOOST_OS_WINDOWS
		const auto makeWave = [](uint32 sampleCount, float startFrequency, float endFrequency,
			float volume, float secondHarmonic) {
			constexpr uint32 sampleRate = 16000;
			std::vector<uint8> data(44 + sampleCount * 2);
			auto put16 = [&](size_t p, uint16 v) { data[p] = v & 0xFF; data[p + 1] = v >> 8; };
			auto put32 = [&](size_t p, uint32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); };
			memcpy(data.data(), "RIFF", 4);
			put32(4, static_cast<uint32>(data.size()) - 8);
			memcpy(data.data() + 8, "WAVEfmt ", 8);
			put32(16, 16);
			put16(20, 1);
			put16(22, 1);
			put32(24, sampleRate);
			put32(28, sampleRate * 2);
			put16(32, 2);
			put16(34, 16);
			memcpy(data.data() + 36, "data", 4);
			put32(40, sampleCount * 2);
			for (uint32 i = 0; i < sampleCount; ++i)
			{
				const float t = static_cast<float>(i) / sampleRate;
				const float duration = static_cast<float>(sampleCount) / sampleRate;
				const float progress = static_cast<float>(i) / sampleCount;
				const float attack = std::min(1.0f, static_cast<float>(i) / 18.0f);
				const float envelope = attack * std::pow(1.0f - progress, 2.7f);
				const float sweep = (endFrequency - startFrequency) / duration;
				const float phase = 2.0f * kPi * (startFrequency * t + 0.5f * sweep * t * t);
				const float value = std::sin(phase) + std::sin(phase * 2.01f) * secondHarmonic;
				const sint16 sample = static_cast<sint16>(value * envelope * volume);
				put16(44 + i * 2, static_cast<uint16>(sample));
			}
			return data;
		};
		static const std::array<std::vector<uint8>, 4> navigate{
			makeWave(430, 720.0f, 610.0f, 2400.0f, 0.08f), makeWave(310, 1280.0f, 870.0f, 4100.0f, 0.18f),
			makeWave(510, 810.0f, 1120.0f, 3300.0f, 0.25f), makeWave(260, 1650.0f, 1220.0f, 3900.0f, 0.10f)};
		static const std::array<std::vector<uint8>, 4> confirm{
			makeWave(920, 440.0f, 760.0f, 3000.0f, 0.12f), makeWave(760, 610.0f, 1080.0f, 4300.0f, 0.22f),
			makeWave(1120, 390.0f, 980.0f, 3900.0f, 0.30f), makeWave(680, 760.0f, 1320.0f, 4200.0f, 0.14f)};
		static const std::array<std::vector<uint8>, 4> back{
			makeWave(820, 620.0f, 350.0f, 2700.0f, 0.10f), makeWave(650, 820.0f, 410.0f, 4000.0f, 0.20f),
			makeWave(980, 690.0f, 310.0f, 3500.0f, 0.25f), makeWave(590, 1080.0f, 510.0f, 3800.0f, 0.12f)};
		static const std::array<std::vector<uint8>, 4> open{
			makeWave(1320, 300.0f, 680.0f, 2700.0f, 0.14f), makeWave(1050, 390.0f, 920.0f, 4100.0f, 0.22f),
			makeWave(1540, 260.0f, 910.0f, 3700.0f, 0.32f), makeWave(940, 470.0f, 1180.0f, 3900.0f, 0.16f)};
		const int profile = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_sound_profile.GetValue(), 0, 3);
		const std::vector<uint8>* wave = &navigate[profile];
		switch (sound)
		{
		case UiSound::Confirm: wave = &confirm[profile]; break;
		case UiSound::Back: wave = &back[profile]; break;
		case UiSound::Open: wave = &open[profile]; break;
		case UiSound::Navigate: break;
		}
		PlaySoundA(reinterpret_cast<const char*>(wave->data()), nullptr,
			SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#endif
	}

	void PulseHaptics(UiSound sound = UiSound::Navigate)
	{
		if (GetConfig().emulated_usb_devices.skylander_ui_sound)
			PlayUiSound(sound);
		const int strength = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_haptic_strength.GetValue(), 0, 3);
		if (GetConfig().emulated_usb_devices.skylander_ui_haptics && strength > 0 && s_menu.ownerController >= 0)
		{
			if (const auto controller = InputManager::instance().get_controller(s_menu.ownerController))
				controller->start_rumble();
			s_menu.hapticActive = true;
			const int duration = sound == UiSound::Navigate ? (strength == 1 ? 12 : strength == 2 ? 22 : 36) :
				(strength == 1 ? 22 : strength == 2 ? 38 : 58);
			s_menu.hapticStop = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
		}
	}

	void StopMenuHaptics()
	{
		if (s_menu.hapticActive && s_menu.ownerController >= 0)
			if (const auto controller = InputManager::instance().get_controller(s_menu.ownerController))
				controller->stop_rumble();
		s_menu.hapticActive = false;
	}

	void UpdateHaptics()
	{
		if (!s_menu.hapticActive || std::chrono::steady_clock::now() < s_menu.hapticStop)
			return;
		StopMenuHaptics();
	}

	ImU32 AnalyzeAccent(const wxImage& image)
	{
		const unsigned char* pixels = image.GetData();
		if (!pixels)
			return IM_COL32(71, 207, 255, 255);
		const int width = image.GetWidth();
		const int height = image.GetHeight();
		uint64 red = 0;
		uint64 green = 0;
		uint64 blue = 0;
		uint64 weightTotal = 0;
		for (int y = 0; y < std::max(1, height * 2 / 5); y += 2)
		{
			for (int x = width * 2 / 3; x < width; x += 2)
			{
				const size_t offset = (static_cast<size_t>(y) * width + x) * 3;
				const int r = pixels[offset];
				const int g = pixels[offset + 1];
				const int b = pixels[offset + 2];
				const int maximum = std::max({r, g, b});
				const int saturation = maximum - std::min({r, g, b});
				if (maximum < 90 || saturation < 45)
					continue;
				const int weight = saturation * saturation;
				red += static_cast<uint64>(r) * weight;
				green += static_cast<uint64>(g) * weight;
				blue += static_cast<uint64>(b) * weight;
				weightTotal += weight;
			}
		}
		if (!weightTotal)
			return IM_COL32(71, 207, 255, 255);
		int r = static_cast<int>(red / weightTotal);
		int g = static_cast<int>(green / weightTotal);
		int b = static_cast<int>(blue / weightTotal);
		const int maximum = std::max({r, g, b});
		if (maximum > 0)
		{
			const float boost = 235.0f / maximum;
			r = std::clamp(static_cast<int>(r * boost), 35, 255);
			g = std::clamp(static_cast<int>(g * boost), 35, 255);
			b = std::clamp(static_cast<int>(b * boost), 35, 255);
		}
		return IM_COL32(r, g, b, 255);
	}

	struct ArtworkDecodeRequest
	{
		std::vector<std::pair<std::string, fs::path>> artwork;
	};

	struct DecodedAtlasEntry
	{
		std::string key;
		ImVec2 size{};
		ImVec2 uvMinimum{};
		ImVec2 uvMaximum{1.0f, 1.0f};
		ImU32 accent{IM_COL32(71, 207, 255, 255)};
		bool valid{};
	};

	struct DecodedArtworkAtlas
	{
		std::vector<uint8> rgba;
		int width{};
		int height{};
		std::vector<DecodedAtlasEntry> entries;
	};

	class ArtworkDecodeService
	{
	  public:
		ArtworkDecodeService()
			: m_worker([this](std::stop_token stopToken) { WorkerLoop(stopToken); })
		{
		}

		~ArtworkDecodeService()
		{
			m_worker.request_stop();
			m_condition.notify_all();
		}

		void Enqueue(ArtworkDecodeRequest request)
		{
			{
				std::scoped_lock lock(m_mutex);
				m_pending.emplace_back(std::move(request));
			}
			m_condition.notify_one();
		}

		std::optional<DecodedArtworkAtlas> TryPopDecoded()
		{
			std::scoped_lock lock(m_mutex);
			if (m_decoded.empty())
				return std::nullopt;
			DecodedArtworkAtlas decoded = std::move(m_decoded.front());
			m_decoded.pop_front();
			return decoded;
		}

	  private:
		void WorkerLoop(std::stop_token stopToken)
		{
#if BOOST_OS_WINDOWS
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
			while (!stopToken.stop_requested())
			{
				ArtworkDecodeRequest request;
				{
					std::unique_lock lock(m_mutex);
					m_condition.wait(lock, [&] { return stopToken.stop_requested() || !m_pending.empty(); });
					if (stopToken.stop_requested())
						return;
					request = std::move(m_pending.front());
					m_pending.pop_front();
				}

				struct LoadedArtwork
				{
					DecodedAtlasEntry entry;
					std::vector<uint8> rgba;
					int width{};
					int height{};
				};
				DecodedArtworkAtlas atlas;
				std::vector<LoadedArtwork> loaded;
				loaded.reserve(request.artwork.size());
				atlas.entries.reserve(request.artwork.size());
				for (auto& [key, path] : request.artwork)
				{
					LoadedArtwork artwork;
					artwork.entry.key = std::move(key);
					std::error_code pathError;
					const bool readableFile = fs::is_regular_file(path, pathError) && !pathError;
					const uintmax_t sourceSize = readableFile ? fs::file_size(path, pathError) : 0;
					if (readableFile && !pathError && sourceSize <= 8 * 1024 * 1024)
					{
						wxLogNull suppressImageErrors;
						wxImage image;
						if (image.LoadFile(path.wstring()) && image.IsOk() && image.GetData() &&
							image.GetWidth() > 0 && image.GetHeight() > 0 &&
							image.GetWidth() <= 4096 && image.GetHeight() <= 4096)
						{
							constexpr int kThumbnailWidth = 192;
							constexpr int kThumbnailHeight = 256;
							if (image.GetWidth() > kThumbnailWidth || image.GetHeight() > kThumbnailHeight)
							{
								const double scale = std::min(
									static_cast<double>(kThumbnailWidth) / image.GetWidth(),
									static_cast<double>(kThumbnailHeight) / image.GetHeight());
								image.Rescale(std::max(1, static_cast<int>(std::round(image.GetWidth() * scale))),
									std::max(1, static_cast<int>(std::round(image.GetHeight() * scale))),
									wxIMAGE_QUALITY_BILINEAR);
							}

							artwork.width = image.GetWidth();
							artwork.height = image.GetHeight();
							artwork.entry.size = {static_cast<float>(artwork.width), static_cast<float>(artwork.height)};
							artwork.entry.accent = AnalyzeAccent(image);
							artwork.entry.valid = true;
							const size_t pixelCount = static_cast<size_t>(artwork.width) * artwork.height;
							artwork.rgba.resize(pixelCount * 4);
							const uint8* rgb = image.GetData();
							const uint8* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
							for (size_t pixel = 0; pixel < pixelCount; ++pixel)
							{
								artwork.rgba[pixel * 4 + 0] = rgb[pixel * 3 + 0];
								artwork.rgba[pixel * 4 + 1] = rgb[pixel * 3 + 1];
								artwork.rgba[pixel * 4 + 2] = rgb[pixel * 3 + 2];
								artwork.rgba[pixel * 4 + 3] = alpha ? alpha[pixel] : 255;
							}
						}
					}
					if (artwork.entry.valid)
						loaded.emplace_back(std::move(artwork));
					else
						atlas.entries.emplace_back(std::move(artwork.entry));
				}

				if (!loaded.empty())
				{
					constexpr int kAtlasGutter = 4;
					constexpr int kThumbnailWidth = 192;
					constexpr int kThumbnailHeight = 256;
					const int cellWidth = kThumbnailWidth + kAtlasGutter * 2;
					const int cellHeight = kThumbnailHeight + kAtlasGutter * 2;
					const int columns = std::min(8, static_cast<int>(loaded.size()));
					const int rows = (static_cast<int>(loaded.size()) + columns - 1) / columns;
					atlas.width = columns * cellWidth;
					atlas.height = rows * cellHeight;
					atlas.rgba.resize(static_cast<size_t>(atlas.width) * atlas.height * 4, 0);
					for (size_t index = 0; index < loaded.size(); ++index)
					{
						auto& artwork = loaded[index];
						const int column = static_cast<int>(index) % columns;
						const int row = static_cast<int>(index) / columns;
						const int left = column * cellWidth + kAtlasGutter + (kThumbnailWidth - artwork.width) / 2;
						const int top = row * cellHeight + kAtlasGutter + (kThumbnailHeight - artwork.height) / 2;
						for (int y = 0; y < artwork.height; ++y)
						{
							const size_t source = static_cast<size_t>(y) * artwork.width * 4;
							const size_t target = (static_cast<size_t>(top + y) * atlas.width + left) * 4;
							memcpy(atlas.rgba.data() + target, artwork.rgba.data() + source,
								static_cast<size_t>(artwork.width) * 4);
						}
						artwork.entry.uvMinimum = {
							(left + 0.5f) / atlas.width, (top + 0.5f) / atlas.height};
						artwork.entry.uvMaximum = {
							(left + artwork.width - 0.5f) / atlas.width,
							(top + artwork.height - 0.5f) / atlas.height};
						atlas.entries.emplace_back(std::move(artwork.entry));
					}
				}

				{
					std::scoped_lock lock(m_mutex);
					m_decoded.emplace_back(std::move(atlas));
				}
			}
		}

		std::mutex m_mutex;
		std::condition_variable m_condition;
		std::deque<ArtworkDecodeRequest> m_pending;
		std::deque<DecodedArtworkAtlas> m_decoded;
		std::jthread m_worker;
	};

	ArtworkDecodeService& GetArtworkDecodeService()
	{
		static ArtworkDecodeService service;
		return service;
	}

	void PumpArtworkTextureUploads()
	{
		auto decoded = GetArtworkDecodeService().TryPopDecoded();
		if (!decoded)
			return;
		const ImTextureID textureId = decoded->rgba.empty() ? nullptr :
			g_renderer->GenerateTextureRGBA(decoded->rgba, {decoded->width, decoded->height});
		for (const auto& entry : decoded->entries)
		{
			auto cached = s_artworkTextures.find(entry.key);
			if (cached == s_artworkTextures.end())
				continue;
			if (!entry.valid || !textureId)
			{
				cached->second = {};
				continue;
			}
			cached->second = {
				textureId, entry.size, entry.uvMinimum, entry.uvMaximum, entry.accent,
				ArtworkTexture::State::Ready};
		}
	}

	void QueueArtworkAtlas(const std::vector<fs::path>& paths)
	{
		constexpr size_t kMaximumAtlasEntries = 32;
		ArtworkDecodeRequest request;
		request.artwork.reserve(std::min(paths.size(), kMaximumAtlasEntries));
		std::unordered_set<std::string> batchKeys;
		for (const auto& path : paths)
		{
			if (path.empty())
				continue;
			const std::string key = _pathToUtf8(path);
			if (!batchKeys.insert(key).second || s_artworkTextures.contains(key))
				continue;
			ArtworkTexture loading;
			loading.state = ArtworkTexture::State::Loading;
			s_artworkTextures.emplace(key, loading);
			request.artwork.emplace_back(key, path);
			if (request.artwork.size() == kMaximumAtlasEntries)
			{
				GetArtworkDecodeService().Enqueue(std::move(request));
				request = {};
				request.artwork.reserve(kMaximumAtlasEntries);
			}
		}
		if (!request.artwork.empty())
			GetArtworkDecodeService().Enqueue(std::move(request));
	}

	ArtworkTexture GetArtworkTexture(const fs::path& path)
	{
		if (path.empty())
			return {};
		const std::string key = _pathToUtf8(path);
		if (const auto found = s_artworkTextures.find(key); found != s_artworkTextures.end())
			return found->second;
		QueueArtworkAtlas({path});
		return s_artworkTextures.at(key);
	}

	void DrawText(ImDrawList* draw, ImVec2 position, ImU32 color, float size, std::string_view text)
	{
		ImFont* font = ImGui_GetFont(size);
		if (!font)
			font = ImGui::GetFont();
		draw->AddText(font, size, position, color, text.data(), text.data() + text.size());
	}

	void DrawTextFit(ImDrawList* draw, ImVec2 position, ImU32 color, float size, float maxWidth, std::string_view text)
	{
		ImFont* font = ImGui_GetFont(size);
		if (!font)
			font = ImGui::GetFont();
		const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
		const float fitted = measured.x > maxWidth ? size * maxWidth / measured.x : size;
		draw->AddText(font, fitted, position, color, text.data(), text.data() + text.size());
	}

	void DrawTextCentered(ImDrawList* draw, ImVec2 center, ImU32 color, float size, std::string_view text)
	{
		ImFont* font = ImGui_GetFont(size);
		if (!font)
			font = ImGui::GetFont();
		const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f,
			text.data(), text.data() + text.size());
		draw->AddText(font, size, {center.x - measured.x * 0.5f, center.y - measured.y * 0.5f},
			color, text.data(), text.data() + text.size());
	}

	void DrawTextRightAligned(ImDrawList* draw, ImVec2 right, ImU32 color, float size, std::string_view text)
	{
		ImFont* font = ImGui_GetFont(size);
		if (!font)
			font = ImGui::GetFont();
		const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f,
			text.data(), text.data() + text.size());
		draw->AddText(font, size, {right.x - measured.x, right.y}, color,
			text.data(), text.data() + text.size());
	}

	void DrawShadowedText(ImDrawList* draw, ImVec2 position, ImU32 color, float size,
		std::string_view text, float scale)
	{
		const float alpha = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
		DrawText(draw, {position.x + 4.0f * scale, position.y + 5.0f * scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), alpha * 0.30f), size, text);
		DrawText(draw, {position.x + 2.0f * scale, position.y + 3.0f * scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), alpha * 0.72f), size, text);
		DrawText(draw, position, color, size, text);
	}

	void DrawShadowedTextRightAligned(ImDrawList* draw, ImVec2 right, ImU32 color, float size,
		std::string_view text, float scale)
	{
		const float alpha = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
		DrawTextRightAligned(draw, {right.x + 4.0f * scale, right.y + 5.0f * scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), alpha * 0.30f), size, text);
		DrawTextRightAligned(draw, {right.x + 2.0f * scale, right.y + 3.0f * scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), alpha * 0.72f), size, text);
		DrawTextRightAligned(draw, right, color, size, text);
	}

	void DrawGlow(ImDrawList* draw, ImVec2 center, float radius, ImU32 color, float alpha)
	{
		const int bloom = BloomLevel();
		if (bloom == 0 || alpha <= 0.0f)
			return;
		const float strength = bloom == 2 ? 1.45f : 1.0f;
		const int ringCount = bloom == 2 ? 3 : 2;
		const int segments = bloom == 2 ? 32 : 24;
		for (int ring = ringCount; ring >= 1; --ring)
		{
			const float fraction = static_cast<float>(ring) / ringCount;
			draw->AddCircleFilled(center, radius * (0.70f + fraction * 0.30f) *
				(bloom == 2 ? 1.08f : 1.0f),
				WithAlpha(color, alpha * strength * (0.045f + (1.0f - fraction) * 0.15f)), segments);
		}
	}

	void DrawCardAura(ImDrawList* draw, ImVec2 center, float radius, ImU32 color, float intensity, float alpha)
	{
		if (intensity < 0.01f || BloomLevel() == 0)
			return;
		const float time = static_cast<float>(ImGui::GetTime());
		const float pulse = MotionLevel() == 2 ? 0.5f + 0.5f * std::sin(time * 3.1f) : 0.5f;
		draw->AddCircle(center, radius * (0.90f + pulse * 0.07f),
			WithAlpha(color, alpha * intensity * (0.18f + pulse * 0.12f)), 32,
			(1.0f + pulse * 0.8f) * intensity);
		const int particles = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_particle_level.GetValue(), 0, 3);
		if (MotionLevel() != 2 || particles == 0)
			return;
		const int sparkCount = particles == 1 ? 3 : particles == 2 ? 5 : 8;
		for (int spark = 0; spark < sparkCount; ++spark)
		{
			const float angle = time * (0.48f + spark * 0.025f) + spark * (2.0f * kPi / sparkCount);
			const float orbit = radius * (0.78f + 0.10f * std::sin(time * 1.7f + spark));
			const ImVec2 position{center.x + std::cos(angle) * orbit, center.y + std::sin(angle) * orbit};
			draw->AddCircleFilled(position, (1.6f + (spark % 3) * 0.7f) * intensity,
				WithAlpha(color, alpha * intensity * (0.34f + pulse * 0.26f)), 8);
		}
	}

	void DrawGlassPanel(ImDrawList* draw, const RiftLayout& layout, ImVec2 minimum, ImVec2 maximum,
		float rounding = 8.0f, bool strong = false)
	{
		const auto theme = CurrentTheme();
		const float radius = CornerRadius(rounding) * layout.scale;
		for (int layer = 4; layer >= 1; --layer)
		{
			const float x = DrawerWidth(layout, 2.0f + layer * 2.0f);
			const float y = (3.0f + layer * 2.8f) * layout.scale;
			draw->AddRectFilled({minimum.x + x, minimum.y + y}, {maximum.x + x, maximum.y + y},
				WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * (0.055f + layer * 0.030f)), radius);
		}
		const float opacity = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_drawer_opacity.GetValue(), 0, 100) / 100.0f;
		const float surfaceAlpha = (strong ? 0.78f : 0.64f) * (0.45f + opacity * 0.55f);
		draw->AddRectFilled(minimum, maximum,
			WithAlpha(strong ? theme.surface : Lighten(theme.surface, 0.06f),
				layout.alpha * surfaceAlpha), radius);
		draw->AddLine({minimum.x + radius, minimum.y + 1.0f * layout.scale},
			{maximum.x - radius, minimum.y + 1.0f * layout.scale},
			WithAlpha(theme.text, layout.alpha * 0.15f), 1.0f * layout.scale);
		draw->AddRect(minimum, maximum, WithAlpha(theme.border, layout.alpha * 0.38f),
			radius, 0, 1.2f * layout.scale);
	}

	void DrawElevatedSurface(ImDrawList* draw, const RiftLayout& layout, ImVec2 minimum, ImVec2 maximum,
		bool active = false, ImU32 accent = 0)
	{
		const auto theme = CurrentTheme();
		if (!accent)
			accent = theme.accent;
		const float radius = CornerRadius(5.0f) * layout.scale;
		for (int layer = 3; layer >= 1; --layer)
		{
			const float offsetX = DrawerWidth(layout, 1.0f + layer * 1.2f);
			const float offsetY = (2.0f + layer * 1.7f) * layout.scale;
			draw->AddRectFilled({minimum.x + offsetX, minimum.y + offsetY},
				{maximum.x + offsetX, maximum.y + offsetY},
				WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * (0.055f + layer * 0.035f)), radius);
		}
		draw->AddRectFilled(minimum, maximum,
			WithAlpha(active ? theme.selectedSurface : theme.surface, layout.alpha * (active ? 0.92f : 0.78f)), radius);
		draw->AddLine({minimum.x + radius, minimum.y + layout.scale},
			{maximum.x - radius, minimum.y + layout.scale},
			WithAlpha(theme.text, layout.alpha * (active ? 0.18f : 0.11f)), layout.scale);
		draw->AddRect(minimum, maximum,
			WithAlpha(active ? accent : theme.border, layout.alpha * (active ? 0.72f : 0.36f)),
			radius, 0, (active ? 1.5f : 1.0f) * layout.scale);
	}

	struct CardGeometry
	{
		ImVec2 p0{};
		ImVec2 p1{};
		ImVec2 p2{};
		ImVec2 p3{};
	};

	CardGeometry MakeCardQuad(ImVec2 center, float width, float height, float rotation)
	{
		const float cosine = std::cos(rotation);
		const float sine = std::sin(rotation);
		auto rotate = [&](float x, float y) {
			return ImVec2{center.x + x * cosine - y * sine, center.y + x * sine + y * cosine};
		};
		return {
			rotate(-width * 0.5f, -height * 0.5f),
			rotate(width * 0.5f, -height * 0.5f),
			rotate(width * 0.5f, height * 0.5f),
			rotate(-width * 0.5f, height * 0.5f)};
	}

	void DrawCard(ImDrawList* draw, const ArtworkTexture& texture, ImVec2 center, float height,
		float rotation, float focus, float alpha)
	{
		focus = Clamp01(focus);
		const float aspect = texture.id && texture.size.y > 0.0f ? texture.size.x / texture.size.y : 0.64f;
		const float width = height * aspect;
		const auto card = MakeCardQuad(center, width, height, rotation);
		const ImU32 accent = texture.id ? texture.accent : CurrentTheme().accent;
		const int effect = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_card_effect.GetValue(), 0, 3);
		const int borderStyle = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_card_border.GetValue(), 0, 3);
		for (int layer = 6; layer >= 1; --layer)
		{
			const float offsetX = 1.0f + layer * 0.85f;
			const float offsetY = 2.0f + layer * 1.1f;
			draw->AddQuadFilled(
				{card.p0.x + offsetX, card.p0.y + offsetY}, {card.p1.x + offsetX, card.p1.y + offsetY},
				{card.p2.x + offsetX, card.p2.y + offsetY}, {card.p3.x + offsetX, card.p3.y + offsetY},
				WithAlpha(IM_COL32(0, 0, 0, 255), alpha * (0.020f + focus * 0.018f)));
		}
		if (focus > 0.001f && effect >= 1)
		{
			const int outer = effect >= 2 ? 14 : 8;
			for (int outline = outer; outline >= 2; outline -= 2)
				draw->AddQuad(card.p0, card.p1, card.p2, card.p3,
					WithAlpha(accent, alpha * focus * (0.018f + (outer - outline) * 0.006f)), outline + 2.0f);
		}
		if (texture.state == ArtworkTexture::State::Ready && texture.id)
		{
			draw->AddImageQuad(texture.id, card.p0, card.p1, card.p2, card.p3,
				texture.uvMinimum, {texture.uvMaximum.x, texture.uvMinimum.y},
				texture.uvMaximum, {texture.uvMinimum.x, texture.uvMaximum.y},
				WithAlpha(IM_COL32_WHITE, alpha));
			if (focus > 0.001f && effect == 3)
			{
				const float shimmer = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.7f);
				const ImVec2 a{card.p0.x + (card.p1.x - card.p0.x) * shimmer, card.p0.y + (card.p1.y - card.p0.y) * shimmer};
				const ImVec2 b{card.p3.x + (card.p2.x - card.p3.x) * shimmer, card.p3.y + (card.p2.y - card.p3.y) * shimmer};
				draw->AddLine(a, b, WithAlpha(IM_COL32_WHITE, alpha * focus * 0.10f), std::max(1.0f, height * 0.025f));
			}
		}
		else
		{
			draw->AddQuadFilled(card.p0, card.p1, card.p2, card.p3,
				WithAlpha(texture.state == ArtworkTexture::State::Loading ?
					IM_COL32(18, 43, 64, 255) : IM_COL32(24, 32, 49, 255), alpha));
			const bool loading = texture.state == ArtworkTexture::State::Loading;
			DrawTextCentered(draw, {center.x, center.y - height * 0.035f},
				WithAlpha(IM_COL32(166, 207, 230, 255), alpha * 0.82f),
				std::max(18.0f, height * (loading ? 0.12f : 0.18f)), loading ? "..." : "?");
			if (!loading && height >= 100.0f)
				DrawTextCentered(draw, {center.x, center.y + height * 0.17f},
					WithAlpha(IM_COL32(124, 158, 181, 255), alpha * 0.70f),
					std::max(8.0f, height * 0.042f), "NO ART");
		}
		const ImU32 borderColor = BlendColor(borderStyle == 0 ? IM_COL32(164, 190, 213, 255) : accent,
			Lighten(accent, 0.25f), focus);
		if (borderStyle >= 2)
			draw->AddQuad(card.p0, card.p1, card.p2, card.p3,
				WithAlpha(IM_COL32(3, 7, 14, 255), alpha * 0.86f), 4.0f + focus * 1.5f);
		const float restingOpacity = borderStyle == 0 ? 0.30f : 0.62f;
		const float restingWidth = borderStyle >= 2 ? 2.0f : 1.35f;
		draw->AddQuad(card.p0, card.p1, card.p2, card.p3,
			WithAlpha(borderColor, alpha * (restingOpacity + (0.96f - restingOpacity) * focus)),
			restingWidth + (3.0f - restingWidth) * focus);
		if (borderStyle == 3)
		{
			constexpr float cornerLength = 0.16f;
			auto edgePoint = [](ImVec2 from, ImVec2 to, float t) {
				return ImVec2{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
			};
			const ImU32 runeColor = WithAlpha(Lighten(accent, 0.36f), alpha * 0.95f);
			draw->AddLine(card.p0, edgePoint(card.p0, card.p1, cornerLength), runeColor, 3.0f);
			draw->AddLine(card.p0, edgePoint(card.p0, card.p3, cornerLength), runeColor, 3.0f);
			draw->AddLine(card.p2, edgePoint(card.p2, card.p1, cornerLength), runeColor, 3.0f);
			draw->AddLine(card.p2, edgePoint(card.p2, card.p3, cornerLength), runeColor, 3.0f);
		}
	}

	void DrawStar(ImDrawList* draw, ImVec2 center, float radius, ImU32 color)
	{
		std::array<ImVec2, 10> points{};
		for (int i = 0; i < 10; ++i)
		{
			const float angle = -kPi * 0.5f + i * kPi / 5.0f;
			const float r = i % 2 ? radius * 0.44f : radius;
			points[i] = {center.x + std::cos(angle) * r, center.y + std::sin(angle) * r};
		}
		draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
	}

	void SetToast(std::string value, float seconds = 2.2f)
	{
		s_menu.toast = std::move(value);
		s_menu.toastLife = seconds;
	}

	void SetMenuOpen(bool open, bool pulse = true)
	{
		if (s_menu.requestedOpen == open)
			return;
		s_menu.requestedOpen = open;
		s_menu.neutralInputFrames = 0;
		if (open)
		{
			s_menu.page = RiftPage::Dashboard;
			s_menu.focus = FocusArea::Library;
			s_menu.selectedElementFilter = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_element_filter.GetValue(), 0, 11);
			s_menu.portalModeFlash = MotionEnabled() ? 0.72f : 0.0f;
			s_menu.openChordBlockedUntilRelease = true;
			s_menu.inputCaptureLatched = true;
			EmulatedController::SetRiftInputCaptured(true);
		}
		else
		{
			s_menu.searchTarget = SearchTarget::None;
			s_menu.reloadCatalog = true;
		}
		if (pulse)
			PulseHaptics(open ? UiSound::Open : UiSound::Back);
	}

	void RequestUpdateCheck()
	{
		SetMenuOpen(false, false);
		s_updateCheckRequested.store(true, std::memory_order_release);
		PulseHaptics(UiSound::Confirm);
	}

	void ResetMenuInteraction()
	{
		StopMenuHaptics();
		s_menu.requestedOpen = false;
		s_menu.inputCaptureLatched = false;
		s_menu.controlsNeutral = true;
		s_menu.openChordBlockedUntilRelease = false;
		s_menu.neutralInputFrames = 0;
		s_menu.visibility = 0.0f;
		s_menu.searchTarget = SearchTarget::None;
		s_menu.forgeToolbarFocused = false;
		s_menu.backWasDown = false;
		s_menu.upWasDown = false;
		s_menu.downWasDown = false;
		s_menu.leftWasDown = false;
		s_menu.rightWasDown = false;
		s_menu.acceptWasDown = false;
		s_menu.removeWasDown = false;
		s_menu.detailsWasDown = false;
		s_menu.sortWasDown = false;
		s_menu.favoriteWasDown = false;
		s_menu.ownerController = -1;
		s_menu.nextNavigationRepeat.fill(0.0);
		EmulatedController::SetRiftInputCaptured(false);
	}

	void SetPortalMode(bool virtualPortal)
	{
		auto& config = GetConfig().emulated_usb_devices;
		const bool alreadyActive = config.emulate_skylander_portal.GetValue() == virtualPortal &&
			config.skylander_portal_mode.GetValue() == (virtualPortal ? 0 : 1);
		s_menu.headerSelection = virtualPortal ? 0 : 1;
		if (alreadyActive)
		{
			SetToast(virtualPortal ? "VIRTUAL PORTAL IS ALREADY ACTIVE" : "PHYSICAL PORTAL IS ALREADY ACTIVE", 1.8f);
			return;
		}
		config.emulate_skylander_portal = virtualPortal;
		config.skylander_portal_mode = virtualPortal ? 0 : 1;
		g_config.Save();
		nsyshid::backend::SetSkylanderPortalEmulation(virtualPortal);
		s_menu.portalModeFlash = 1.0f;
		SetToast(virtualPortal ? "VIRTUAL PORTAL ACTIVE" : "PHYSICAL PORTAL ACTIVE", 2.4f);
	}

	std::string SafeFileName(std::string value)
	{
		for (char& c : value)
			if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
				c = '-';
		return value;
	}

	bool CreateFigure(const skylander_ui::FigureDefinition& figure)
	{
		const std::string configuredFolder = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
		if (configuredFolder.empty())
			return false;
		const fs::path folder = _utf8ToPath(configuredFolder);
		std::error_code ec;
		fs::create_directories(folder, ec);
		if (ec)
			return false;
		const std::string base = SafeFileName(figure.name);
		fs::path output = folder / _utf8ToPath(base + ".sky");
		for (unsigned copy = 2; fs::exists(output); ++copy)
			output = folder / _utf8ToPath(fmt::format("{} ({}).sky", base, copy));
		return nsyshid::g_skyportal.CreateSkylander(output, figure.id, figure.variant);
	}

	uint8 PlaceFigure(const skylander_ui::CollectionFigure& figure, int replaceSlot)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(figure.filePath, true));
		if (!file)
			return 0xFF;
		std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data{};
		if (file->readData(data.data(), data.size()) != data.size())
			return 0xFF;
		if (replaceSlot >= 0)
			nsyshid::g_skyportal.RemoveSkylander(static_cast<uint8>(replaceSlot));
		return nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(file));
	}

	std::string FavoriteKey(const fs::path& path)
	{
		return Lowercase(_pathToUtf8(path.lexically_normal()));
	}

	void LoadPreferences()
	{
		s_menu.favorites.clear();
		const std::string serialized = GetConfig().emulated_usb_devices.skylander_favorites.GetValue();
		size_t start = 0;
		while (start < serialized.size())
		{
			const size_t separator = serialized.find('|', start);
			const std::string key = serialized.substr(start,
				separator == std::string::npos ? std::string::npos : separator - start);
			if (!key.empty())
				s_menu.favorites.insert(key);
			if (separator == std::string::npos)
				break;
			start = separator + 1;
		}
		const sint32 sort = GetConfig().emulated_usb_devices.skylander_sort_mode.GetValue();
		if (sort < 0 || sort >= static_cast<sint32>(SortMode::Count))
			GetConfig().emulated_usb_devices.skylander_sort_mode = 0;
	}

	bool IsFavorite(const fs::path& path)
	{
		return s_menu.favorites.contains(FavoriteKey(path));
	}

	void SaveFavorites()
	{
		std::vector<std::string> keys(s_menu.favorites.begin(), s_menu.favorites.end());
		std::sort(keys.begin(), keys.end());
		std::string serialized;
		for (const auto& key : keys)
		{
			if (!serialized.empty())
				serialized += '|';
			serialized += key;
		}
		GetConfig().emulated_usb_devices.skylander_favorites = serialized;
		g_config.Save();
	}

	int WrapChoice(int value, int count)
	{
		value %= count;
		return value < 0 ? value + count : value;
	}

	skylander_ui::FigureElement ElementForChoice(int choice)
	{
		using skylander_ui::FigureElement;
		switch (choice)
		{
		case 1: return FigureElement::Air;
		case 2: return FigureElement::Earth;
		case 3: return FigureElement::Fire;
		case 4: return FigureElement::Water;
		case 5: return FigureElement::Magic;
		case 6: return FigureElement::Tech;
		case 7: return FigureElement::Life;
		case 8: return FigureElement::Undead;
		case 9: return FigureElement::Light;
		case 10: return FigureElement::Dark;
		case 11: return FigureElement::Kaos;
		default: return FigureElement::Unknown;
		}
	}

	std::string_view ElementChoiceName(int choice, std::string_view noneLabel)
	{
		switch (choice)
		{
		case 1: return "AIR";
		case 2: return "EARTH";
		case 3: return "FIRE";
		case 4: return "WATER";
		case 5: return "MAGIC";
		case 6: return "TECH";
		case 7: return "LIFE";
		case 8: return "UNDEAD";
		case 9: return "LIGHT";
		case 10: return "DARK";
		case 11: return "KAOS";
		default: return noneLabel;
		}
	}

	ImU32 ElementColor(skylander_ui::FigureElement element, ImU32 fallback)
	{
		using skylander_ui::FigureElement;
		switch (element)
		{
		case FigureElement::Air: return IM_COL32(126, 213, 255, 255);
		case FigureElement::Earth: return IM_COL32(201, 145, 76, 255);
		case FigureElement::Fire: return IM_COL32(255, 92, 55, 255);
		case FigureElement::Water: return IM_COL32(64, 163, 255, 255);
		case FigureElement::Magic: return IM_COL32(182, 103, 255, 255);
		case FigureElement::Tech: return IM_COL32(255, 190, 58, 255);
		case FigureElement::Life: return IM_COL32(92, 226, 111, 255);
		case FigureElement::Undead: return IM_COL32(142, 94, 211, 255);
		case FigureElement::Light: return IM_COL32(255, 235, 147, 255);
		case FigureElement::Dark: return IM_COL32(101, 91, 171, 255);
		case FigureElement::Kaos: return IM_COL32(228, 72, 194, 255);
		default: return fallback;
		}
	}

	int ElementRank(skylander_ui::FigureElement element)
	{
		using skylander_ui::FigureElement;
		switch (element)
		{
		case FigureElement::Air: return 0;
		case FigureElement::Earth: return 1;
		case FigureElement::Fire: return 2;
		case FigureElement::Water: return 3;
		case FigureElement::Magic: return 4;
		case FigureElement::Tech: return 5;
		case FigureElement::Life: return 6;
		case FigureElement::Undead: return 7;
		case FigureElement::Light: return 8;
		case FigureElement::Dark: return 9;
		case FigureElement::Kaos: return 10;
		case FigureElement::None: return 11;
		default: return 12;
		}
	}

	int FigureTypeRank(skylander_ui::FigureType type)
	{
		return type == skylander_ui::FigureType::Unknown ? 99 : static_cast<int>(type);
	}

	bool IsAccessoryType(skylander_ui::FigureType type)
	{
		using skylander_ui::FigureType;
		return type == FigureType::Item || type == FigureType::CreationCrystal ||
			type == FigureType::RacingDriver;
	}

	std::string_view PriorityModeName()
	{
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_priority_mode.GetValue(), 0, 4))
		{
		case 1: return "FAVORITES FIRST";
		case 2: return "TRAPS FIRST";
		case 3: return "SKYLANDERS FIRST";
		case 4: return "ITEMS FIRST";
		default: return "NONE";
		}
	}

	std::string_view FilterModeName()
	{
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_filter_mode.GetValue(), 0, 5))
		{
		case 1: return "FAVORITES";
		case 2: return "SKYLANDERS";
		case 3: return "TRAPS";
		case 4: return "VEHICLES";
		case 5: return "ITEMS & CRYSTALS";
		default: return "EVERYTHING";
		}
	}

	SortMode CurrentSortMode()
	{
		return static_cast<SortMode>(std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_sort_mode.GetValue(), 0,
			static_cast<sint32>(SortMode::Count) - 1));
	}

	std::string_view SortModeName()
	{
		switch (CurrentSortMode())
		{
		case SortMode::NameAscending: return "NAME A-Z";
		case SortMode::NameDescending: return "NAME Z-A";
		case SortMode::FavoritesFirst: return "FAVORITES FIRST";
		case SortMode::NewestFiles: return "NEWEST FILES";
		case SortMode::FavoritesOnly: return "FAVORITES ONLY";
		case SortMode::Element: return "ELEMENT";
		case SortMode::FigureType: return "FIGURE TYPE";
		default: return "NAME A-Z";
		}
	}

	void RebuildLibrary()
	{
		std::string preserve;
		if (!s_menu.library.empty() && s_menu.selectedLibrary >= 0 &&
			s_menu.selectedLibrary < static_cast<int>(s_menu.library.size()))
			preserve = FavoriteKey(s_menu.library[s_menu.selectedLibrary].filePath);
		const std::string query = Lowercase(s_menu.searchText.data());
		s_menu.library.clear();
		s_cardCarouselOffsets.clear();
		s_cardFocusAmounts.clear();
		for (const auto& figure : s_menu.catalog.GetCollection())
		{
			if (CurrentSortMode() == SortMode::FavoritesOnly && !IsFavorite(figure.filePath))
				continue;
			const int filterMode = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_filter_mode.GetValue(), 0, 5);
			using skylander_ui::FigureType;
			if ((filterMode == 1 && !IsFavorite(figure.filePath)) ||
				(filterMode == 2 && figure.type != FigureType::Skylander) ||
				(filterMode == 3 && figure.type != FigureType::Trap) ||
				(filterMode == 4 && figure.type != FigureType::Vehicle) ||
				(filterMode == 5 && !IsAccessoryType(figure.type)))
				continue;
			const int elementFilter = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_element_filter.GetValue(), 0, 11);
			if (elementFilter != 0 && figure.element != ElementForChoice(elementFilter))
				continue;
			const std::string haystack = Lowercase(figure.name + " " + _pathToUtf8(figure.filePath.filename()));
			if (!query.empty() && haystack.find(query) == std::string::npos)
				continue;
			s_menu.library.push_back(figure);
		}

		auto byName = [](const auto& lhs, const auto& rhs) {
			return Lowercase(lhs.name) < Lowercase(rhs.name);
		};
		switch (CurrentSortMode())
		{
		case SortMode::NameDescending:
			std::sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				return byName(rhs, lhs);
			});
			break;
		case SortMode::FavoritesFirst:
			std::sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				const bool leftFavorite = IsFavorite(lhs.filePath);
				const bool rightFavorite = IsFavorite(rhs.filePath);
				return leftFavorite != rightFavorite ? leftFavorite : byName(lhs, rhs);
			});
			break;
		case SortMode::NewestFiles:
		{
			std::unordered_map<std::string, fs::file_time_type> modificationTimes;
			for (const auto& figure : s_menu.library)
			{
				std::error_code error;
				const auto modified = fs::last_write_time(figure.filePath, error);
				modificationTimes.emplace(FavoriteKey(figure.filePath),
					error ? fs::file_time_type::min() : modified);
			}
			std::sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				const auto leftTime = modificationTimes.at(FavoriteKey(lhs.filePath));
				const auto rightTime = modificationTimes.at(FavoriteKey(rhs.filePath));
				if (leftTime != rightTime)
					return leftTime > rightTime;
				return byName(lhs, rhs);
			});
			break;
		}
		case SortMode::Element:
			std::stable_sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				const auto left = std::tuple{ElementRank(lhs.element), Lowercase(lhs.name), lhs.id, lhs.variant,
					FavoriteKey(lhs.filePath)};
				const auto right = std::tuple{ElementRank(rhs.element), Lowercase(rhs.name), rhs.id, rhs.variant,
					FavoriteKey(rhs.filePath)};
				return left < right;
			});
			break;
		case SortMode::FigureType:
			std::sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				return lhs.type != rhs.type ? FigureTypeRank(lhs.type) < FigureTypeRank(rhs.type) : byName(lhs, rhs);
			});
			break;
		default:
			std::sort(s_menu.library.begin(), s_menu.library.end(), byName);
			break;
		}

		const int elementPriority = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_element_priority.GetValue(), 0, 11);
		if (elementPriority != 0 && CurrentSortMode() != SortMode::FigureType)
		{
			const auto preferred = ElementForChoice(elementPriority);
			std::stable_sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				return (lhs.element == preferred) > (rhs.element == preferred);
			});
		}
		const int priorityMode = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_priority_mode.GetValue(), 0, 4);
		if (priorityMode != 0 && CurrentSortMode() != SortMode::Element &&
			CurrentSortMode() != SortMode::FigureType)
		{
			using skylander_ui::FigureType;
			auto preferred = [&](const auto& figure) {
				switch (priorityMode)
				{
				case 1: return IsFavorite(figure.filePath);
				case 2: return figure.type == FigureType::Trap;
				case 3: return figure.type == FigureType::Skylander;
				case 4: return IsAccessoryType(figure.type);
				default: return false;
				}
			};
			std::stable_sort(s_menu.library.begin(), s_menu.library.end(), [&](const auto& lhs, const auto& rhs) {
				return preferred(lhs) > preferred(rhs);
			});
		}

		s_menu.selectedLibrary = std::clamp(s_menu.selectedLibrary, 0,
			std::max(0, static_cast<int>(s_menu.library.size()) - 1));
		if (!preserve.empty())
		{
			const auto found = std::find_if(s_menu.library.begin(), s_menu.library.end(), [&](const auto& figure) {
				return FavoriteKey(figure.filePath) == preserve;
			});
			if (found != s_menu.library.end())
				s_menu.selectedLibrary = static_cast<int>(std::distance(s_menu.library.begin(), found));
		}
		std::vector<fs::path> dashboardArtwork;
		dashboardArtwork.reserve(s_menu.catalog.GetCollection().size() +
			s_menu.catalog.GetCreatableDefinitions().size() + 10);
		for (const auto& figure : s_menu.catalog.GetCollection())
			if (!figure.imagePath.empty())
				dashboardArtwork.emplace_back(figure.imagePath);
		for (const auto& definition : s_menu.catalog.GetCreatableDefinitions())
			if (!definition.imagePath.empty())
				dashboardArtwork.emplace_back(definition.imagePath);
		const fs::path glyphFolder = s_menu.catalog.GetAssetsPath() / "ui" / "glyphs" / "kenney" /
			"Xbox Series" / "Default";
		for (std::string_view glyph : {"xbox_button_a.png", "xbox_button_b.png", "xbox_button_x.png",
			"xbox_button_y.png", "xbox_lb.png", "xbox_rb.png", "xbox_dpad_down.png",
			"xbox_button_menu.png", "xbox_button_view.png"})
			dashboardArtwork.emplace_back(glyphFolder / glyph);
		QueueArtworkAtlas(dashboardArtwork);
		s_menu.rebuildLibrary = false;
	}

	void ReloadCatalogIfNeeded()
	{
		if (!s_menu.reloadCatalog)
			return;
		s_menu.catalog.Reload();
		LoadPreferences();
		RebuildLibrary();
		s_menu.reloadCatalog = false;
	}

	void CycleSortMode()
	{
		const sint32 next = (static_cast<sint32>(CurrentSortMode()) + 1) %
			static_cast<sint32>(SortMode::Count);
		GetConfig().emulated_usb_devices.skylander_sort_mode = next;
		g_config.Save();
		RebuildLibrary();
		SetToast("SORT: " + std::string(SortModeName()));
		PulseHaptics();
	}

	void ToggleFavoritePath(const fs::path& filePath, std::string_view name)
	{
		if (filePath.empty())
			return;
		const std::string key = FavoriteKey(filePath);
		const bool added = !s_menu.favorites.contains(key);
		if (added)
			s_menu.favorites.insert(key);
		else
			s_menu.favorites.erase(key);
		SaveFavorites();
		SetToast(Uppercase(std::string(name)) +
			(added ? " ADDED TO FAVORITES" : " REMOVED FROM FAVORITES"));
		RebuildLibrary();
		PulseHaptics(UiSound::Confirm);
	}

	void ToggleFavorite()
	{
		if (s_menu.library.empty())
			return;
		const auto& figure = s_menu.library[s_menu.selectedLibrary];
		ToggleFavoritePath(figure.filePath, figure.name);
	}

	std::vector<PortalRow> BuildPortalRows()
	{
		std::vector<PortalRow> rows;
		const auto loaded = nsyshid::g_skyportal.GetLoadedSkylanders();
		rows.reserve(kPortalCapacity);
		for (int slot = 0; slot < static_cast<int>(loaded.size()); ++slot)
		{
			if (!loaded[slot])
				continue;
			const auto* definition = s_menu.catalog.Find(loaded[slot]->first, loaded[slot]->second);
			rows.push_back({slot, loaded[slot]->first, loaded[slot]->second, definition, 0});
		}
		if (rows.size() < kPortalCapacity)
			rows.emplace_back();
		return rows;
	}

	std::string_view ElementName(skylander_ui::FigureElement element)
	{
		using skylander_ui::FigureElement;
		switch (element)
		{
		case FigureElement::Air: return "AIR";
		case FigureElement::Earth: return "EARTH";
		case FigureElement::Fire: return "FIRE";
		case FigureElement::Water: return "WATER";
		case FigureElement::Magic: return "MAGIC";
		case FigureElement::Tech: return "TECH";
		case FigureElement::Life: return "LIFE";
		case FigureElement::Undead: return "UNDEAD";
		case FigureElement::Light: return "LIGHT";
		case FigureElement::Dark: return "DARK";
		case FigureElement::Kaos: return "KAOS";
		case FigureElement::None: return "ITEM";
		default: return "UNKNOWN";
		}
	}

	std::string_view FigureTypeName(skylander_ui::FigureType type)
	{
		using skylander_ui::FigureType;
		switch (type)
		{
		case FigureType::Skylander: return "SKYLANDER";
		case FigureType::Trap: return "TRAP";
		case FigureType::Vehicle: return "VEHICLE";
		case FigureType::Item: return "ITEM";
		case FigureType::CreationCrystal: return "CRYSTAL";
		case FigureType::RacingDriver: return "DRIVER";
		default: return "OTHER";
		}
	}

	std::string_view ForgeSortName()
	{
		switch (static_cast<ForgeSortMode>(s_menu.forgeSortMode))
		{
		case ForgeSortMode::NameDescending: return "NAME Z-A";
		case ForgeSortMode::Element: return "ELEMENT";
		case ForgeSortMode::FigureType: return "FIGURE TYPE";
		default: return "NAME A-Z";
		}
	}

	std::string_view ForgeTypeFilterName()
	{
		switch (s_menu.forgeTypeFilter)
		{
		case 1: return "SKYLANDERS";
		case 2: return "TRAPS";
		case 3: return "VEHICLES";
		case 4: return "ITEMS";
		default: return "ALL TYPES";
		}
	}

	bool MatchesForgeType(skylander_ui::FigureType type)
	{
		using skylander_ui::FigureType;
		switch (s_menu.forgeTypeFilter)
		{
		case 1: return type == FigureType::Skylander;
		case 2: return type == FigureType::Trap;
		case 3: return type == FigureType::Vehicle;
		case 4: return type == FigureType::Item || type == FigureType::CreationCrystal ||
				type == FigureType::RacingDriver;
		default: return true;
		}
	}

	std::vector<const skylander_ui::FigureDefinition*> BuildForgeDefinitions()
	{
		std::vector<const skylander_ui::FigureDefinition*> filtered;
		const auto& definitions = s_menu.catalog.GetCreatableDefinitions();
		filtered.reserve(definitions.size());
		const std::string query = Lowercase(s_menu.forgeSearchText.data());
		for (const auto& definition : definitions)
		{
			if (s_menu.forgeElementFilter != 0 &&
				definition.element != ElementForChoice(s_menu.forgeElementFilter))
				continue;
			if (!MatchesForgeType(definition.type))
				continue;
			const std::string haystack = Lowercase(fmt::format("{} {} {} {:04X} {:04X}",
				definition.name, ElementName(definition.element), FigureTypeName(definition.type),
				definition.id, definition.variant));
			if (!query.empty() && haystack.find(query) == std::string::npos)
				continue;
			filtered.emplace_back(&definition);
		}
		auto byName = [](const auto* lhs, const auto* rhs) {
			return std::tuple{Lowercase(lhs->name), lhs->id, lhs->variant} <
				std::tuple{Lowercase(rhs->name), rhs->id, rhs->variant};
		};
		switch (static_cast<ForgeSortMode>(s_menu.forgeSortMode))
		{
		case ForgeSortMode::NameDescending:
			std::sort(filtered.begin(), filtered.end(), [&](const auto* lhs, const auto* rhs) {
				return byName(rhs, lhs);
			});
			break;
		case ForgeSortMode::Element:
			std::sort(filtered.begin(), filtered.end(), [&](const auto* lhs, const auto* rhs) {
				const auto left = std::tuple{ElementRank(lhs->element), Lowercase(lhs->name), lhs->id, lhs->variant};
				const auto right = std::tuple{ElementRank(rhs->element), Lowercase(rhs->name), rhs->id, rhs->variant};
				return left < right;
			});
			break;
		case ForgeSortMode::FigureType:
			std::sort(filtered.begin(), filtered.end(), [&](const auto* lhs, const auto* rhs) {
				const auto left = std::tuple{FigureTypeRank(lhs->type), Lowercase(lhs->name), lhs->id, lhs->variant};
				const auto right = std::tuple{FigureTypeRank(rhs->type), Lowercase(rhs->name), rhs->id, rhs->variant};
				return left < right;
			});
			break;
		default:
			std::sort(filtered.begin(), filtered.end(), byName);
			break;
		}
		return filtered;
	}

	void CycleForgeSort()
	{
		s_menu.forgeSortMode = WrapChoice(s_menu.forgeSortMode + 1, static_cast<int>(ForgeSortMode::Count));
		s_menu.selectedDefinition = 0;
		SetToast("CREATE SORT: " + std::string(ForgeSortName()), 1.5f);
		PulseHaptics();
	}

	void CycleForgeTypeFilter()
	{
		s_menu.forgeTypeFilter = WrapChoice(s_menu.forgeTypeFilter + 1, 5);
		s_menu.selectedDefinition = 0;
		SetToast("CREATE TYPE: " + std::string(ForgeTypeFilterName()), 1.5f);
		PulseHaptics();
	}

	void CycleForgeElementFilter()
	{
		s_menu.forgeElementFilter = WrapChoice(s_menu.forgeElementFilter + 1, 12);
		s_menu.selectedDefinition = 0;
		SetToast("CREATE ELEMENT: " + std::string(ElementChoiceName(s_menu.forgeElementFilter, "ALL")), 1.5f);
		PulseHaptics();
	}

	void ResetForgeFilters()
	{
		s_menu.forgeSearchText.fill(0);
		s_menu.forgeTypeFilter = 0;
		s_menu.forgeElementFilter = 0;
		s_menu.selectedDefinition = 0;
		SetToast("CREATE FILTERS CLEARED", 1.5f);
		PulseHaptics();
	}

	std::array<char, 96>& ActiveSearchText()
	{
		return s_menu.searchTarget == SearchTarget::Create ? s_menu.forgeSearchText : s_menu.searchText;
	}

	const std::array<std::string_view, 4>& KeyboardCharacterRows()
	{
		static constexpr std::array<std::string_view, 4> rows{
			"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", "1234567890"};
		return rows;
	}

	int KeyboardColumnCount(int row)
	{
		return row == 4 ? 4 : static_cast<int>(KeyboardCharacterRows()[std::clamp(row, 0, 3)].size());
	}

	void SearchTextChanged()
	{
		if (s_menu.searchTarget == SearchTarget::Collection)
			RebuildLibrary();
		else
			s_menu.selectedDefinition = 0;
	}

	void OpenSearchKeyboard(SearchTarget target)
	{
		s_menu.searchTarget = target;
		s_menu.keyboardRow = 0;
		s_menu.keyboardColumn = 0;
		PulseHaptics(UiSound::Confirm);
	}

	void CloseSearchKeyboard()
	{
		s_menu.searchTarget = SearchTarget::None;
		PulseHaptics(UiSound::Back);
	}

	void BackspaceSearchText()
	{
		auto& text = ActiveSearchText();
		const size_t length = std::char_traits<char>::length(text.data());
		if (length == 0)
			return;
		text[length - 1] = '\0';
		SearchTextChanged();
		PulseHaptics();
	}

	void AppendSearchCharacter(char character)
	{
		auto& text = ActiveSearchText();
		const size_t length = std::char_traits<char>::length(text.data());
		if (length + 1 >= text.size())
			return;
		text[length] = character;
		text[length + 1] = '\0';
		SearchTextChanged();
		PulseHaptics();
	}

	void ActivateKeyboardKey()
	{
		if (s_menu.keyboardRow < 4)
		{
			const auto row = KeyboardCharacterRows()[s_menu.keyboardRow];
			AppendSearchCharacter(row[std::clamp(s_menu.keyboardColumn, 0, static_cast<int>(row.size()) - 1)]);
			return;
		}
		switch (s_menu.keyboardColumn)
		{
		case 0: AppendSearchCharacter(' '); break;
		case 1: BackspaceSearchText(); break;
		case 2:
			ActiveSearchText().fill(0);
			SearchTextChanged();
			PulseHaptics();
			break;
		default: CloseSearchKeyboard(); break;
		}
	}

	void MoveKeyboardHorizontal(int direction)
	{
		const int count = KeyboardColumnCount(s_menu.keyboardRow);
		s_menu.keyboardColumn = WrapChoice(s_menu.keyboardColumn + direction, count);
		PulseHaptics();
	}

	void MoveKeyboardVertical(int direction)
	{
		const int oldCount = KeyboardColumnCount(s_menu.keyboardRow);
		const float position = oldCount > 1 ? static_cast<float>(s_menu.keyboardColumn) / (oldCount - 1) : 0.0f;
		s_menu.keyboardRow = WrapChoice(s_menu.keyboardRow + direction, 5);
		const int newCount = KeyboardColumnCount(s_menu.keyboardRow);
		s_menu.keyboardColumn = static_cast<int>(std::round(position * (newCount - 1)));
		PulseHaptics();
	}

	int TargetPortalRow()
	{
		const auto rows = BuildPortalRows();
		if (s_menu.focus == FocusArea::Portal && s_menu.selectedPortalRow >= 0 &&
			s_menu.selectedPortalRow < static_cast<int>(rows.size()))
			return s_menu.selectedPortalRow;
		const auto empty = std::find_if(rows.begin(), rows.end(), [](const PortalRow& row) { return row.actualSlot < 0; });
		return empty == rows.end() ? -1 : static_cast<int>(std::distance(rows.begin(), empty));
	}

	int FindLoadedSlot(uint16 id, uint16 variant)
	{
		const auto loaded = nsyshid::g_skyportal.GetLoadedSkylanders();
		for (int slot = 0; slot < static_cast<int>(loaded.size()); ++slot)
			if (loaded[slot] && loaded[slot]->first == id && loaded[slot]->second == variant)
				return slot;
		return -1;
	}

	void BeginPlacement(const skylander_ui::CollectionFigure& figure)
	{
		if (!GetConfig().emulated_usb_devices.emulate_skylander_portal)
		{
			SetToast("SELECT VIRTUAL PORTAL BEFORE PLACING A CARD", 2.5f);
			PulseHaptics(UiSound::Back);
			return;
		}
		const int existingSlot = FindLoadedSlot(figure.id, figure.variant);
		if (existingSlot >= 0)
		{
			const auto existingRows = BuildPortalRows();
			const auto existingRow = std::find_if(existingRows.begin(), existingRows.end(),
				[&](const PortalRow& row) { return row.actualSlot == existingSlot; });
			if (existingRow != existingRows.end())
				s_menu.selectedPortalRow = static_cast<int>(std::distance(existingRows.begin(), existingRow));
			SetToast(Uppercase(figure.name) + " IS ALREADY ON THE PORTAL", 2.2f);
			PulseHaptics(UiSound::Back);
			return;
		}
		const int targetRow = TargetPortalRow();
		const auto rows = BuildPortalRows();
		if (targetRow < 0 || targetRow >= static_cast<int>(rows.size()))
		{
			SetToast("PORTAL FULL - SELECT A CARD TO REPLACE", 2.5f);
			PulseHaptics(UiSound::Back);
			return;
		}
		s_menu.selectedPortalRow = targetRow;
		const uint8 slot = PlaceFigure(figure, rows[targetRow].actualSlot);
		if (slot == 0xFF)
		{
			SetToast("THE PORTAL COULD NOT LOAD THAT FILE", 2.5f);
			PulseHaptics(UiSound::Back);
			return;
		}
		s_menu.placementSlot = slot;
		const auto updatedRows = BuildPortalRows();
		const auto placed = std::find_if(updatedRows.begin(), updatedRows.end(),
			[&](const PortalRow& row) { return row.actualSlot == slot; });
		s_menu.placementTargetRow = placed == updatedRows.end() ? targetRow :
			static_cast<int>(std::distance(updatedRows.begin(), placed));
		s_menu.placementAnimation = 0.0f;
		s_menu.placementImpact = 0.0f;
		s_menu.placementArtwork = figure.imagePath;
		s_menu.placementAccent = ElementColor(figure.element, GetArtworkTexture(figure.imagePath).accent);
		s_menu.selectionFlash = 1.0f;
		SetToast(Uppercase(figure.name) + " ENTERED THE PORTAL", 2.4f);
		PulseHaptics(UiSound::Confirm);
	}

	void RemoveActualSlot(int slot)
	{
		if (slot < 0)
		{
			SetToast("THAT PORTAL SLOT IS EMPTY");
			PulseHaptics(UiSound::Back);
			return;
		}
		const auto loaded = nsyshid::g_skyportal.GetLoadedSkylanders();
		std::string name = "FIGURE";
		if (loaded[slot])
			if (const auto* definition = s_menu.catalog.Find(loaded[slot]->first, loaded[slot]->second))
				name = Uppercase(definition->name);
		const bool removed = nsyshid::g_skyportal.RemoveSkylander(static_cast<uint8>(slot));
		if (removed)
			SetToast(name + " REMOVED");
		PulseHaptics(removed ? UiSound::Confirm : UiSound::Back);
	}

	void RemoveFocusedFigure()
	{
		if (!GetConfig().emulated_usb_devices.emulate_skylander_portal)
		{
			SetToast("FIGURES ON THE PHYSICAL PORTAL ARE MANAGED IN HARDWARE", 2.8f);
			PulseHaptics(UiSound::Back);
			return;
		}
		const auto rows = BuildPortalRows();
		if (s_menu.focus == FocusArea::Portal)
		{
			if (s_menu.selectedPortalRow >= 0 && s_menu.selectedPortalRow < static_cast<int>(rows.size()))
				RemoveActualSlot(rows[s_menu.selectedPortalRow].actualSlot);
			return;
		}
		if (!s_menu.library.empty())
		{
			const auto& figure = s_menu.library[s_menu.selectedLibrary];
			RemoveActualSlot(FindLoadedSlot(figure.id, figure.variant));
		}
	}

	void ForgeSelectedFigure()
	{
		if (GetConfig().emulated_usb_devices.skylander_collection_path.GetValue().empty())
		{
			SetToast("CHOOSE A COLLECTION FOLDER IN GENERAL SETTINGS", 3.0f);
			PulseHaptics(UiSound::Back);
			return;
		}
		const auto definitions = BuildForgeDefinitions();
		if (definitions.empty())
			return;
		s_menu.selectedDefinition = std::clamp(s_menu.selectedDefinition, 0,
			static_cast<int>(definitions.size()) - 1);
		const auto& figure = *definitions[s_menu.selectedDefinition];
		const bool created = CreateFigure(figure);
		if (created)
		{
			SetToast(Uppercase(figure.name) + " WAS CREATED", 2.5f);
			s_menu.reloadCatalog = true;
			s_menu.page = RiftPage::Dashboard;
			s_menu.focus = FocusArea::Library;
		}
		else
			SetToast("THE .SKY FILE COULD NOT BE CREATED", 2.5f);
		PulseHaptics(created ? UiSound::Confirm : UiSound::Back);
	}

	void OpenLibraryDetails()
	{
		if (s_menu.library.empty())
			return;
		const auto& figure = s_menu.library[s_menu.selectedLibrary];
		s_menu.details = {
			true, s_menu.selectedLibrary, FindLoadedSlot(figure.id, figure.variant),
			figure.id, figure.variant, figure.name, figure.imagePath, figure.filePath};
		s_menu.page = RiftPage::Details;
		PulseHaptics(UiSound::Confirm);
	}

	void OpenPortalDetails()
	{
		if (!GetConfig().emulated_usb_devices.emulate_skylander_portal)
		{
			SetToast("PHYSICAL FIGURE DETAILS ARE NOT AVAILABLE YET", 2.6f);
			PulseHaptics(UiSound::Back);
			return;
		}
		const auto rows = BuildPortalRows();
		if (s_menu.selectedPortalRow < 0 || s_menu.selectedPortalRow >= static_cast<int>(rows.size()))
			return;
		const auto& row = rows[s_menu.selectedPortalRow];
		if (row.actualSlot < 0)
		{
			SetToast("CHOOSE A LIBRARY CARD TO FILL THIS SLOT", 2.2f);
			PulseHaptics(UiSound::Back);
			return;
		}
		if (row.actualSlot < 0)
			return;
		s_menu.details = {
			false, -1, row.actualSlot, row.id, row.variant,
			row.definition ? row.definition->name : "Unknown figure",
			row.definition ? row.definition->imagePath : fs::path{}, {}};
		s_menu.page = RiftPage::Details;
		PulseHaptics(UiSound::Confirm);
	}

	void RequestDeleteDetailFigure()
	{
		if (!s_menu.details.fromLibrary || s_menu.details.filePath.empty())
		{
			SetToast("ONLY LOCAL COLLECTION FILES CAN BE DELETED", 2.8f);
			PulseHaptics(UiSound::Back);
			return;
		}
		s_menu.page = RiftPage::DeleteConfirm;
		PulseHaptics(UiSound::Back);
	}

	void DeleteDetailFigure()
	{
		const std::string configured = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
		if (configured.empty() || s_menu.details.filePath.empty())
		{
			SetToast("THE COLLECTION FILE COULD NOT BE LOCATED", 2.8f);
			s_menu.page = RiftPage::Details;
			return;
		}

		std::error_code error;
		const fs::path collectionRoot = fs::weakly_canonical(_utf8ToPath(configured), error);
		if (error)
		{
			SetToast("THE COLLECTION FOLDER COULD NOT BE VERIFIED", 2.8f);
			s_menu.page = RiftPage::Details;
			return;
		}
		const fs::path source = fs::weakly_canonical(s_menu.details.filePath, error);
		const fs::path relative = error ? fs::path{} : fs::relative(source, collectionRoot, error);
		bool insideCollection = !error && !relative.empty() && !relative.is_absolute();
		for (const auto& part : relative)
			insideCollection = insideCollection && part != "..";
		const std::string extension = Lowercase(_pathToUtf8(source.extension()));
		if (!insideCollection || extension != ".sky" || !fs::is_regular_file(source, error) || error)
		{
			SetToast("RIFT REFUSED TO DELETE A FILE OUTSIDE THE COLLECTION", 3.2f);
			s_menu.page = RiftPage::Details;
			PulseHaptics(UiSound::Back);
			return;
		}

		const fs::path trashFolder = collectionRoot / ".rift-trash";
		fs::create_directories(trashFolder, error);
		if (error)
		{
			SetToast("THE RIFT TRASH FOLDER COULD NOT BE CREATED", 2.8f);
			s_menu.page = RiftPage::Details;
			return;
		}
		const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		const fs::path destination = trashFolder /
			_utf8ToPath(fmt::format("{}-{}{}", _pathToUtf8(source.stem()), stamp, _pathToUtf8(source.extension())));
		if (s_menu.details.actualSlot >= 0)
			RemoveActualSlot(s_menu.details.actualSlot);
		fs::rename(source, destination, error);
		if (error)
		{
			SetToast("THE .SKY FILE COULD NOT BE MOVED TO RIFT TRASH", 3.0f);
			s_menu.page = RiftPage::Details;
			PulseHaptics(UiSound::Back);
			return;
		}
		s_menu.favorites.erase(FavoriteKey(source));
		SaveFavorites();
		s_menu.details = {};
		s_menu.selectedLibrary = 0;
		s_menu.reloadCatalog = true;
		s_menu.page = RiftPage::Dashboard;
		s_menu.focus = FocusArea::Library;
		SetToast(".SKY FILE MOVED TO COLLECTION / .RIFT-TRASH", 3.2f);
		PulseHaptics(UiSound::Confirm);
	}

	void DrawBackdrop(ImDrawList* draw, const RiftLayout& layout, ImVec2 display)
	{
		const auto theme = CurrentTheme();
		const ImVec2 shellMinimum = Point(layout, kDrawerX, 0);
		const ImVec2 shellMaximum = Point(layout, 1280, 720);
		for (int layer = 24; layer >= 1; --layer)
		{
			const float fraction = static_cast<float>(layer) / 24.0f;
			const float width = fraction * 112.0f * layout.scale;
			draw->AddRectFilled({shellMinimum.x - width, 0}, {shellMinimum.x, display.y},
				WithAlpha(theme.background, layout.alpha * 0.045f * (1.0f - fraction)));
		}
		const float opacity = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_drawer_opacity.GetValue(), 0, 100) / 100.0f;
		draw->AddRectFilled(shellMinimum, shellMaximum,
			WithAlpha(theme.background, layout.alpha * (0.48f + opacity * 0.42f)));
		const int particleLevel = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_particle_level.GetValue(), 0, 3);
		const int moteCount = particleLevel == 0 ? 0 : particleLevel == 1 ? 12 : particleLevel == 2 ? 22 : 34;
		for (int mote = 0; mote < moteCount; ++mote)
		{
			const float drift = MotionEnabled() ? std::sin(static_cast<float>(ImGui::GetTime()) * (0.22f + (mote % 5) * 0.03f) + mote) * 8.0f : 0.0f;
			const float x = 332.0f + static_cast<float>((mote * 149) % 910) + drift;
			const float y = 18.0f + static_cast<float>((mote * 83 + mote * mote * 7) % 676);
			const ImU32 moteColor = mote % 3 == 0 ? theme.secondary : mote % 3 == 1 ? theme.accent : Lighten(theme.accent, 0.38f);
			draw->AddCircleFilled(Point(layout, x, y), (mote % 3 == 0 ? 1.2f : 0.75f) * layout.scale,
				WithAlpha(moteColor, layout.alpha * (mote % 4 == 0 ? 0.17f : 0.09f)), 8);
		}
		draw->AddLine(Point(layout, kDrawerX + 8.0f, 1.0f), Point(layout, 1272.0f, 1.0f),
			WithAlpha(IM_COL32(189, 227, 248, 255), layout.alpha * 0.12f), layout.scale);
		draw->AddLine({shellMinimum.x, shellMinimum.y + 2.0f * layout.scale},
			{shellMinimum.x, shellMaximum.y - 2.0f * layout.scale},
			WithAlpha(theme.accent, layout.alpha * 0.34f), 1.2f * layout.scale);
	}

	bool ClickedRect(const RiftLayout& layout, ImVec2 minimum, ImVec2 maximum)
	{
		return layout.alpha > 0.9f && ImGui::IsMouseClicked(0) &&
			ImGui::IsMouseHoveringRect(minimum, maximum);
	}

	void ActivateHeaderSelection()
	{
		switch (s_menu.headerSelection)
		{
		case 0: SetPortalMode(true); PulseHaptics(UiSound::Confirm); break;
		case 1: SetPortalMode(false); PulseHaptics(UiSound::Confirm); break;
		case 2:
			s_menu.page = RiftPage::Forge;
			s_menu.forgeToolbarFocused = false;
			PulseHaptics(UiSound::Confirm);
			break;
		case 3:
			s_menu.page = RiftPage::Options;
			s_menu.selectedOption = 0;
			PulseHaptics(UiSound::Confirm);
			break;
		}
	}

	void DrawHeaderTab(ImDrawList* draw, const RiftLayout& layout, int index, float x, float width,
		std::string_view label, bool active, bool disabled = false)
	{
		const ImVec2 minimum = Point(layout, x, 77);
		const ImVec2 maximum = Point(layout, x + width, 113);
		const bool focused = s_menu.focus == FocusArea::Header && s_menu.headerSelection == index;
		float& focusAnimation = s_menu.headerFocusAnimations[index];
		focusAnimation = AnimateFocus(focusAnimation, focused ? 1.0f : active ? 0.32f : 0.0f);
		const ImU32 accent = index == 1 ? IM_COL32(91, 231, 151, 255) :
			(index == 2 ? CurrentTheme().secondary : IM_COL32(76, 218, 255, 255));
		if (focusAnimation > 0.01f)
			draw->AddRectFilled({minimum.x + 3.0f * layout.scale, minimum.y + 4.0f * layout.scale},
				{maximum.x + 3.0f * layout.scale, maximum.y + 5.0f * layout.scale},
				WithAlpha(IM_COL32(0, 3, 12, 255), layout.alpha * focusAnimation * 0.22f), 2.0f * layout.scale);
		if (focusAnimation > 0.01f)
			draw->AddRectFilled(minimum, maximum,
				WithAlpha(index == 1 ? IM_COL32(17, 55, 43, 255) :
					(index == 2 ? IM_COL32(45, 30, 69, 255) :
						(focused ? CurrentTheme().selectedSurface : IM_COL32(15, 38, 57, 255))),
					layout.alpha * focusAnimation * (disabled ? 0.22f : 0.72f)), 2.0f * layout.scale);
		if (active || focused)
			draw->AddRectFilled(Point(layout, x, 109), Point(layout, x + width, 112),
				WithAlpha(accent, layout.alpha * std::max(focusAnimation, active ? 0.72f : 0.0f)),
				1.0f * layout.scale);
		DrawTextCentered(draw, Point(layout, x + width * 0.5f, 95),
			WithAlpha(disabled ? IM_COL32(117, 140, 159, 255) :
				(active ? Lighten(accent, 0.22f) : IM_COL32(235, 244, 251, 255)), layout.alpha),
			13.0f * layout.scale, label);
		if (ClickedRect(layout, minimum, maximum))
		{
			s_menu.focus = FocusArea::Header;
			s_menu.headerSelection = index;
			ActivateHeaderSelection();
		}
	}

	void DrawHeader(ImDrawList* draw, const RiftLayout& layout)
	{
		const bool virtualPortal = GetConfig().emulated_usb_devices.emulate_skylander_portal;
		const ImU32 portalColor = virtualPortal ? IM_COL32(67, 214, 255, 255) : IM_COL32(91, 231, 151, 255);
		const ImU32 wordmarkColor = Lighten(CurrentTheme().accent, 0.58f);
		DrawText(draw, Point(layout, 326, 34), WithAlpha(IM_COL32(0, 2, 12, 255), layout.alpha * 0.28f),
			29.0f * layout.scale, "RIFT");
		DrawText(draw, Point(layout, 324, 32), WithAlpha(IM_COL32(0, 2, 12, 255), layout.alpha * 0.74f),
			29.0f * layout.scale, "RIFT");
		DrawText(draw, Point(layout, 321, 29), WithAlpha(wordmarkColor, layout.alpha),
			29.0f * layout.scale, "RIFT");
		draw->AddRectFilled(Point(layout, 321, 63), Point(layout, 375, 65),
			WithAlpha(wordmarkColor, layout.alpha * 0.88f), 1.0f * layout.scale);
		DrawShadowedTextRightAligned(draw, Point(layout, 1244, 31), WithAlpha(portalColor, layout.alpha),
			11.0f * layout.scale, virtualPortal ? "VIRTUAL PORTAL" : "PHYSICAL PORTAL", layout.scale);
		DrawShadowedTextRightAligned(draw, Point(layout, 1244, 49), WithAlpha(CurrentTheme().text, layout.alpha),
			13.0f * layout.scale, virtualPortal ? "LOCAL COLLECTION" : "USB PASSTHROUGH", layout.scale);

		draw->AddRect(Point(layout, 790, 77), Point(layout, 1244, 113),
			WithAlpha(IM_COL32(147, 190, 217, 255), layout.alpha * 0.26f), 4.0f * layout.scale);
		DrawHeaderTab(draw, layout, 0, 790, 116, "VIRTUAL", virtualPortal);
		DrawHeaderTab(draw, layout, 1, 908, 116, "PHYSICAL", !virtualPortal);
		DrawHeaderTab(draw, layout, 2, 1026, 126, "CREATE .SKY", s_menu.page == RiftPage::Forge);
		DrawHeaderTab(draw, layout, 3, 1154, 90, "OPTIONS", s_menu.page == RiftPage::Options);
		draw->AddLine(Point(layout, 318, 126), Point(layout, 1258, 126),
			WithAlpha(IM_COL32(165, 218, 246, 255), layout.alpha * 0.26f), 1.0f * layout.scale);
	}

	void DrawPortalColumn(ImDrawList* draw, const RiftLayout& layout)
	{
		const bool virtualPortal = GetConfig().emulated_usb_devices.emulate_skylander_portal;
		DrawText(draw, Point(layout, 334, 158), WithAlpha(CurrentTheme().text, layout.alpha),
			18.0f * layout.scale, "CURRENT PORTAL");
		DrawText(draw, Point(layout, 1094, 163), WithAlpha(IM_COL32(139, 174, 199, 255), layout.alpha),
			11.0f * layout.scale, virtualPortal ? "VIRTUAL LOADOUT" : "HARDWARE CONTROL");

		const auto rows = virtualPortal ? BuildPortalRows() : std::vector<PortalRow>(1);
		s_menu.selectedPortalRow = std::clamp(s_menu.selectedPortalRow, 0,
			std::max(0, static_cast<int>(rows.size()) - 1));
		const int visibleCount = std::min(kVisiblePortalSlots, static_cast<int>(rows.size()));
		const int firstVisible = std::clamp(s_menu.selectedPortalRow - visibleCount / 2, 0,
			std::max(0, static_cast<int>(rows.size()) - visibleCount));
		const float gap = 7.0f;
		const float targetWidth = (910.0f - gap * (visibleCount - 1)) / std::max(1, visibleCount);
		for (int visibleIndex = 0; visibleIndex < visibleCount; ++visibleIndex)
		{
			const int rowIndex = firstVisible + visibleIndex;
			const int layoutKey = rows[rowIndex].actualSlot < 0 ? kPortalCapacity : rows[rowIndex].actualSlot;
			float& cardX = s_menu.portalCardX[layoutKey];
			float& cardWidth = s_menu.portalCardWidth[layoutKey];
			const float targetX = 334.0f + visibleIndex * (targetWidth + gap);
			if (cardWidth == 0.0f || !MotionEnabled())
			{
				cardX = targetX;
				cardWidth = targetWidth;
			}
			else
			{
				cardX = SmoothTowards(cardX, targetX, 10.0f);
				cardWidth = SmoothTowards(cardWidth, targetWidth, 10.0f);
			}
			const float x = cardX;
			const float width = cardWidth;
			const ImVec2 minimum = Point(layout, x, 190);
			const ImVec2 maximum = Point(layout, x + width, 282);
			const bool focused = s_menu.focus == FocusArea::Portal && s_menu.selectedPortalRow == rowIndex;
			float& focusAnimation = s_menu.portalFocusAnimations[rowIndex];
			focusAnimation = AnimateFocus(focusAnimation, focused ? 1.0f : 0.0f);
			ImU32 accent = IM_COL32(79, 210, 255, 255);
			ArtworkTexture rowTexture;
			if (rows[rowIndex].definition)
			{
				rowTexture = GetArtworkTexture(rows[rowIndex].definition->imagePath);
				rowTexture.accent = ElementColor(rows[rowIndex].definition->element, rowTexture.accent);
				accent = rowTexture.accent;
			}
			const float shadowX = 3.0f + focusAnimation * 2.0f;
			const float shadowY = 4.0f + focusAnimation * 3.0f;
			draw->AddRectFilled({minimum.x + shadowX * layout.scale, minimum.y + shadowY * layout.scale},
				{maximum.x + shadowX * layout.scale, maximum.y + shadowY * layout.scale},
				WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * (0.16f + focusAnimation * 0.14f)),
				CornerRadius(4.0f) * layout.scale);
			draw->AddRectFilled(minimum, maximum,
					WithAlpha(BlendColor(CurrentTheme().surface, CurrentTheme().selectedSurface, focusAnimation),
					layout.alpha * (0.76f + focusAnimation * 0.10f)),
					CornerRadius(4.0f) * layout.scale);
			draw->AddRect(minimum, maximum,
				WithAlpha(BlendColor(CurrentTheme().border, accent, focusAnimation),
					layout.alpha * (0.24f + focusAnimation * 0.62f)),
				CornerRadius(4.0f) * layout.scale, 0, (1.0f + focusAnimation) * layout.scale);
			std::string slotLabel = "ADD CARD";
			if (rows[rowIndex].definition)
			{
				using skylander_ui::FigureType;
				switch (rows[rowIndex].definition->type)
				{
				case FigureType::Vehicle: slotLabel = "VEHICLE"; break;
				case FigureType::Trap: slotLabel = "TRAP"; break;
				case FigureType::CreationCrystal: slotLabel = "IMAGINATOR"; break;
				case FigureType::Item: slotLabel = "ITEM"; break;
				default: slotLabel = "SKYLANDER"; break;
				}
			}
			DrawTextFit(draw, Point(layout, x + 9, 199), WithAlpha(CurrentTheme().muted, layout.alpha),
				9.5f * layout.scale, DrawerWidth(layout, width - 17.0f), slotLabel);
			s_menu.portalTargets[rowIndex] = Point(layout, x + 31, 244);

			if (rows[rowIndex].actualSlot >= 0)
			{
				const bool hiddenDuringPlacement =
					s_menu.placementAnimation < 1.0f && rows[rowIndex].actualSlot == s_menu.placementSlot;
				if (!hiddenDuringPlacement)
					DrawCard(draw, rowTexture,
						s_menu.portalTargets[rowIndex], (67.0f + focusAnimation * 3.0f) * layout.scale,
						0.0f, focusAnimation, layout.alpha);
				const std::string name = rows[rowIndex].definition ? rows[rowIndex].definition->name :
					fmt::format("Unknown ({}, {})", rows[rowIndex].id, rows[rowIndex].variant);
				DrawTextFit(draw, Point(layout, x + 58, 226), WithAlpha(CurrentTheme().text, layout.alpha),
						11.5f * layout.scale, DrawerWidth(layout, std::max(42.0f, width - 64.0f)), Uppercase(name));
				DrawText(draw, Point(layout, x + 58, 254), WithAlpha(accent, layout.alpha),
					10.5f * layout.scale, fmt::format("SLOT {:02}", rows[rowIndex].actualSlot + 1));
			}
			else
			{
				DrawTextFit(draw, Point(layout, x + 10, 237), WithAlpha(CurrentTheme().muted, layout.alpha * 0.72f),
					12.0f * layout.scale, DrawerWidth(layout, std::max(42.0f, width - 20.0f)),
					virtualPortal ? (width > 260 ? "CHOOSE A CARD BELOW TO ADD IT" : "ADD CARD") : "PLACE A FIGURE ON YOUR PORTAL");
			}
			if (ClickedRect(layout, minimum, maximum))
			{
				if (s_menu.focus != FocusArea::Portal || s_menu.selectedPortalRow != rowIndex)
				{
					s_menu.focus = FocusArea::Portal;
					s_menu.selectedPortalRow = rowIndex;
					PulseHaptics();
				}
			}
		}
		if (rows.size() > kVisiblePortalSlots)
			DrawText(draw, Point(layout, 1120, 164), WithAlpha(CurrentTheme().accent, layout.alpha),
				10.0f * layout.scale, fmt::format("{}-{} OF {}", firstVisible + 1,
					firstVisible + visibleCount, rows.size()));
		draw->AddLine(Point(layout, 334, 293), Point(layout, 1244, 293),
			WithAlpha(IM_COL32(151, 202, 231, 255), layout.alpha * 0.20f), layout.scale);
	}

	void DrawSearchIcon(ImDrawList* draw, const RiftLayout& layout, ImVec2 center)
	{
		draw->AddCircle(center, 6.0f * layout.scale, WithAlpha(IM_COL32(169, 198, 220, 255), layout.alpha),
			24, 1.6f * layout.scale);
		draw->AddLine({center.x + 4.5f * layout.scale, center.y + 4.5f * layout.scale},
			{center.x + 10.0f * layout.scale, center.y + 10.0f * layout.scale},
			WithAlpha(IM_COL32(169, 198, 220, 255), layout.alpha), 1.6f * layout.scale);
	}

	ArtworkTexture GetControllerGlyph(std::string_view glyph);
	void OpenLibraryOrder();

	void DrawInlineGlyph(ImDrawList* draw, const RiftLayout& layout, ImVec2 center,
		std::string_view glyph, float size = 24.0f)
	{
		const auto texture = GetControllerGlyph(glyph);
		if (!texture.id)
			return;
		const float half = size * 0.5f * layout.scale;
		draw->AddImage(texture.id, {center.x - half, center.y - half}, {center.x + half, center.y + half},
			texture.uvMinimum, texture.uvMaximum, WithAlpha(IM_COL32_WHITE, layout.alpha));
	}

	void DrawLibraryToolbar(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawText(draw, Point(layout, 334, 306), WithAlpha(CurrentTheme().text, layout.alpha),
			18.0f * layout.scale, "LIBRARY");
		if (!s_menu.library.empty())
			DrawTextFit(draw, Point(layout, 425, 309), WithAlpha(IM_COL32(126, 208, 238, 255), layout.alpha),
				12.0f * layout.scale, DrawerWidth(layout, 610.0f),
				Uppercase(ShortName(s_menu.library[s_menu.selectedLibrary].name, 44)));
		DrawText(draw, Point(layout, 1202, 310), WithAlpha(IM_COL32(142, 175, 199, 255), layout.alpha),
			11.0f * layout.scale, fmt::format("{:02}", s_menu.library.size()));

		const ImVec2 toolbarMin = Point(layout, 334, 330);
		const ImVec2 toolbarMax = Point(layout, 1244, 369);
		draw->AddRectFilled(toolbarMin, toolbarMax,
			WithAlpha(IM_COL32(5, 15, 30, 220), layout.alpha * 0.84f), 5.0f * layout.scale);
		draw->AddRect(toolbarMin, toolbarMax,
			WithAlpha(CurrentTheme().border, layout.alpha * 0.28f), 5.0f * layout.scale);
		draw->AddLine(Point(layout, 650, 335), Point(layout, 650, 364),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.22f), layout.scale);
		draw->AddLine(Point(layout, 930, 335), Point(layout, 930, 364),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.22f), layout.scale);
		const ImVec2 searchMin = Point(layout, 334, 330);
		const ImVec2 searchMax = Point(layout, 650, 369);
		if (s_menu.focus == FocusArea::Search)
			DrawElevatedSurface(draw, layout, searchMin, searchMax, true);
		DrawSearchIcon(draw, layout, Point(layout, 355, 348));
		ImGui::SetCursorScreenPos(Point(layout, 375, 334));
		ImGui::SetNextItemWidth(DrawerWidth(layout, 260.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 7.0f * layout.scale});
		ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(CurrentTheme().text, layout.alpha));
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, WithAlpha(IM_COL32(132, 165, 188, 255), layout.alpha));
		ImFont* searchFont = ImGui_GetFont(14.0f * layout.scale);
		if (searchFont)
			ImGui::PushFont(searchFont);
		if (ImGui::InputTextWithHint("##rift_search", "Search collection...", s_menu.searchText.data(),
			s_menu.searchText.size()))
			RebuildLibrary();
		if (searchFont)
			ImGui::PopFont();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();

		const ImVec2 sortMin = Point(layout, 650, 330);
		const ImVec2 sortMax = Point(layout, 930, 369);
		DrawInlineGlyph(draw, layout, Point(layout, 675, 349), "LB", 24.0f);
		DrawTextFit(draw, Point(layout, 698, 338), WithAlpha(CurrentTheme().text, layout.alpha),
			12.0f * layout.scale, DrawerWidth(layout, 216.0f), SortModeName());
		if (ClickedRect(layout, sortMin, sortMax))
			OpenLibraryOrder();

		const ImVec2 favoriteMin = Point(layout, 930, 330);
		const ImVec2 favoriteMax = Point(layout, 1244, 369);
		const bool favorite = !s_menu.library.empty() && IsFavorite(s_menu.library[s_menu.selectedLibrary].filePath);
		if (favorite)
			draw->AddRectFilled(favoriteMin, favoriteMax,
				WithAlpha(IM_COL32(91, 69, 18, 220), layout.alpha * 0.52f), 0.0f);
		DrawInlineGlyph(draw, layout, Point(layout, 955, 349), "RB", 24.0f);
		if (favorite)
			DrawStar(draw, Point(layout, 988, 349), 7.0f * layout.scale,
				WithAlpha(IM_COL32(255, 210, 91, 255), layout.alpha));
		DrawText(draw, Point(layout, favorite ? 1003 : 980, 338),
			WithAlpha(CurrentTheme().text, layout.alpha),
			12.0f * layout.scale, favorite ? "FAVORITED" : "ADD FAVORITE");
		if (ClickedRect(layout, favoriteMin, favoriteMax))
			ToggleFavorite();
	}

	void UpdateSelectedAnimation(int oldSelection)
	{
		if (s_menu.selectedLibrary == oldSelection)
			return;
		s_menu.selectionFlash = 1.0f;
	}

	void ApplySelectedElementFilter()
	{
		const int choice = std::clamp(s_menu.selectedElementFilter, 0, 11);
		auto& config = GetConfig().emulated_usb_devices;
		if (config.skylander_element_filter.GetValue() != choice)
		{
			config.skylander_element_filter = choice;
			g_config.Save();
			RebuildLibrary();
		}
		SetToast("FILTER: " + std::string(ElementChoiceName(choice, "ALL ELEMENTS")), 1.5f);
		PulseHaptics(UiSound::Confirm);
	}

	int WrappedLibraryOffset(int index, int selected, int count)
	{
		int offset = index - selected;
		const int half = count / 2;
		if (offset > half)
			offset -= count;
		else if (offset < -half)
			offset += count;
		return offset;
	}

	void DrawLibraryGrid(ImDrawList* draw, const RiftLayout& layout)
	{
		const int activeElement = GetConfig().emulated_usb_devices.skylander_element_filter.GetValue();
		constexpr float filterStartX = 334.0f;
		constexpr float filterEndX = 1244.0f;
		constexpr float filterGap = 5.5f;
		constexpr float filterWidth = (filterEndX - filterStartX - filterGap * 11.0f) / 12.0f;
		for (int choice = 0; choice < 12; ++choice)
		{
			const float x = filterStartX + choice * (filterWidth + filterGap);
			const ImVec2 lo = Point(layout, x, 373);
			const ImVec2 hi = Point(layout, x + filterWidth, 392);
			const bool active = choice == activeElement;
			const bool focused = s_menu.focus == FocusArea::ElementFilter &&
				s_menu.selectedElementFilter == choice;
			float& filterFocus = s_menu.elementFilterFocus[choice];
			filterFocus = AnimateFocus(filterFocus, focused ? 1.0f : active ? 0.48f : 0.0f);
			const ImU32 color = choice == 0 ? CurrentTheme().accent : ElementColor(ElementForChoice(choice), CurrentTheme().accent);
			draw->AddRectFilled(lo, hi, WithAlpha(BlendColor(CurrentTheme().surface,
				CurrentTheme().selectedSurface, filterFocus), layout.alpha * 0.78f), 4.0f * layout.scale);
			draw->AddRect(lo, hi, WithAlpha(BlendColor(CurrentTheme().border, color, filterFocus),
				layout.alpha * (0.24f + filterFocus * 0.68f)), 4.0f * layout.scale, 0,
				(1.0f + filterFocus) * layout.scale);
			draw->AddLine({lo.x + DrawerWidth(layout, 5.0f), hi.y}, {hi.x - DrawerWidth(layout, 5.0f), hi.y},
				WithAlpha(color, layout.alpha * (0.25f + 0.70f * filterFocus)), (1.0f + filterFocus) * layout.scale);
			DrawTextCentered(draw, {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f},
				WithAlpha(BlendColor(CurrentTheme().muted, color, filterFocus), layout.alpha),
				9.5f * layout.scale, ElementChoiceName(choice, "ALL"));
			if (ClickedRect(layout, lo, hi))
			{
				s_menu.focus = FocusArea::ElementFilter;
				s_menu.selectedElementFilter = choice;
				ApplySelectedElementFilter();
			}
		}
		if (s_menu.library.empty())
		{
			const std::string catalogError = s_menu.catalog.GetError();
			const std::string configuredFolder = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
			std::error_code folderError;
			const bool folderExists = !configuredFolder.empty() &&
				fs::is_directory(_utf8ToPath(configuredFolder), folderError) && !folderError;
			const char* heading = CurrentSortMode() == SortMode::FavoritesOnly ?
				"NO FAVORITES MATCH THIS FILTER" : "NO .SKY FILES MATCH";
			std::string guidance = "CHANGE THE FILTER OR COLLECTION FOLDER";
			if (!catalogError.empty())
			{
				heading = "RIFT ASSET LIBRARY COULD NOT LOAD";
				guidance = catalogError;
			}
			else if (configuredFolder.empty())
			{
				heading = "COLLECTION FOLDER IS NOT SET";
				guidance = "CHOOSE IT IN OPTIONS > GENERAL SETTINGS";
			}
			else if (!folderExists)
			{
				heading = "COLLECTION FOLDER COULD NOT BE FOUND";
				guidance = "CHOOSE AN EXISTING FOLDER IN GENERAL SETTINGS";
			}
			DrawTextCentered(draw, Point(layout, 790, 475),
				WithAlpha(IM_COL32(224, 237, 246, 255), layout.alpha),
				20.0f * layout.scale, heading);
			DrawTextCentered(draw, Point(layout, 790, 508),
				WithAlpha(IM_COL32(145, 177, 200, 255), layout.alpha),
				13.0f * layout.scale, Uppercase(guidance));
			return;
		}
		s_menu.selectedLibrary = std::clamp(s_menu.selectedLibrary, 0, static_cast<int>(s_menu.library.size()) - 1);


		struct CarouselEntry
		{
			int index{};
			float offset{};
			float focus{};
			ArtworkTexture texture;
		};
		std::array<CarouselEntry, 15> visible{};
		int visibleCount = 0;
		const int count = static_cast<int>(s_menu.library.size());
		const int requestedDensity = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_card_density.GetValue(), 5, 11);
		const int cardDensity = requestedDensity % 2 == 0 ? requestedDensity + 1 : requestedDensity;
		const int cascadeStyle = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_cascade_style.GetValue(), 0, 3);
		const int visibleRadius = cardDensity / 2;
		std::array<int, 15> candidateIndices{};
		int candidateCount = 0;
		for (int relative = -visibleRadius - 1; relative <= visibleRadius + 1; ++relative)
		{
			int index = (s_menu.selectedLibrary + relative) % count;
			if (index < 0)
				index += count;
			if (std::find(candidateIndices.begin(), candidateIndices.begin() + candidateCount, index) !=
				candidateIndices.begin() + candidateCount)
				continue;
			candidateIndices[candidateCount++] = index;
			const int targetOffset = WrappedLibraryOffset(index, s_menu.selectedLibrary, count);
			const std::string key = FavoriteKey(s_menu.library[index].filePath);
			auto existing = s_cardCarouselOffsets.find(key);
			if (existing == s_cardCarouselOffsets.end())
				existing = s_cardCarouselOffsets.emplace(key, static_cast<float>(targetOffset)).first;
			else if (std::abs(existing->second - targetOffset) > visibleRadius + 1.0f)
				existing->second = static_cast<float>(targetOffset);
			existing->second = MotionEnabled() ?
				SmoothTowards(existing->second, static_cast<float>(targetOffset), MotionLevel() == 2 ? 13.5f : 24.0f) :
				static_cast<float>(targetOffset);
			if (std::abs(targetOffset) > visibleRadius + 1 &&
				std::abs(existing->second) > visibleRadius + 1.1f)
				continue;
			const bool focused = s_menu.focus == FocusArea::Library && index == s_menu.selectedLibrary;
			float& focusAmount = s_cardFocusAmounts[key];
			focusAmount = AnimateFocus(focusAmount, focused ? 1.0f : 0.0f);
			auto texture = GetArtworkTexture(s_menu.library[index].imagePath);
			texture.accent = ElementColor(s_menu.library[index].element, texture.accent);
			visible[visibleCount++] = {index, existing->second, focusAmount, texture};
		}
		std::sort(visible.begin(), visible.begin() + visibleCount, [](const auto& left, const auto& right) {
			const float leftDistance = std::abs(left.offset);
			const float rightDistance = std::abs(right.offset);
			if (leftDistance != rightDistance)
				return leftDistance > rightDistance;
			return left.offset < right.offset;
		});

		int clicked = -1;
		float clickedDistance = FLT_MAX;
		const float time = static_cast<float>(ImGui::GetTime());
		for (int visibleIndex = 0; visibleIndex < visibleCount; ++visibleIndex)
		{
			const auto& entry = visible[visibleIndex];
			const float distance = std::abs(entry.offset);
			const float opacity = Clamp01(1.0f - std::max(0.0f, distance - visibleRadius));
			if (opacity <= 0.01f)
				continue;
			const bool selected = entry.index == s_menu.selectedLibrary;
			const float magicPulse = MotionLevel() == 2 ?
				0.5f + 0.5f * std::sin(time * 2.7f + entry.index * 0.21f) : 0.5f;
			const float lift = entry.focus * (MotionLevel() == 2 ? 7.0f + magicPulse : 7.0f);
			const float depthStep = cascadeStyle == 2 ? 7.0f : cascadeStyle == 3 ? 27.0f : 18.0f;
			const float designHeight = 206.0f - std::min(distance, 4.0f) * depthStep + entry.focus * 7.0f;
			const float cardSpacing = cardDensity == 5 ? 137.0f : cardDensity == 7 ? 105.0f :
				cardDensity == 9 ? 82.0f : 68.0f;
			const float cascadeDrop = cascadeStyle == 1 ? distance * 5.0f : cascadeStyle == 2 ? 0.0f :
				cascadeStyle == 3 ? distance * 10.0f : distance * 7.0f;
			const ImVec2 center = Point(layout, 790.0f + entry.offset * cardSpacing,
				512.0f + cascadeDrop - lift);
			const float height = designHeight * layout.scale;
			const int cardEffect = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_card_effect.GetValue(), 0, 3);
			if (entry.focus > 0.01f && cardEffect >= 1)
			{
				DrawGlow(draw, center, 132.0f * layout.scale, entry.texture.accent,
					layout.alpha * entry.focus * (0.42f + magicPulse * 0.13f));
				if (cardEffect >= 2)
					DrawCardAura(draw, center, 111.0f * layout.scale, entry.texture.accent, entry.focus, layout.alpha);
			}
			const float fanTilt = entry.offset * (cascadeStyle == 1 ? 0.052f : cascadeStyle == 2 ? 0.0f :
				cascadeStyle == 3 ? 0.018f : 0.034f);
			const float focusTilt = MotionLevel() == 2 ?
				entry.focus * std::sin(time * 1.45f + entry.index * 0.17f) * 0.013f : 0.0f;
			DrawCard(draw, entry.texture, center, height, fanTilt + focusTilt, entry.focus,
				layout.alpha * opacity * (0.88f + entry.focus * 0.12f));
			const float cardWidth = height * (entry.texture.id && entry.texture.size.y > 0 ?
				entry.texture.size.x / entry.texture.size.y : 0.64f);
			if (IsFavorite(s_menu.library[entry.index].filePath))
			{
				const ImVec2 badge{center.x - cardWidth * 0.5f + 13.0f * layout.scale,
					center.y - height * 0.5f + 13.0f * layout.scale};
				draw->AddCircleFilled(badge, 10.0f * layout.scale,
					WithAlpha(IM_COL32(30, 24, 10, 240), layout.alpha * opacity), 24);
				DrawStar(draw, badge, 6.8f * layout.scale,
					WithAlpha(IM_COL32(255, 209, 79, 255), layout.alpha * opacity));
			}
			const ImVec2 minimum{center.x - cardWidth * 0.58f, center.y - height * 0.58f};
			const ImVec2 maximum{center.x + cardWidth * 0.58f, center.y + height * 0.64f};
			if (ClickedRect(layout, minimum, maximum))
			{
				const float mouseDistance = std::abs(ImGui::GetIO().MousePos.x - center.x);
				if (mouseDistance < clickedDistance)
				{
					clickedDistance = mouseDistance;
					clicked = entry.index;
				}
			}
			if (selected)
				s_menu.selectedCardCenter = center;
		}
		if (clicked >= 0)
		{
			const int old = s_menu.selectedLibrary;
			s_menu.selectedLibrary = clicked;
			s_menu.focus = FocusArea::Library;
			UpdateSelectedAnimation(old);
			if (old != s_menu.selectedLibrary)
				PulseHaptics();
			if (ImGui::IsMouseDoubleClicked(0))
				BeginPlacement(s_menu.library[clicked]);
		}
	}

	void DrawDashboard(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const ImVec2 contentMin = Point(layout, 316, 142);
		const ImVec2 contentMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, contentMin, contentMax, 8.0f, true);
		DrawPortalColumn(draw, layout);
		DrawLibraryToolbar(draw, layout);
		DrawLibraryGrid(draw, layout);
	}

	void DrawDetailsPage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		auto texture = GetArtworkTexture(s_menu.details.imagePath);
		if (const auto* definition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant))
			texture.accent = ElementColor(definition->element, texture.accent);
		const ImVec2 cardCenter = Point(layout, 490, 390);
		DrawGlow(draw, cardCenter, 164.0f * layout.scale, texture.accent, layout.alpha * 0.46f);
		DrawCard(draw, texture, cardCenter, 310.0f * layout.scale,
			std::sin(static_cast<float>(ImGui::GetTime()) * 1.3f) * 0.009f, true, layout.alpha);
		DrawText(draw, Point(layout, 660, 207), WithAlpha(texture.accent, layout.alpha),
			12.0f * layout.scale, "SKYLANDER DETAILS");
		DrawTextFit(draw, Point(layout, 657, 237), WithAlpha(IM_COL32(245, 250, 255, 255), layout.alpha),
			29.0f * layout.scale, DrawerWidth(layout, 540.0f), Uppercase(s_menu.details.name));
		draw->AddLine(Point(layout, 660, 285), Point(layout, 1218, 285),
			WithAlpha(IM_COL32(156, 207, 234, 255), layout.alpha * 0.28f), 1.0f * layout.scale);
		DrawText(draw, Point(layout, 660, 317), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "FIGURE ID");
		DrawText(draw, Point(layout, 812, 317), WithAlpha(IM_COL32(237, 246, 252, 255), layout.alpha),
			14.0f * layout.scale, fmt::format("{:04X}", s_menu.details.id));
		DrawText(draw, Point(layout, 660, 355), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "VARIANT");
		DrawText(draw, Point(layout, 812, 355), WithAlpha(IM_COL32(237, 246, 252, 255), layout.alpha),
			14.0f * layout.scale, fmt::format("{:04X}", s_menu.details.variant));
		DrawText(draw, Point(layout, 660, 393), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "PORTAL");
		DrawText(draw, Point(layout, 812, 393),
			WithAlpha(s_menu.details.actualSlot >= 0 ? IM_COL32(94, 225, 170, 255) : IM_COL32(174, 199, 217, 255),
				layout.alpha), 14.0f * layout.scale,
			s_menu.details.actualSlot >= 0 ? fmt::format("SLOT {:02}", s_menu.details.actualSlot + 1) : "NOT ACTIVE");
		if (s_menu.details.fromLibrary)
		{
			const bool favorite = IsFavorite(s_menu.details.filePath);
			DrawText(draw, Point(layout, 660, 431), WithAlpha(CurrentTheme().muted, layout.alpha),
				12.0f * layout.scale, "FAVORITE");
			if (favorite)
				DrawStar(draw, Point(layout, 820, 440), 8.0f * layout.scale,
					WithAlpha(IM_COL32(255, 209, 79, 255), layout.alpha));
			DrawText(draw, Point(layout, favorite ? 837 : 812, 431),
				WithAlpha(favorite ? IM_COL32(255, 217, 111, 255) : IM_COL32(174, 199, 217, 255), layout.alpha),
				14.0f * layout.scale, favorite ? "YES" : "NO");
			DrawText(draw, Point(layout, 660, 481), WithAlpha(CurrentTheme().muted, layout.alpha),
				12.0f * layout.scale, "FILE");
			DrawTextFit(draw, Point(layout, 660, 505), WithAlpha(IM_COL32(207, 223, 235, 255), layout.alpha),
				12.0f * layout.scale, DrawerWidth(layout, 540.0f), _pathToUtf8(s_menu.details.filePath.filename()));
		}
	}

	void DrawDeleteConfirmPage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		const ImU32 warning = IM_COL32(255, 142, 73, 255);
		DrawText(draw, Point(layout, 338, 164), WithAlpha(warning, layout.alpha),
			12.0f * layout.scale, "COLLECTION FILE");
		DrawText(draw, Point(layout, 337, 188), WithAlpha(CurrentTheme().text, layout.alpha),
			24.0f * layout.scale, "MOVE THIS CARD TO RIFT TRASH?");
		draw->AddLine(Point(layout, 338, 230), Point(layout, 1236, 230),
			WithAlpha(IM_COL32(156, 207, 234, 255), layout.alpha * 0.28f), layout.scale);
		DrawTextFit(draw, Point(layout, 390, 292), WithAlpha(IM_COL32(245, 250, 255, 255), layout.alpha),
			29.0f * layout.scale, DrawerWidth(layout, 800.0f), Uppercase(s_menu.details.name));
		DrawTextCentered(draw, Point(layout, 790, 387), WithAlpha(CurrentTheme().text, layout.alpha),
			14.0f * layout.scale, "The .sky file will disappear from your active collection.");
		DrawTextCentered(draw, Point(layout, 790, 425), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "Rift keeps a recoverable copy inside the collection's .rift-trash folder.");
		draw->AddRect(Point(layout, 510, 488), Point(layout, 1070, 548),
			WithAlpha(warning, layout.alpha * 0.52f), CornerRadius(5.0f) * layout.scale, 0, layout.scale);
		DrawTextCentered(draw, Point(layout, 790, 518), WithAlpha(warning, layout.alpha),
			13.0f * layout.scale, "A  CONFIRM DELETE        B  CANCEL");
	}

	void DrawForgePage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const auto theme = CurrentTheme();
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		DrawShadowedText(draw, Point(layout, 338, 162), WithAlpha(theme.text, layout.alpha),
			23.0f * layout.scale, "CREATE A FIGURE", layout.scale);
		const auto definitions = BuildForgeDefinitions();
		if (!definitions.empty())
			s_menu.selectedDefinition = std::clamp(s_menu.selectedDefinition, 0,
				static_cast<int>(definitions.size()) - 1);
		else
			s_menu.selectedDefinition = 0;
		if (!definitions.empty())
		{
			const auto& selected = *definitions[s_menu.selectedDefinition];
			DrawTextFit(draw, Point(layout, 548, 166), WithAlpha(theme.text, layout.alpha),
				17.0f * layout.scale, DrawerWidth(layout, 454.0f), selected.name);
			DrawTextRightAligned(draw, Point(layout, 1238, 170),
				WithAlpha(ElementColor(selected.element, theme.accent), layout.alpha),
				10.5f * layout.scale, fmt::format("{}  /  {}", ElementName(selected.element),
					FigureTypeName(selected.type)));
		}

		const ImVec2 searchMin = Point(layout, 338, 211);
		const ImVec2 searchMax = Point(layout, 685, 253);
		const ImVec2 sortMin = Point(layout, 697, 211);
		const ImVec2 sortMax = Point(layout, 866, 253);
		const ImVec2 typeMin = Point(layout, 878, 211);
		const ImVec2 typeMax = Point(layout, 1056, 253);
		const ImVec2 elementMin = Point(layout, 1068, 211);
		const ImVec2 elementMax = Point(layout, 1238, 253);
		const bool searchFocused = s_menu.forgeToolbarFocused && s_menu.forgeToolbarSelection == 0;
		const bool sortFocused = s_menu.forgeToolbarFocused && s_menu.forgeToolbarSelection == 1;
		const bool typeFocused = s_menu.forgeToolbarFocused && s_menu.forgeToolbarSelection == 2;
		const bool elementFocused = s_menu.forgeToolbarFocused && s_menu.forgeToolbarSelection == 3;
		DrawElevatedSurface(draw, layout, searchMin, searchMax, searchFocused || s_menu.forgeSearchText[0] != '\0');
		DrawElevatedSurface(draw, layout, sortMin, sortMax, sortFocused || s_menu.forgeSortMode != 0);
		DrawElevatedSurface(draw, layout, typeMin, typeMax, typeFocused || s_menu.forgeTypeFilter != 0);
		DrawElevatedSurface(draw, layout, elementMin, elementMax, elementFocused || s_menu.forgeElementFilter != 0,
			s_menu.forgeElementFilter == 0 ? theme.accent :
				ElementColor(ElementForChoice(s_menu.forgeElementFilter), theme.accent));
		DrawSearchIcon(draw, layout, Point(layout, 359, 231));
		ImGui::SetCursorScreenPos(Point(layout, 378, 215));
		ImGui::SetNextItemWidth(DrawerWidth(layout, 294.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 7.0f * layout.scale});
		ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(theme.text, layout.alpha));
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, WithAlpha(theme.muted, layout.alpha));
		ImFont* searchFont = ImGui_GetFont(13.0f * layout.scale);
		if (searchFont)
			ImGui::PushFont(searchFont);
		if (ImGui::InputTextWithHint("##rift_create_search", "Search figures...",
			s_menu.forgeSearchText.data(), s_menu.forgeSearchText.size()))
			s_menu.selectedDefinition = 0;
		if (searchFont)
			ImGui::PopFont();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
		if (ClickedRect(layout, searchMin, searchMax))
			s_menu.focus = FocusArea::Search;
		if (ClickedRect(layout, searchMin, searchMax))
		{
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 0;
		}
		DrawTextFit(draw, Point(layout, 712, 226), WithAlpha(theme.text, layout.alpha),
			11.0f * layout.scale, DrawerWidth(layout, 142.0f), fmt::format("SORT  {}", ForgeSortName()));
		DrawTextFit(draw, Point(layout, 892, 226), WithAlpha(theme.text, layout.alpha),
			11.0f * layout.scale, DrawerWidth(layout, 151.0f), fmt::format("TYPE  {}", ForgeTypeFilterName()));
		DrawTextFit(draw, Point(layout, 1082, 226), WithAlpha(theme.text, layout.alpha),
			11.0f * layout.scale, DrawerWidth(layout, 143.0f),
			fmt::format("ELEMENT  {}", ElementChoiceName(s_menu.forgeElementFilter, "ALL")));
		if (ClickedRect(layout, sortMin, sortMax))
		{
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 1;
			CycleForgeSort();
		}
		if (ClickedRect(layout, typeMin, typeMax))
		{
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 2;
			CycleForgeTypeFilter();
		}
		if (ClickedRect(layout, elementMin, elementMax))
		{
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 3;
			CycleForgeElementFilter();
		}

		if (definitions.empty())
		{
			DrawTextCentered(draw, Point(layout, 788, 376), WithAlpha(theme.text, layout.alpha),
				18.0f * layout.scale, "No figures match these filters");
			DrawTextCentered(draw, Point(layout, 788, 409), WithAlpha(theme.muted, layout.alpha),
				12.0f * layout.scale, "Clear the search or press X to reset the create catalog.");
			return;
		}
		s_menu.selectedDefinition = std::clamp(s_menu.selectedDefinition, 0, static_cast<int>(definitions.size()) - 1);
		const int page = s_menu.selectedDefinition / kLibraryPageSize;
		const int pageBegin = page * kLibraryPageSize;
		const int pageEnd = std::min(pageBegin + kLibraryPageSize, static_cast<int>(definitions.size()));
		DrawTextRightAligned(draw, Point(layout, 1238, 270), WithAlpha(theme.muted, layout.alpha),
			10.0f * layout.scale, fmt::format("{:03} / {:03}", s_menu.selectedDefinition + 1, definitions.size()));
		int clicked = -1;
		for (int index = pageBegin; index < pageEnd; ++index)
		{
			const auto& definition = *definitions[index];
			const int local = index - pageBegin;
			const int column = local % kLibraryColumns;
			const int row = local / kLibraryColumns;
			const bool selected = index == s_menu.selectedDefinition;
			const std::string focusKey = fmt::format("{}:{}", definition.id, definition.variant);
			float& focus = s_forgeFocusAmounts[focusKey];
			focus = AnimateFocus(focus, selected ? 1.0f : 0.0f);
			const float side = column < 2 ? 1.0f : -1.0f;
			const ImVec2 center = Point(layout, 430.0f + column * 220.0f + side * focus * 8.0f,
				354.0f + row * 170.0f - focus * 10.0f);
			auto texture = GetArtworkTexture(definition.imagePath);
			texture.accent = ElementColor(definition.element, texture.accent);
			if (focus > 0.01f)
				DrawGlow(draw, center, 102.0f * layout.scale, texture.accent, layout.alpha * focus * 0.50f);
			DrawCard(draw, texture, center, (138.0f + focus * 16.0f) * layout.scale,
				side * (1.0f - focus) * 0.012f, focus, layout.alpha * (0.80f + focus * 0.20f));
			const ImVec2 minimum = Point(layout, 356.0f + column * 220.0f, 278.0f + row * 170.0f);
			const ImVec2 maximum = Point(layout, 504.0f + column * 220.0f, 430.0f + row * 170.0f);
			if (layout.alpha > 0.9f && ImGui::IsMouseHoveringRect(minimum, maximum) &&
				index != s_menu.selectedDefinition)
				s_menu.selectedDefinition = index;
			if (ImGui::IsMouseHoveringRect(minimum, maximum))
				ImGui::SetTooltip("%s", definition.name.c_str());
			if (ClickedRect(layout, minimum, maximum))
			{
				s_menu.forgeToolbarFocused = false;
				clicked = index;
			}
		}
		if (clicked >= 0)
		{
			if (clicked == s_menu.selectedDefinition && ImGui::IsMouseDoubleClicked(0))
				ForgeSelectedFigure();
			else if (clicked != s_menu.selectedDefinition)
			{
				s_menu.selectedDefinition = clicked;
				PulseHaptics();
			}
		}
	}

	std::string OptionValue(int option)
	{
		auto& config = GetConfig().emulated_usb_devices;
		switch (option)
		{
		case 0:
			switch (std::clamp<sint32>(config.skylander_motion_level.GetValue(), 0, 2))
			{
			case 0: return "OFF";
			case 1: return "REDUCED";
			default: return "FULL";
			}
		case 1:
			switch (std::clamp<sint32>(config.skylander_bloom_level.GetValue(), 0, 2))
			{
			case 0: return "OFF";
			case 1: return "NORMAL";
			default: return "HIGH";
			}
		case 2: return fmt::format("{}%", std::clamp<sint32>(config.skylander_drawer_opacity.GetValue(), 0, 100));
		case 3: return config.skylander_ui_sound.GetValue() ? "ON" : "OFF";
		case 4: return config.skylander_ui_haptics.GetValue() ? "ON" : "OFF";
		case 5:
		{
			const int density = std::clamp<sint32>(config.skylander_card_density.GetValue(), 5, 11);
			return fmt::format("{} CARDS", density % 2 == 0 ? density + 1 : density);
		}
		case 6:
		{
			static constexpr std::array names{"CLASSIC", "DARK RIFT", "EMBER", "VERDANT", "ARCANE"};
			return names[std::clamp<sint32>(config.skylander_theme.GetValue(), 0, 4)];
		}
		case 7:
		{
			static constexpr std::array names{"THEME", "CYAN", "VIOLET", "GOLD", "GREEN", "CORAL"};
			return names[std::clamp<sint32>(config.skylander_accent.GetValue(), 0, 5)];
		}
		case 8:
		{
			static constexpr std::array names{"CLEAN", "GLOW", "RIFT", "FOIL"};
			return names[std::clamp<sint32>(config.skylander_card_effect.GetValue(), 0, 3)];
		}
		case 9:
		{
			static constexpr std::array names{"OFF", "LOW", "NORMAL", "HIGH"};
			return names[std::clamp<sint32>(config.skylander_particle_level.GetValue(), 0, 3)];
		}
		case 10:
		{
			static constexpr std::array names{"SOFT", "TACTILE", "ARCANE", "BRIGHT"};
			return names[std::clamp<sint32>(config.skylander_sound_profile.GetValue(), 0, 3)];
		}
		case 11:
		{
			static constexpr std::array names{"OFF", "SOFT", "MEDIUM", "STRONG"};
			return names[std::clamp<sint32>(config.skylander_haptic_strength.GetValue(), 0, 3)];
		}
		case 12:
		{
			static constexpr std::array names{"SQUARE", "SUBTLE", "ROUND"};
			return names[std::clamp<sint32>(config.skylander_corner_style.GetValue(), 0, 2)];
		}
		case 13:
		{
			static constexpr std::array names{"MINIMAL", "ELEMENT", "ETCHED", "RUNES"};
			return names[std::clamp<sint32>(config.skylander_card_border.GetValue(), 0, 3)];
		}
		case 14:
		{
			static constexpr std::array names{"ARC", "FAN", "FLAT", "DEPTH"};
			return names[std::clamp<sint32>(config.skylander_cascade_style.GetValue(), 0, 3)];
		}
		case 15: return "CHECK NOW";
		default: return {};
		}
	}

	void OpenLibraryOrder()
	{
		s_menu.page = RiftPage::LibraryOrder;
		s_menu.selectedLibraryOption = 0;
		PulseHaptics(UiSound::Confirm);
	}

	std::string LibraryOptionValue(int option)
	{
		auto& config = GetConfig().emulated_usb_devices;
		switch (option)
		{
		case 0: return std::string(SortModeName());
		case 1: return std::string(PriorityModeName());
		case 2: return std::string(ElementChoiceName(
			std::clamp<sint32>(config.skylander_element_priority.GetValue(), 0, 11), "NONE"));
		case 3: return std::string(FilterModeName());
		case 4: return std::string(ElementChoiceName(
			std::clamp<sint32>(config.skylander_element_filter.GetValue(), 0, 11), "ALL ELEMENTS"));
		default: return {};
		}
	}

	void AdjustLibraryOption(int option, int direction)
	{
		auto& config = GetConfig().emulated_usb_devices;
		switch (option)
		{
		case 0:
			config.skylander_sort_mode = WrapChoice(config.skylander_sort_mode.GetValue() + direction,
				static_cast<int>(SortMode::Count));
			break;
		case 1:
			config.skylander_priority_mode = WrapChoice(config.skylander_priority_mode.GetValue() + direction, 5);
			break;
		case 2:
			config.skylander_element_priority = WrapChoice(
				config.skylander_element_priority.GetValue() + direction, 12);
			break;
		case 3:
			config.skylander_filter_mode = WrapChoice(config.skylander_filter_mode.GetValue() + direction, 6);
			break;
		case 4:
			config.skylander_element_filter = WrapChoice(
				config.skylander_element_filter.GetValue() + direction, 12);
			break;
		default: return;
		}
		g_config.Save();
		RebuildLibrary();
		SetToast("LIBRARY: " + LibraryOptionValue(option), 1.6f);
		PulseHaptics(UiSound::Confirm);
	}

	void AdjustOption(int option, int direction)
	{
		auto& config = GetConfig().emulated_usb_devices;
		const std::string previousValue = OptionValue(option);
		switch (option)
		{
		case 0:
			config.skylander_motion_level = WrapChoice(config.skylander_motion_level.GetValue() + direction, 3);
			break;
		case 1:
			config.skylander_bloom_level = WrapChoice(config.skylander_bloom_level.GetValue() + direction, 3);
			break;
		case 2:
			config.skylander_drawer_opacity = std::clamp<sint32>(
				config.skylander_drawer_opacity.GetValue() + direction * 5, 40, 100);
			break;
		case 3:
			config.skylander_ui_sound = !config.skylander_ui_sound.GetValue();
			break;
		case 4:
			config.skylander_ui_haptics = !config.skylander_ui_haptics.GetValue();
			if (config.skylander_ui_haptics.GetValue() && config.skylander_haptic_strength.GetValue() == 0)
				config.skylander_haptic_strength = 2;
			if (!config.skylander_ui_haptics.GetValue())
				StopMenuHaptics();
			break;
		case 5:
		{
			const int density = std::clamp<sint32>(config.skylander_card_density.GetValue(), 5, 11);
			const int densityIndex = std::clamp((density - 5) / 2, 0, 3);
			config.skylander_card_density = 5 + WrapChoice(densityIndex + direction, 4) * 2;
			break;
		}
		case 6: config.skylander_theme = WrapChoice(config.skylander_theme.GetValue() + direction, 5); break;
		case 7: config.skylander_accent = WrapChoice(config.skylander_accent.GetValue() + direction, 6); break;
		case 8: config.skylander_card_effect = WrapChoice(config.skylander_card_effect.GetValue() + direction, 4); break;
		case 9: config.skylander_particle_level = WrapChoice(config.skylander_particle_level.GetValue() + direction, 4); break;
		case 10: config.skylander_sound_profile = WrapChoice(config.skylander_sound_profile.GetValue() + direction, 4); break;
		case 11:
			config.skylander_haptic_strength = WrapChoice(config.skylander_haptic_strength.GetValue() + direction, 4);
			config.skylander_ui_haptics = config.skylander_haptic_strength.GetValue() > 0;
			if (!config.skylander_ui_haptics.GetValue()) StopMenuHaptics();
			break;
		case 12: config.skylander_corner_style = WrapChoice(config.skylander_corner_style.GetValue() + direction, 3); break;
		case 13: config.skylander_card_border = WrapChoice(config.skylander_card_border.GetValue() + direction, 4); break;
		case 14: config.skylander_cascade_style = WrapChoice(config.skylander_cascade_style.GetValue() + direction, 4); break;
		case 15:
			RequestUpdateCheck();
			return;
		default: return;
		}
		if (OptionValue(option) == previousValue)
			return;
		g_config.Save();
		SetToast("RIFT OPTION: " + OptionValue(option), 1.5f);
		PulseHaptics(UiSound::Confirm);
	}

	void DrawOptionsPage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		DrawText(draw, Point(layout, 338, 164), WithAlpha(CurrentTheme().accent, layout.alpha),
			12.0f * layout.scale, "RIFT SETTINGS");
		DrawText(draw, Point(layout, 337, 188), WithAlpha(CurrentTheme().text, layout.alpha),
			24.0f * layout.scale, "DISPLAY, FEEDBACK & UPDATES");
		DrawText(draw, Point(layout, 337, 222), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "USE THE D-PAD TO CHOOSE. PRESS A TO CHANGE THE SELECTED OPTION.");

		const std::array<std::pair<std::string_view, std::string_view>, kOptionCount> rows{{
			{"MOTION", "Opening, card lift and magical movement"},
			{"BLOOM", "Portal and selected-card light"},
			{"GLASS OPACITY", "How much of the game shows through"},
			{"UI SOUND", "Navigation and confirmation clicks"},
			{"CONTROLLER HAPTICS", "Short focus and action pulses"},
			{"CARD CASCADE", "Visible library density"},
			{"COLOR SCHEME", "Complete Rift palette"},
			{"ACCENT COLOR", "Highlight and portal glow"},
			{"CARD EFFECT", "Clean, glow, rift, or foil"},
			{"PARTICLES", "Ambient magical texture"},
			{"SOUND PROFILE", "Soft through bright tactile clicks"},
			{"HAPTIC STRENGTH", "Off through strong feedback"},
			{"CORNERS", "Square, subtle, or round panels"},
			{"CARD BORDER", "Element edge, etched frame, or runes"},
			{"CASCADE SHAPE", "Arc, fan, flat, or deep stack"},
			{"CHECK FOR UPDATES", "Compare this build with the latest GitHub release"}}};
		for (int option = 0; option < static_cast<int>(rows.size()); ++option)
		{
			const int column = option / 8;
			const int row = option % 8;
			const float x = 338.0f + column * 454.0f;
			const float y = 249.0f + row * 43.0f;
			const ImVec2 minimum = Point(layout, x, y);
			const ImVec2 maximum = Point(layout, x + 440.0f, y + 36.0f);
			const bool selected = option == s_menu.selectedOption;
			float& focusAnimation = s_menu.optionFocusAnimations[option];
			focusAnimation = AnimateFocus(focusAnimation, selected ? 1.0f : 0.0f);
			draw->AddRectFilled(minimum, maximum, WithAlpha(CurrentTheme().surface, layout.alpha * 0.52f), CornerRadius(4.0f) * layout.scale);
			if (focusAnimation > 0.01f)
			{
				draw->AddRectFilled({minimum.x + 4.0f * layout.scale, minimum.y + 6.0f * layout.scale},
					{maximum.x + 4.0f * layout.scale, maximum.y + 6.0f * layout.scale},
					WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * focusAnimation * 0.30f),
					4.0f * layout.scale);
				draw->AddRectFilled(minimum, maximum,
					WithAlpha(CurrentTheme().selectedSurface, layout.alpha * focusAnimation * 0.86f),
					4.0f * layout.scale);
			}
			draw->AddRect(minimum, maximum,
				WithAlpha(BlendColor(CurrentTheme().border, CurrentTheme().accent, focusAnimation),
					layout.alpha * (0.20f + focusAnimation * 0.56f)), 4.0f * layout.scale, 0,
				(1.0f + focusAnimation * 0.8f) * layout.scale);
			DrawText(draw, Point(layout, x + 12.0f, y + 3.0f),
				WithAlpha(BlendColor(Lighten(CurrentTheme().muted, 0.15f), CurrentTheme().text, focusAnimation), layout.alpha),
				11.5f * layout.scale, rows[option].first);
			DrawTextFit(draw, Point(layout, x + 12.0f, y + 19.0f), WithAlpha(CurrentTheme().muted, layout.alpha),
				8.8f * layout.scale, DrawerWidth(layout, 290.0f), rows[option].second);
			DrawTextFit(draw, Point(layout, x + 318.0f, y + 9.0f),
				WithAlpha(BlendColor(CurrentTheme().text, CurrentTheme().accent, focusAnimation), layout.alpha),
				11.5f * layout.scale, DrawerWidth(layout, 108.0f), OptionValue(option));
			if (ClickedRect(layout, minimum, maximum))
			{
				if (s_menu.selectedOption == option)
					AdjustOption(option, 1);
				else
				{
					s_menu.selectedOption = option;
					PulseHaptics();
				}
			}
		}
	}

	void DrawLibraryOrderPage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		DrawText(draw, Point(layout, 338, 164), WithAlpha(CurrentTheme().accent, layout.alpha),
			12.0f * layout.scale, "LIBRARY ORDER");
		DrawText(draw, Point(layout, 337, 188), WithAlpha(CurrentTheme().text, layout.alpha),
			24.0f * layout.scale, "PUT THE NEXT FIGURE ONE MOVE AWAY");
		DrawText(draw, Point(layout, 337, 222), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "SORT, PIN, OR NARROW THE CASCADE WITHOUT LEAVING THE GAME.");
		DrawText(draw, Point(layout, 1158, 188), WithAlpha(CurrentTheme().accent, layout.alpha),
			13.0f * layout.scale, fmt::format("{} MATCH", s_menu.library.size()));

		const std::array<std::pair<std::string_view, std::string_view>, 5> rows{{
			{"SORT BY", "Base order inside the card cascade"},
			{"PIN FIRST", "Favorites, traps, figures, or items"},
			{"ELEMENT FIRST", "Bring one element to the front"},
			{"SHOW", "Limit the library by figure family"},
			{"ELEMENT FILTER", "Show only one element"}}};
		for (int option = 0; option < static_cast<int>(rows.size()); ++option)
		{
			const float y = 264.0f + option * 62.0f;
			const ImVec2 minimum = Point(layout, 338, y);
			const ImVec2 maximum = Point(layout, 1238, y + 51.0f);
			const bool selected = option == s_menu.selectedLibraryOption;
			float& focusAnimation = s_menu.libraryOptionFocusAnimations[option];
			focusAnimation = AnimateFocus(focusAnimation, selected ? 1.0f : 0.0f);
			draw->AddRectFilled(minimum, maximum, WithAlpha(CurrentTheme().surface, layout.alpha * 0.52f), CornerRadius(4.0f) * layout.scale);
			if (focusAnimation > 0.01f)
			{
				draw->AddRectFilled({minimum.x + 5.0f * layout.scale, minimum.y + 7.0f * layout.scale},
					{maximum.x + 5.0f * layout.scale, maximum.y + 7.0f * layout.scale},
					WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * focusAnimation * 0.32f),
					4.0f * layout.scale);
				draw->AddRectFilled(minimum, maximum,
					WithAlpha(CurrentTheme().selectedSurface, layout.alpha * focusAnimation * 0.88f),
					4.0f * layout.scale);
			}
			draw->AddRect(minimum, maximum,
				WithAlpha(BlendColor(CurrentTheme().border, CurrentTheme().accent, focusAnimation),
					layout.alpha * (0.20f + focusAnimation * 0.56f)), 4.0f * layout.scale, 0,
				(1.0f + focusAnimation * 0.8f) * layout.scale);
			DrawText(draw, Point(layout, 355, y + 9.0f),
				WithAlpha(selected ? IM_COL32(235, 248, 255, 255) : IM_COL32(211, 227, 238, 255), layout.alpha),
				13.0f * layout.scale, rows[option].first);
			DrawText(draw, Point(layout, 555, y + 9.0f), WithAlpha(IM_COL32(132, 167, 191, 255), layout.alpha),
				11.5f * layout.scale, rows[option].second);
			DrawTextFit(draw, Point(layout, 1058, y + 9.0f),
				WithAlpha(selected ? CurrentTheme().accent : IM_COL32(226, 238, 246, 255), layout.alpha),
				13.0f * layout.scale, DrawerWidth(layout, 160.0f), LibraryOptionValue(option));
			if (ClickedRect(layout, minimum, maximum))
			{
				if (s_menu.selectedLibraryOption == option)
					AdjustLibraryOption(option, 1);
				else
				{
					s_menu.selectedLibraryOption = option;
					PulseHaptics();
				}
			}
		}
	}

	ArtworkTexture GetControllerGlyph(std::string_view glyph)
	{
		std::string_view fileName;
		if (glyph == "A") fileName = "xbox_button_a.png";
		else if (glyph == "B") fileName = "xbox_button_b.png";
		else if (glyph == "X") fileName = "xbox_button_x.png";
		else if (glyph == "Y") fileName = "xbox_button_y.png";
		else if (glyph == "LB") fileName = "xbox_lb.png";
		else if (glyph == "RB") fileName = "xbox_rb.png";
		else if (glyph == "DPAD_DOWN") fileName = "xbox_dpad_down.png";
		else if (glyph == "MENU") fileName = "xbox_button_menu.png";
		else if (glyph == "VIEW") fileName = "xbox_button_view.png";
		else return {};
		return GetArtworkTexture(s_menu.catalog.GetAssetsPath() / "ui" / "glyphs" / "kenney" /
			"Xbox Series" / "Default" / fileName);
	}

	void DrawActionHint(ImDrawList* draw, const RiftLayout& layout, float x, std::string_view glyph,
		std::string_view label)
	{
		const float glyphSize = glyph.size() > 1 ? 27.0f : 24.0f;
		const ImVec2 center = Point(layout, x + glyphSize * 0.5f, 674);
		const ImVec2 minimum{center.x - glyphSize * 0.5f * layout.scale,
			center.y - glyphSize * 0.5f * layout.scale};
		const ImVec2 maximum{center.x + glyphSize * 0.5f * layout.scale,
			center.y + glyphSize * 0.5f * layout.scale};
		const auto texture = GetControllerGlyph(glyph);
		if (texture.id)
			draw->AddImage(texture.id, minimum, maximum, texture.uvMinimum, texture.uvMaximum,
				WithAlpha(IM_COL32(255, 255, 255, 255), layout.alpha));
		else
		{
			draw->AddRect(minimum, maximum,
				WithAlpha(IM_COL32(227, 237, 244, 255), layout.alpha * 0.72f), 3.0f * layout.scale);
			DrawText(draw, {minimum.x + 5.0f * layout.scale, minimum.y + 4.0f * layout.scale},
				WithAlpha(IM_COL32(245, 248, 251, 255), layout.alpha), 11.0f * layout.scale, glyph);
		}
		ImFont* font = ImGui_GetFont(12.0f * layout.scale);
		if (!font)
			font = ImGui::GetFont();
		const ImVec2 labelSize = font->CalcTextSizeA(12.0f * layout.scale, FLT_MAX, 0.0f,
			label.data(), label.data() + label.size());
		DrawText(draw, {maximum.x + 8.0f * layout.scale, center.y - labelSize.y * 0.5f},
			WithAlpha(IM_COL32(220, 232, 240, 255), layout.alpha), 12.0f * layout.scale, label);
	}

	void DrawOpenChordHint(ImDrawList* draw, const RiftLayout& layout, float x)
	{
		const float size = 22.0f * layout.scale;
		const ImVec2 center = Point(layout, x + 11.0f, 674);
		const auto shoulder = GetControllerGlyph("RB");
		const auto dpad = GetControllerGlyph("DPAD_DOWN");
		if (shoulder.id)
			draw->AddImage(shoulder.id, {center.x - size * 0.5f, center.y - size * 0.5f},
				{center.x + size * 0.5f, center.y + size * 0.5f},
				shoulder.uvMinimum, shoulder.uvMaximum,
				WithAlpha(IM_COL32_WHITE, layout.alpha));
		DrawText(draw, {center.x + 14.0f * layout.scale, center.y - 7.0f * layout.scale},
			WithAlpha(IM_COL32(151, 181, 202, 255), layout.alpha), 11.0f * layout.scale, "+");
		const ImVec2 secondCenter{center.x + 37.0f * layout.scale, center.y};
		if (dpad.id)
			draw->AddImage(dpad.id, {secondCenter.x - size * 0.5f, secondCenter.y - size * 0.5f},
				{secondCenter.x + size * 0.5f, secondCenter.y + size * 0.5f},
				dpad.uvMinimum, dpad.uvMaximum,
				WithAlpha(IM_COL32_WHITE, layout.alpha));
		DrawText(draw, {secondCenter.x + 18.0f * layout.scale, secondCenter.y - 7.0f * layout.scale},
			WithAlpha(IM_COL32(177, 204, 222, 255), layout.alpha), 11.0f * layout.scale, "RIFT");
	}

	bool FooterClicked(const RiftLayout& layout, float minimumX, float maximumX)
	{
		return ClickedRect(layout, Point(layout, minimumX, kFooterTop), Point(layout, maximumX, 720));
	}

	void DrawFooter(ImDrawList* draw, const RiftLayout& layout)
	{
		draw->AddRectFilled(Point(layout, kDrawerX, kFooterTop), Point(layout, 1280, 720),
			WithAlpha(CurrentTheme().background, layout.alpha * 0.86f));
		draw->AddLine(Point(layout, kDrawerX, kFooterTop), Point(layout, 1280, kFooterTop),
			WithAlpha(IM_COL32(157, 207, 234, 255), layout.alpha * 0.24f), 1.0f * layout.scale);
		if (s_menu.page == RiftPage::Dashboard)
		{
			if (s_menu.focus == FocusArea::Search)
			{
				DrawActionHint(draw, layout, 318, "A", "SEARCH");
				DrawActionHint(draw, layout, 438, "X", "CLEAR");
				DrawActionHint(draw, layout, 548, "LB", "ORDER");
				DrawActionHint(draw, layout, 665, "B", "CLOSE");
				DrawText(draw, Point(layout, 975, 668), WithAlpha(IM_COL32(140, 171, 193, 255), layout.alpha),
					11.0f * layout.scale, "A  OPEN CONTROLLER KEYBOARD");
				if (FooterClicked(layout, 310, 425))
					OpenSearchKeyboard(SearchTarget::Collection);
				else if (FooterClicked(layout, 427, 535))
				{
					s_menu.searchText.fill(0);
					RebuildLibrary();
					PulseHaptics();
				}
				else if (FooterClicked(layout, 537, 650))
					OpenLibraryOrder();
				else if (FooterClicked(layout, 652, 780))
				{
					SetMenuOpen(false, false);
					PulseHaptics(UiSound::Back);
				}
			}
			else if (s_menu.focus == FocusArea::ElementFilter)
			{
				DrawActionHint(draw, layout, 318, "A", "APPLY FILTER");
				DrawActionHint(draw, layout, 474, "LB", "ORDER");
				DrawActionHint(draw, layout, 590, "B", "CLOSE");
				DrawText(draw, Point(layout, 900, 668), WithAlpha(IM_COL32(140, 171, 193, 255), layout.alpha),
					11.0f * layout.scale, "D-PAD LEFT / RIGHT  CHOOSE ELEMENT");
				if (FooterClicked(layout, 310, 465))
					ApplySelectedElementFilter();
				else if (FooterClicked(layout, 467, 580))
					OpenLibraryOrder();
				else if (FooterClicked(layout, 582, 710))
				{
					SetMenuOpen(false, false);
					PulseHaptics(UiSound::Back);
				}
			}
			else
			{
				DrawActionHint(draw, layout, 318, "A", "PLACE");
				DrawActionHint(draw, layout, 430, "X", "REMOVE");
				DrawActionHint(draw, layout, 545, "Y", "DETAILS");
				DrawActionHint(draw, layout, 658, "LB", "ORDER");
				DrawActionHint(draw, layout, 750, "RB", "FAVORITE");
				DrawActionHint(draw, layout, 877, "B", "CLOSE");
				DrawOpenChordHint(draw, layout, 1138);
				if (FooterClicked(layout, 310, 420) && !s_menu.library.empty())
					BeginPlacement(s_menu.library[s_menu.selectedLibrary]);
				else if (FooterClicked(layout, 422, 535))
					RemoveFocusedFigure();
				else if (FooterClicked(layout, 537, 650))
				{
					if (s_menu.focus == FocusArea::Portal)
						OpenPortalDetails();
					else if (s_menu.focus == FocusArea::Library)
						OpenLibraryDetails();
				}
				else if (FooterClicked(layout, 652, 742))
					OpenLibraryOrder();
				else if (FooterClicked(layout, 744, 869))
					ToggleFavorite();
				else if (FooterClicked(layout, 871, 990))
				{
					SetMenuOpen(false, false);
					PulseHaptics(UiSound::Back);
				}
			}
		}
		else if (s_menu.page == RiftPage::Details)
		{
			if (s_menu.details.fromLibrary)
				DrawActionHint(draw, layout, 318, "A", "PLACE");
			if (s_menu.details.fromLibrary)
				DrawActionHint(draw, layout, 430, "X", "DELETE FILE");
			else if (s_menu.details.actualSlot >= 0)
				DrawActionHint(draw, layout, 430, "X", "REMOVE");
			if (s_menu.details.fromLibrary)
				DrawActionHint(draw, layout, 560, "RB", "FAVORITE");
			DrawActionHint(draw, layout, 700, "B", "BACK");
			if (FooterClicked(layout, 310, 420) && s_menu.details.fromLibrary)
			{
				const auto* definition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant);
				BeginPlacement({s_menu.details.filePath, s_menu.details.id, s_menu.details.variant,
					s_menu.details.name, s_menu.details.imagePath,
					definition ? definition->element : skylander_ui::FigureElement::Unknown,
					definition ? definition->type : skylander_ui::FigureType::Unknown});
			}
			else if (FooterClicked(layout, 422, 535))
			{
				if (s_menu.details.fromLibrary)
					RequestDeleteDetailFigure();
				else if (s_menu.details.actualSlot >= 0)
					RemoveActualSlot(s_menu.details.actualSlot);
			}
			else if (FooterClicked(layout, 552, 680) && s_menu.details.fromLibrary)
				ToggleFavoritePath(s_menu.details.filePath, s_menu.details.name);
			else if (FooterClicked(layout, 690, 815))
			{
				s_menu.page = RiftPage::Dashboard;
				s_menu.focus = FocusArea::Library;
				PulseHaptics(UiSound::Back);
			}
		}
		else if (s_menu.page == RiftPage::DeleteConfirm)
		{
			DrawActionHint(draw, layout, 318, "A", "CONFIRM DELETE");
			DrawActionHint(draw, layout, 510, "B", "CANCEL");
			if (FooterClicked(layout, 310, 495))
				DeleteDetailFigure();
			else if (FooterClicked(layout, 500, 640))
			{
				s_menu.page = RiftPage::Details;
				PulseHaptics(UiSound::Back);
			}
		}
		else if (s_menu.page == RiftPage::Forge)
		{
			DrawActionHint(draw, layout, 318, "A", s_menu.forgeToolbarFocused ? "SELECT" : "CREATE .SKY");
			DrawActionHint(draw, layout, 470, "B", "BACK");
			DrawActionHint(draw, layout, 592, "X", "RESET FILTERS");
			DrawText(draw, Point(layout, 876, 668), WithAlpha(IM_COL32(140, 171, 193, 255), layout.alpha),
				10.0f * layout.scale, s_menu.forgeToolbarFocused ?
					"D-PAD  CHOOSE TOOL     A  SELECT" : "D-PAD  BROWSE     UP  SEARCH & FILTERS");
			if (FooterClicked(layout, 310, 455))
			{
				if (!s_menu.forgeToolbarFocused)
					ForgeSelectedFigure();
				else if (s_menu.forgeToolbarSelection == 0)
					OpenSearchKeyboard(SearchTarget::Create);
				else if (s_menu.forgeToolbarSelection == 1)
					CycleForgeSort();
				else if (s_menu.forgeToolbarSelection == 2)
					CycleForgeTypeFilter();
				else
					CycleForgeElementFilter();
			}
			else if (FooterClicked(layout, 458, 585))
			{
				s_menu.page = RiftPage::Dashboard;
				s_menu.focus = FocusArea::Library;
				PulseHaptics(UiSound::Back);
			}
			else if (FooterClicked(layout, 588, 760))
				ResetForgeFilters();
		}
		else
		{
			DrawActionHint(draw, layout, 318, "A", s_menu.page == RiftPage::Options &&
				s_menu.selectedOption == kOptionCount - 1 ? "CHECK" : "CHANGE");
			DrawActionHint(draw, layout, 445, "B", "BACK");
			DrawText(draw, Point(layout, 965, 668), WithAlpha(IM_COL32(140, 171, 193, 255), layout.alpha),
				11.0f * layout.scale, s_menu.page == RiftPage::Options &&
					s_menu.selectedOption == kOptionCount - 1 ?
					"D-PAD  SELECT     A  CHECK" : "D-PAD  SELECT     A  CHANGE");
			if (FooterClicked(layout, 310, 430))
			{
				if (s_menu.page == RiftPage::Options)
					AdjustOption(s_menu.selectedOption, 1);
				else
					AdjustLibraryOption(s_menu.selectedLibraryOption, 1);
			}
			else if (FooterClicked(layout, 435, 570))
			{
				s_menu.page = RiftPage::Dashboard;
				s_menu.focus = FocusArea::Library;
				PulseHaptics(UiSound::Back);
			}
		}
	}

	void DrawPlacementAnimation(ImDrawList* draw, const RiftLayout& layout)
	{
		if (s_menu.placementAnimation >= 1.0f)
			return;
		const float t = SmoothStep(s_menu.placementAnimation);
		const ImVec2 start = s_menu.selectedCardCenter;
		const ImVec2 target = s_menu.portalTargets[std::clamp(s_menu.placementTargetRow, 0, kPortalCapacity - 1)];
		const ImVec2 center{
			start.x + (target.x - start.x) * t,
			start.y + (target.y - start.y) * t - std::sin(t * kPi) * 82.0f * layout.scale};
		for (int trail = 1; trail <= 4; ++trail)
		{
			const float trailT = std::max(0.0f, t - trail * 0.045f);
			const ImVec2 trailCenter{
				start.x + (target.x - start.x) * trailT,
				start.y + (target.y - start.y) * trailT - std::sin(trailT * kPi) * 82.0f * layout.scale};
			draw->AddCircleFilled(trailCenter, (10.0f - trail * 1.4f) * layout.scale,
				WithAlpha(s_menu.placementAccent, layout.alpha * (0.18f - trail * 0.025f)), 24);
		}
		auto texture = GetArtworkTexture(s_menu.placementArtwork);
		texture.accent = s_menu.placementAccent;
		DrawGlow(draw, center, (118.0f - 40.0f * t) * layout.scale, texture.accent,
			layout.alpha * (0.62f - t * 0.26f));
		DrawCard(draw, texture, center, (185.0f - 108.0f * t) * layout.scale,
			-std::sin(t * kPi) * 0.11f, true, layout.alpha);
	}

	void DrawPlacementImpact(ImDrawList* draw, const RiftLayout& layout)
	{
		if (s_menu.placementImpact <= 0.0f)
			return;
		const float progress = 1.0f - s_menu.placementImpact;
		const ImVec2 center = s_menu.portalTargets[std::clamp(s_menu.placementTargetRow, 0, kPortalCapacity - 1)];
		for (int ring = 0; ring < 3; ++ring)
		{
			const float radius = (24.0f + progress * 54.0f + ring * 8.0f) * layout.scale;
			draw->AddCircle(center, radius, WithAlpha(s_menu.placementAccent,
				layout.alpha * s_menu.placementImpact * (0.48f - ring * 0.10f)), 40,
				(2.4f - ring * 0.45f) * layout.scale);
		}
	}

	void DrawToast(ImDrawList* draw, const RiftLayout& layout)
	{
		if (s_menu.toastLife <= 0.0f || s_menu.toast.empty())
			return;
		const float fade = Clamp01(s_menu.toastLife * 2.0f);
		ImFont* font = ImGui_GetFont(14.0f * layout.scale);
		if (!font)
			font = ImGui::GetFont();
		const ImVec2 textSize = font->CalcTextSizeA(14.0f * layout.scale, FLT_MAX, 0.0f,
			s_menu.toast.data(), s_menu.toast.data() + s_menu.toast.size());
		const float width = std::min(DrawerWidth(layout, 620.0f), textSize.x + DrawerWidth(layout, 58.0f));
		const ImVec2 center = Point(layout, 790, 132);
		const ImVec2 minimum{center.x - width * 0.5f, center.y - 21.0f * layout.scale};
		const ImVec2 maximum{center.x + width * 0.5f, center.y + 21.0f * layout.scale};
		draw->AddRectFilled({minimum.x + DrawerWidth(layout, 5.0f), minimum.y + 7.0f * layout.scale},
			{maximum.x + DrawerWidth(layout, 5.0f), maximum.y + 7.0f * layout.scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * fade * 0.42f), 9.0f * layout.scale);
		draw->AddRectFilled(minimum, maximum, WithAlpha(IM_COL32(6, 18, 34, 242), layout.alpha * fade),
			9.0f * layout.scale);
		draw->AddRect(minimum, maximum, WithAlpha(IM_COL32(97, 214, 255, 255), layout.alpha * fade * 0.64f),
			9.0f * layout.scale, 0, 1.2f * layout.scale);
		DrawTextFit(draw, {minimum.x + DrawerWidth(layout, 22.0f), center.y - 8.0f * layout.scale},
			WithAlpha(IM_COL32(238, 247, 252, 255), layout.alpha * fade),
			14.0f * layout.scale, width - DrawerWidth(layout, 44.0f), s_menu.toast);
	}

	void DrawOnScreenKeyboard(ImDrawList* draw, const RiftLayout& layout)
	{
		if (s_menu.searchTarget == SearchTarget::None)
			return;
		const auto theme = CurrentTheme();
		draw->AddRectFilled(Point(layout, kDrawerX, 0), Point(layout, 1280, 720),
			WithAlpha(IM_COL32(0, 2, 9, 255), layout.alpha * 0.68f));
		const ImVec2 panelMin = Point(layout, 376, 142);
		const ImVec2 panelMax = Point(layout, 1248, 620);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 9.0f, true);
		const std::string_view title = s_menu.searchTarget == SearchTarget::Create ?
			"SEARCH FIGURES" : "SEARCH COLLECTION";
		DrawShadowedText(draw, Point(layout, 412, 169), WithAlpha(theme.text, layout.alpha),
			24.0f * layout.scale, title, layout.scale);
		DrawTextRightAligned(draw, Point(layout, 1216, 176), WithAlpha(theme.muted, layout.alpha),
			10.0f * layout.scale, "CONTROLLER KEYBOARD");

		const ImVec2 fieldMin = Point(layout, 412, 210);
		const ImVec2 fieldMax = Point(layout, 1216, 258);
		DrawElevatedSurface(draw, layout, fieldMin, fieldMax, true);
		const auto& searchText = ActiveSearchText();
		const std::string_view value = searchText[0] ? std::string_view(searchText.data()) : std::string_view("Search is empty");
		DrawTextFit(draw, Point(layout, 433, 224),
			WithAlpha(searchText[0] ? theme.text : theme.muted, layout.alpha),
			15.0f * layout.scale, DrawerWidth(layout, 760.0f), value);

		for (int row = 0; row < 5; ++row)
		{
			const int count = KeyboardColumnCount(row);
			const float gap = 8.0f;
			const float rowWidth = 780.0f;
			const float keyWidth = (rowWidth - gap * (count - 1)) / count;
			const float startX = 814.0f - rowWidth * 0.5f;
			const float y = 282.0f + row * 56.0f;
			for (int column = 0; column < count; ++column)
			{
				const ImVec2 minimum = Point(layout, startX + column * (keyWidth + gap), y);
				const ImVec2 maximum = Point(layout, startX + column * (keyWidth + gap) + keyWidth, y + 42.0f);
				const bool selected = row == s_menu.keyboardRow && column == s_menu.keyboardColumn;
				DrawElevatedSurface(draw, layout, minimum, maximum, selected);
				std::string label;
				if (row < 4)
					label.assign(1, KeyboardCharacterRows()[row][column]);
				else
				{
					static constexpr std::array<std::string_view, 4> actions{
						"SPACE", "BACKSPACE", "CLEAR", "DONE"};
					label = actions[column];
				}
				DrawTextCentered(draw, {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f},
					WithAlpha(selected ? theme.accent : theme.text, layout.alpha),
					(row == 4 ? 11.0f : 14.0f) * layout.scale, label);
				if (ClickedRect(layout, minimum, maximum))
				{
					s_menu.keyboardRow = row;
					s_menu.keyboardColumn = column;
					ActivateKeyboardKey();
				}
			}
		}
		DrawTextCentered(draw, Point(layout, 814, 589), WithAlpha(theme.muted, layout.alpha),
			10.5f * layout.scale, "A  SELECT    X  BACKSPACE    Y  SPACE    B  CLOSE");
	}

	void MoveLibrarySelection(int delta)
	{
		if (s_menu.library.empty())
			return;
		const int old = s_menu.selectedLibrary;
		const int count = static_cast<int>(s_menu.library.size());
		s_menu.selectedLibrary = (s_menu.selectedLibrary + delta) % count;
		if (s_menu.selectedLibrary < 0)
			s_menu.selectedLibrary += count;
		UpdateSelectedAnimation(old);
		if (old != s_menu.selectedLibrary)
			PulseHaptics();
	}

	void HandleDashboardNavigation(bool previous, bool next, bool up, bool down)
	{
		if (s_menu.focus == FocusArea::Header)
		{
			if (previous)
			{
				s_menu.headerSelection = (s_menu.headerSelection + 3) % 4;
				PulseHaptics();
			}
			if (next)
			{
				s_menu.headerSelection = (s_menu.headerSelection + 1) % 4;
				PulseHaptics();
			}
			if (down)
			{
				s_menu.focus = FocusArea::Portal;
				PulseHaptics();
			}
			return;
		}
		if (s_menu.focus == FocusArea::Portal)
		{
			const int portalCount = std::max(1, static_cast<int>(BuildPortalRows().size()));
			if (previous)
			{
				const int old = s_menu.selectedPortalRow;
				s_menu.selectedPortalRow = (s_menu.selectedPortalRow + portalCount - 1) % portalCount;
				if (old != s_menu.selectedPortalRow) PulseHaptics();
			}
			if (next)
			{
				const int old = s_menu.selectedPortalRow;
				s_menu.selectedPortalRow = (s_menu.selectedPortalRow + 1) % portalCount;
				if (old != s_menu.selectedPortalRow) PulseHaptics();
			}
			if (up)
			{
				s_menu.focus = FocusArea::Header;
				s_menu.headerSelection = GetConfig().emulated_usb_devices.emulate_skylander_portal ? 0 : 1;
				PulseHaptics();
			}
			if (down)
			{
				s_menu.focus = FocusArea::Search;
				PulseHaptics();
			}
			return;
		}
		if (s_menu.focus == FocusArea::Search)
		{
			if (up)
			{
				s_menu.focus = FocusArea::Portal;
				PulseHaptics();
			}
			if (down)
			{
				s_menu.focus = FocusArea::ElementFilter;
				s_menu.selectedElementFilter = std::clamp<sint32>(
					GetConfig().emulated_usb_devices.skylander_element_filter.GetValue(), 0, 11);
				PulseHaptics();
			}
			return;
		}
		if (s_menu.focus == FocusArea::ElementFilter)
		{
			if (previous || next)
			{
				s_menu.selectedElementFilter = WrapChoice(
					s_menu.selectedElementFilter + (next ? 1 : -1), 12);
				PulseHaptics();
			}
			if (up)
			{
				s_menu.focus = FocusArea::Search;
				PulseHaptics();
			}
			if (down)
			{
				s_menu.focus = FocusArea::Library;
				PulseHaptics();
			}
			return;
		}

		if (up)
		{
			s_menu.focus = FocusArea::ElementFilter;
			s_menu.selectedElementFilter = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_element_filter.GetValue(), 0, 11);
			PulseHaptics();
		}
		if (previous)
			MoveLibrarySelection(-1);
		if (next)
			MoveLibrarySelection(1);
	}

	bool NavigationPulse(bool down, bool wasDown, size_t direction)
	{
		if (!down)
		{
			s_menu.nextNavigationRepeat[direction] = 0.0;
			return false;
		}
		const double now = ImGui::GetTime();
		if (!wasDown)
		{
			s_menu.nextNavigationRepeat[direction] = now + 0.36;
			return true;
		}
		if (now < s_menu.nextNavigationRepeat[direction])
			return false;
		s_menu.nextNavigationRepeat[direction] = now + 0.095;
		return true;
	}

	void HandleControllerInput()
	{
		struct ControllerSnapshot
		{
			bool connected{};
			bool open{};
			bool back{};
			bool up{};
			bool down{};
			bool left{};
			bool right{};
			bool accept{};
			bool remove{};
			bool details{};
			bool sort{};
			bool favorite{};

			bool AnyControlDown() const
			{
				return open || back || up || down || left || right || accept || remove || details || sort ||
					favorite;
			}
		};
		std::array<ControllerSnapshot, InputManager::kMaxController> snapshots{};
		bool anyControlDown = false;
		for (int i = 0; i < InputManager::kMaxController; ++i)
		{
			const auto controller = InputManager::instance().get_controller(i);
			if (!controller)
				continue;
			auto& snapshot = snapshots[i];
			snapshot.connected = true;
			snapshot.open = controller->is_quick_menu_down();
			snapshot.back = controller->is_b_down();
			snapshot.up = controller->is_up_down();
			snapshot.down = controller->is_down_down();
			snapshot.left = controller->is_left_down();
			snapshot.right = controller->is_right_down();
			snapshot.accept = controller->is_a_down();
			snapshot.remove = controller->is_x_down();
			snapshot.details = controller->is_y_down();
			snapshot.sort = controller->is_l_down();
			snapshot.favorite = controller->is_r_down();
			const glm::vec2 axis = controller->get_axis();
			snapshot.left |= axis.x < -0.55f;
			snapshot.right |= axis.x > 0.55f;
			snapshot.up |= axis.y > 0.55f;
			snapshot.down |= axis.y < -0.55f;
			anyControlDown |= snapshot.AnyControlDown();
		}

		if (s_menu.ownerController >= 0 && !snapshots[s_menu.ownerController].connected)
		{
			StopMenuHaptics();
			s_menu.ownerController = -1;
		}
		if (s_menu.ownerController < 0)
		{
			for (int i = 0; i < InputManager::kMaxController; ++i)
			{
				if (snapshots[i].open || (s_menu.requestedOpen && snapshots[i].AnyControlDown()))
				{
					s_menu.ownerController = i;
					break;
				}
			}
		}

		bool backDown = false;
		bool upDown = false;
		bool downDown = false;
		bool leftDown = false;
		bool rightDown = false;
		bool acceptDown = false;
		bool removeDown = false;
		bool detailsDown = false;
		bool sortDown = false;
		bool favoriteDown = false;
		if (s_menu.ownerController >= 0)
		{
			const auto& snapshot = snapshots[s_menu.ownerController];
			backDown = snapshot.back;
			upDown = snapshot.up;
			downDown = snapshot.down;
			leftDown = snapshot.left;
			rightDown = snapshot.right;
			acceptDown = snapshot.accept;
			removeDown = snapshot.remove;
			detailsDown = snapshot.details;
			sortDown = snapshot.sort;
			favoriteDown = snapshot.favorite;
		}
		s_menu.controlsNeutral = s_menu.ownerController >= 0 ?
			!snapshots[s_menu.ownerController].AnyControlDown() : !anyControlDown;

		if (s_menu.openChordBlockedUntilRelease && !favoriteDown && !downDown)
			s_menu.openChordBlockedUntilRelease = false;

		if (s_menu.requestedOpen && !s_menu.openChordBlockedUntilRelease)
		{
			const bool previous = NavigationPulse(leftDown, s_menu.leftWasDown, 0);
			const bool next = NavigationPulse(rightDown, s_menu.rightWasDown, 1);
			const bool up = NavigationPulse(upDown, s_menu.upWasDown, 2);
			const bool down = NavigationPulse(downDown, s_menu.downWasDown, 3);
			if (s_menu.searchTarget != SearchTarget::None)
			{
				if (backDown && !s_menu.backWasDown)
					CloseSearchKeyboard();
				else
				{
					if (previous) MoveKeyboardHorizontal(-1);
					if (next) MoveKeyboardHorizontal(1);
					if (up) MoveKeyboardVertical(-1);
					if (down) MoveKeyboardVertical(1);
					if (acceptDown && !s_menu.acceptWasDown) ActivateKeyboardKey();
					if (removeDown && !s_menu.removeWasDown) BackspaceSearchText();
					if (detailsDown && !s_menu.detailsWasDown) AppendSearchCharacter(' ');
				}
			}
			else if (backDown && !s_menu.backWasDown)
			{
				if (s_menu.page == RiftPage::Dashboard)
					SetMenuOpen(false, false);
				else if (s_menu.page == RiftPage::DeleteConfirm)
					s_menu.page = RiftPage::Details;
				else
				{
					s_menu.page = RiftPage::Dashboard;
					s_menu.focus = FocusArea::Library;
				}
				PulseHaptics(UiSound::Back);
			}
			else if (s_menu.page == RiftPage::Dashboard)
			{
				HandleDashboardNavigation(previous, next, up, down);
				if (acceptDown && !s_menu.acceptWasDown)
				{
					if (s_menu.focus == FocusArea::Header)
						ActivateHeaderSelection();
					else if (s_menu.focus == FocusArea::ElementFilter)
						ApplySelectedElementFilter();
					else if (s_menu.focus == FocusArea::Search)
						OpenSearchKeyboard(SearchTarget::Collection);
					else if (s_menu.focus == FocusArea::Library && !s_menu.library.empty())
						BeginPlacement(s_menu.library[s_menu.selectedLibrary]);
					else if (s_menu.focus == FocusArea::Portal)
						OpenPortalDetails();
				}
				if (removeDown && !s_menu.removeWasDown)
				{
					if (s_menu.focus == FocusArea::Search)
					{
						s_menu.searchText.fill(0);
						RebuildLibrary();
						PulseHaptics();
					}
					else
						RemoveFocusedFigure();
				}
				if (detailsDown && !s_menu.detailsWasDown)
				{
					if (s_menu.focus == FocusArea::Portal)
						OpenPortalDetails();
					else if (s_menu.focus == FocusArea::Library)
						OpenLibraryDetails();
				}
				if (sortDown && !s_menu.sortWasDown)
					OpenLibraryOrder();
				if (favoriteDown && !s_menu.favoriteWasDown && s_menu.focus == FocusArea::Library)
					ToggleFavorite();
			}
			else if (s_menu.page == RiftPage::Details)
			{
				if (acceptDown && !s_menu.acceptWasDown && s_menu.details.fromLibrary)
				{
					const auto* definition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant);
					const skylander_ui::CollectionFigure figure{
						s_menu.details.filePath, s_menu.details.id, s_menu.details.variant,
						s_menu.details.name, s_menu.details.imagePath,
						definition ? definition->element : skylander_ui::FigureElement::Unknown,
						definition ? definition->type : skylander_ui::FigureType::Unknown};
					BeginPlacement(figure);
				}
				if (removeDown && !s_menu.removeWasDown)
				{
					if (s_menu.details.fromLibrary)
						RequestDeleteDetailFigure();
					else if (s_menu.details.actualSlot >= 0)
						RemoveActualSlot(s_menu.details.actualSlot);
				}
				if (favoriteDown && !s_menu.favoriteWasDown && s_menu.details.fromLibrary)
					ToggleFavoritePath(s_menu.details.filePath, s_menu.details.name);
			}
			else if (s_menu.page == RiftPage::DeleteConfirm)
			{
				if (acceptDown && !s_menu.acceptWasDown)
					DeleteDetailFigure();
			}
			else if (s_menu.page == RiftPage::Forge)
			{
				const auto definitions = BuildForgeDefinitions();
				if (s_menu.forgeToolbarFocused)
				{
					if (previous || next)
					{
						s_menu.forgeToolbarSelection = WrapChoice(
							s_menu.forgeToolbarSelection + (next ? 1 : -1), 4);
						PulseHaptics();
					}
					if (down)
					{
						s_menu.forgeToolbarFocused = false;
						PulseHaptics();
					}
					if (acceptDown && !s_menu.acceptWasDown)
					{
						switch (s_menu.forgeToolbarSelection)
						{
						case 0: OpenSearchKeyboard(SearchTarget::Create); break;
						case 1: CycleForgeSort(); break;
						case 2: CycleForgeTypeFilter(); break;
						default: CycleForgeElementFilter(); break;
						}
					}
				}
				else
				{
					if (up && (definitions.empty() ||
						(s_menu.selectedDefinition % kLibraryPageSize) < kLibraryColumns))
					{
						s_menu.forgeToolbarFocused = true;
						PulseHaptics();
					}
					else if (!definitions.empty() && (previous || next || up || down))
					{
						const int delta = next ? 1 : previous ? -1 : down ? kLibraryColumns : -kLibraryColumns;
						const int old = s_menu.selectedDefinition;
						s_menu.selectedDefinition = std::clamp(s_menu.selectedDefinition + delta, 0,
							static_cast<int>(definitions.size()) - 1);
						if (old != s_menu.selectedDefinition) PulseHaptics();
					}
					if (acceptDown && !s_menu.acceptWasDown)
						ForgeSelectedFigure();
				}
				if (removeDown && !s_menu.removeWasDown)
					ResetForgeFilters();
				if (sortDown && !s_menu.sortWasDown)
					CycleForgeSort();
				if (favoriteDown && !s_menu.favoriteWasDown)
					CycleForgeTypeFilter();
				if (detailsDown && !s_menu.detailsWasDown)
					CycleForgeElementFilter();
			}
			else if (s_menu.page == RiftPage::Options)
			{
				if (up || down)
				{
					const int column = s_menu.selectedOption / 8;
					const int count = column == 0 ? 8 : kOptionCount - 8;
					const int row = WrapChoice(s_menu.selectedOption % 8 + (down ? 1 : -1), count);
					s_menu.selectedOption = column * 8 + row;
					PulseHaptics();
				}
				if (previous || next)
				{
					const int old = s_menu.selectedOption;
					const int row = s_menu.selectedOption % 8;
					const int column = next ? 1 : 0;
					s_menu.selectedOption = column * 8 + std::min(row, column == 0 ? 7 : kOptionCount - 9);
					if (old != s_menu.selectedOption) PulseHaptics();
				}
				if (acceptDown && !s_menu.acceptWasDown)
					AdjustOption(s_menu.selectedOption, 1);
			}
			else if (s_menu.page == RiftPage::LibraryOrder)
			{
				if (up || down)
				{
					s_menu.selectedLibraryOption = (s_menu.selectedLibraryOption + (down ? 1 : 4)) % 5;
					PulseHaptics();
				}
				if (acceptDown && !s_menu.acceptWasDown)
					AdjustLibraryOption(s_menu.selectedLibraryOption, 1);
			}
		}

		s_menu.backWasDown = backDown;
		s_menu.upWasDown = upDown;
		s_menu.downWasDown = downDown;
		s_menu.leftWasDown = leftDown;
		s_menu.rightWasDown = rightDown;
		s_menu.acceptWasDown = acceptDown;
		s_menu.removeWasDown = removeDown;
		s_menu.detailsWasDown = detailsDown;
		s_menu.sortWasDown = sortDown;
		s_menu.favoriteWasDown = favoriteDown;
	}

	void RenderQuickMenu(bool padView)
	{
		if (s_resetRequested.exchange(false, std::memory_order_acq_rel))
			ResetMenuInteraction();
		size_t controllerOwner = 0;
		const uint32 controllerToggleRequests = EmulatedController::ConsumeMenuToggleRequests(controllerOwner);
		if (controllerToggleRequests & 1)
		{
			s_menu.ownerController = static_cast<int>(std::min(controllerOwner,
				InputManager::kMaxController - 1));
			SetMenuOpen(!s_menu.requestedOpen);
		}
		const uint32 toggleRequests = s_toggleRequests.exchange(0, std::memory_order_acq_rel);
		if (toggleRequests & 1)
			SetMenuOpen(!s_menu.requestedOpen);
		if (padView)
			return;
		ReloadCatalogIfNeeded();
		if (!s_menu.requestedOpen && s_menu.visibility < 0.01f)
			PumpArtworkTextureUploads();
		HandleControllerInput();
		UpdateHaptics();
		ImGui_SetGamepadNavigationEnabled(!s_menu.requestedOpen);

		const float targetVisibility = s_menu.requestedOpen ? 1.0f : 0.0f;
		const int motion = MotionLevel();
		if (motion == 0)
			s_menu.visibility = targetVisibility;
		else
		{
			const float speed = motion == 1 ? (s_menu.requestedOpen ? 18.0f : 24.0f) :
				(s_menu.requestedOpen ? 6.4f : 10.5f);
			s_menu.visibility = SmoothTowards(s_menu.visibility, targetVisibility, speed);
		}
		if (s_menu.inputCaptureLatched)
		{
			EmulatedController::SetRiftInputCaptured(true);
			if (!s_menu.requestedOpen && s_menu.visibility < 0.02f && s_menu.controlsNeutral)
			{
				if (++s_menu.neutralInputFrames >= 2)
				{
					StopMenuHaptics();
					s_menu.inputCaptureLatched = false;
					s_menu.ownerController = -1;
					EmulatedController::SetRiftInputCaptured(false);
				}
			}
			else
				s_menu.neutralInputFrames = 0;
		}
		s_menu.selectionFlash = motion == 0 ? 0.0f : SmoothTowards(s_menu.selectionFlash, 0.0f, motion == 1 ? 15.0f : 8.5f);
		s_menu.portalModeFlash = motion == 0 ? 0.0f : SmoothTowards(s_menu.portalModeFlash, 0.0f, motion == 1 ? 10.0f : 6.0f);
		if (motion == 0)
		{
			s_menu.placementAnimation = 1.0f;
			s_menu.placementImpact = 0.0f;
		}
		else if (s_menu.placementAnimation < 1.0f)
		{
			s_menu.placementAnimation = std::min(1.0f,
				s_menu.placementAnimation + ImGui::GetIO().DeltaTime * (motion == 1 ? 6.5f : 2.2f));
			if (s_menu.placementAnimation >= 1.0f)
				s_menu.placementImpact = 1.0f;
		}
		else if (s_menu.placementImpact > 0.0f)
			s_menu.placementImpact = std::max(0.0f,
				s_menu.placementImpact - ImGui::GetIO().DeltaTime * 2.7f);
		if (s_menu.toastLife > 0.0f)
			s_menu.toastLife = std::max(0.0f, s_menu.toastLife - ImGui::GetIO().DeltaTime);
		if (s_menu.renderedPage != s_menu.page)
		{
			s_menu.renderedPage = s_menu.page;
			s_menu.pageVisibility = MotionEnabled() ? 0.0f : 1.0f;
		}
		s_menu.pageVisibility = MotionEnabled() ? SmoothTowards(s_menu.pageVisibility, 1.0f,
			MotionLevel() == 2 ? 8.0f : 15.0f) : 1.0f;
		if (s_menu.visibility < 0.01f && !s_menu.requestedOpen)
			return;

		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float baseScale = std::max(0.55f, std::min(display.x / 1280.0f, display.y / 720.0f));
		const float easedVisibility = EaseOutCubic(s_menu.visibility);
		const float zoom = motion == 2 ? 0.975f + easedVisibility * 0.025f :
			(motion == 1 ? 0.995f + easedVisibility * 0.005f : 1.0f);
		RiftLayout layout;
		layout.scale = baseScale * zoom;
		layout.origin = {display.x - 1280.0f * layout.scale, (display.y - 720.0f * layout.scale) * 0.5f};
		layout.alpha = easedVisibility;
		layout.slideX = (1.0f - layout.alpha) * (motion == 2 ? 82.0f : motion == 1 ? 12.0f : 0.0f);

		ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
		ImGui::SetNextWindowSize(display, ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
		if (ImGui::Begin("Rift of Power", nullptr, flags))
		{
			auto* draw = ImGui::GetWindowDrawList();
			DrawBackdrop(draw, layout, display);
			RiftLayout pageLayout = layout;
			const float pageEase = SmoothStep(s_menu.pageVisibility);
			pageLayout.slideX += (1.0f - pageEase) * (MotionLevel() == 2 ? 22.0f : 8.0f);
			switch (s_menu.page)
			{
			case RiftPage::Dashboard: DrawDashboard(draw, pageLayout); break;
			case RiftPage::Details: DrawDetailsPage(draw, pageLayout); break;
			case RiftPage::Forge: DrawForgePage(draw, pageLayout); break;
			case RiftPage::Options: DrawOptionsPage(draw, pageLayout); break;
			case RiftPage::LibraryOrder: DrawLibraryOrderPage(draw, pageLayout); break;
			case RiftPage::DeleteConfirm: DrawDeleteConfirmPage(draw, pageLayout); break;
			}
			DrawPlacementAnimation(ImGui::GetForegroundDrawList(), layout);
			DrawPlacementImpact(ImGui::GetForegroundDrawList(), layout);
			DrawToast(ImGui::GetForegroundDrawList(), layout);
			DrawFooter(ImGui::GetForegroundDrawList(), pageLayout);
			DrawOnScreenKeyboard(ImGui::GetForegroundDrawList(), pageLayout);
		}
		ImGui::End();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}
}

void SkylanderQuickMenu_Init()
{
	ResetMenuInteraction();
	LatteOverlay_setSkylanderQuickMenuCallback(RenderQuickMenu);
}

void SkylanderQuickMenu_Toggle()
{
	s_toggleRequests.fetch_add(1, std::memory_order_release);
}

void SkylanderQuickMenu_Reset()
{
	s_resetRequested.store(true, std::memory_order_release);
	EmulatedController::SetRiftInputCaptured(false);
}

bool SkylanderQuickMenu_ConsumeUpdateCheckRequest()
{
	return s_updateCheckRequested.exchange(false, std::memory_order_acq_rel);
}
