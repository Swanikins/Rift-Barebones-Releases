#include "SkylanderQuickMenu.h"

#include <imgui.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <wx/image.h>
#include <wx/log.h>
#include <condition_variable>
#include <deque>
#include <sstream>
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
#include "resource/IconsFontAwesome5.h"

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
	constexpr int kOptionCount = 18;
	constexpr int kLibraryOptionCount = 11;
	constexpr int kMaximumCascadeRows = 6;

	enum class RiftPage
	{
		Dashboard,
		Details,
		Forge,
		Options,
		ColorEditor,
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
		Create,
		Label
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
		Game,
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
		bool startWasDown{};
		bool hapticActive{};
		bool mouseNavigationActive{true};
		bool mousePositionInitialized{};
		int neutralInputFrames{};
		int ownerController{-1};
		float visibility{};
		float selectionFlash{};
		float portalModeFlash{};
		float placementAnimation{1.0f};
		float placementImpact{};
		float toastLife{};
		float pageVisibility{1.0f};
		float cascadeViewport{-1.0f};
		int selectedLibrary{};
		int selectedElementFilter{};
		int selectedPortalRow{};
		int selectedDefinition{};
		int forgeSortMode{};
		int forgeTypeFilter{};
		int forgeElementFilter{};
		int forgeGameFilter{};
		int forgeToolbarSelection{};
		int keyboardRow{};
		int keyboardColumn{};
		int selectedOption{};
		int colorEditorTarget{};
		int selectedLibraryOption{};
		int selectedCascadeRow{};
		int selectedCascadeEditorRow{};
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
		std::array<float, 5> headerFocusAnimations{};
		std::array<float, 5> forgeToolbarFocusAnimations{};
		std::array<std::array<float, 10>, 5> keyboardFocusAnimations{};
		std::array<float, kPortalCapacity> portalFocusAnimations{};
		std::array<float, kOptionCount> optionFocusAnimations{};
		std::array<std::array<float, 3>, 2> colorEditorHsv{};
		std::array<ImVec2, 2> colorEditorCursor{};
		bool colorEditorDirty{};
		std::array<float, kLibraryOptionCount> libraryOptionFocusAnimations{};
		std::array<int, kMaximumCascadeRows> cascadeRowPositions{};
		std::array<float, kMaximumCascadeRows> cascadeRowFocusAnimations{};
		std::array<double, 4> nextNavigationRepeat{};
		fs::path placementArtwork;
		ImU32 placementAccent{IM_COL32(71, 207, 255, 255)};
		std::string toast;
		std::array<char, 96> searchText{};
		std::array<char, 96> forgeSearchText{};
		std::array<char, 96> labelText{};
		float keyboardVisibility{};
		ImVec2 lastMousePosition{};
		std::chrono::steady_clock::time_point hapticStop{};
		skylander_ui::SkylanderCatalog catalog;
		std::vector<skylander_ui::CollectionFigure> library;
		std::unordered_set<std::string> favorites;
		std::unordered_map<std::string, std::string> labels;
		DetailState details;
	} s_menu;

	std::atomic_uint32_t s_toggleRequests{};
	std::atomic_bool s_resetRequested{};
	std::atomic_bool s_updateCheckRequested{};
	std::atomic_int s_textInputTarget{};
	std::mutex s_textInputMutex;
	std::deque<std::pair<int, unsigned int>> s_textInputQueue;
	std::unordered_map<std::string, ArtworkTexture> s_artworkTextures;
	std::unordered_map<std::string, float> s_cardFocusAmounts;
	std::unordered_map<std::string, float> s_cardCarouselOffsets;
	std::unordered_map<std::string, float> s_cascadeCardFocusAmounts;
	std::unordered_map<std::string, float> s_cascadeCardOffsets;
	std::unordered_map<std::string, float> s_forgeFocusAmounts;
	uint64 s_libraryGeneration{};

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
		return std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_motion_level.GetValue(), 0, 4);
	}

	float MotionValue(float reduced, float smooth, float floating, float cinematic)
	{
		switch (MotionLevel())
		{
		case 1: return reduced;
		case 2: return smooth;
		case 3: return floating;
		case 4: return cinematic;
		default: return 0.0f;
		}
	}

	bool AmbientMotionEnabled()
	{
		return MotionLevel() >= 2;
	}

	float AmbientTempo()
	{
		return MotionValue(0.0f, 1.0f, 0.72f, 0.52f);
	}

	float AmbientStrength()
	{
		return MotionValue(0.0f, 0.72f, 1.0f, 1.24f);
	}

	float CarouselResponse()
	{
		return MotionValue(16.0f, 7.0f, 5.2f, 4.0f);
	}

	int BloomLevel()
	{
		return std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_bloom_level.GetValue(), 0, 2);
	}

	int CardEffect()
	{
		return std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_card_effect.GetValue(), 0, 3);
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
		const float speed = fadingOut ? MotionValue(18.0f, 9.0f, 7.0f, 5.5f) :
			MotionValue(14.0f, 7.0f, 5.2f, 4.0f);
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

	ImU32 ConfigToColor(uint32 rgb)
	{
		return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
	}

	uint32 ColorToConfig(ImU32 color)
	{
		return ((color & 0xFF) << 16) | (color & 0xFF00) | ((color >> 16) & 0xFF);
	}

	ImU32 HsvColor(float hue, float saturation, float value)
	{
		float red = 0.0f;
		float green = 0.0f;
		float blue = 0.0f;
		ImGui::ColorConvertHSVtoRGB(hue, saturation, value, red, green, blue);
		return IM_COL32(
			static_cast<int>(red * 255.0f + 0.5f),
			static_cast<int>(green * 255.0f + 0.5f),
			static_cast<int>(blue * 255.0f + 0.5f), 255);
	}

	void ColorToHsv(ImU32 color, std::array<float, 3>& hsv)
	{
		ImGui::ColorConvertRGBtoHSV(
			static_cast<float>(color & 0xFF) / 255.0f,
			static_cast<float>((color >> 8) & 0xFF) / 255.0f,
			static_cast<float>((color >> 16) & 0xFF) / 255.0f,
			hsv[0], hsv[1], hsv[2]);
	}

	ImU32 ThemeBaseColor(int theme)
	{
		switch (theme)
		{
		case 1: return IM_COL32(91, 50, 130, 255);
		case 2: return IM_COL32(119, 52, 59, 255);
		case 3: return IM_COL32(45, 113, 76, 255);
		case 4: return IM_COL32(114, 53, 123, 255);
		case 5: return ConfigToColor(GetConfig().emulated_usb_devices.skylander_custom_theme.GetValue());
		default: return IM_COL32(47, 124, 160, 255);
		}
	}

	ImU32 ThemeAccentColor(int theme, int accent)
	{
		switch (accent)
		{
		case 1: return IM_COL32(34, 206, 255, 255);
		case 2: return IM_COL32(181, 78, 255, 255);
		case 3: return IM_COL32(255, 181, 24, 255);
		case 4: return IM_COL32(43, 232, 122, 255);
		case 5: return IM_COL32(255, 72, 56, 255);
		case 6: return ConfigToColor(GetConfig().emulated_usb_devices.skylander_custom_accent.GetValue());
		default: break;
		}
		switch (theme)
		{
		case 1: return IM_COL32(174, 91, 255, 255);
		case 2: return IM_COL32(255, 188, 57, 255);
		case 3: return IM_COL32(91, 224, 113, 255);
		case 4: return IM_COL32(240, 76, 177, 255);
		case 5: return Lighten(ThemeBaseColor(5), 0.48f);
		default: return IM_COL32(79, 211, 255, 255);
		}
	}

	ImU32 ContrastingTextColor(ImU32 color)
	{
		const int red = color & 0xFF;
		const int green = (color >> 8) & 0xFF;
		const int blue = (color >> 16) & 0xFF;
		return red * 299 + green * 587 + blue * 114 > 154000 ?
			IM_COL32(8, 14, 24, 255) : IM_COL32(255, 255, 255, 255);
	}

	void DrawGradientQuad(ImDrawList* draw, const ImVec2& point0, const ImVec2& point1,
		const ImVec2& point2, const ImVec2& point3, ImU32 color0, ImU32 color1,
		ImU32 color2, ImU32 color3)
	{
		const ImDrawIdx index = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		draw->PrimReserve(6, 4);
		draw->PrimWriteIdx(index);
		draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1));
		draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
		draw->PrimWriteIdx(index);
		draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
		draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 3));
		draw->PrimWriteVtx(point0, uv, color0);
		draw->PrimWriteVtx(point1, uv, color1);
		draw->PrimWriteVtx(point2, uv, color2);
		draw->PrimWriteVtx(point3, uv, color3);
	}

	struct ThemePalette
	{
		ImU32 accent, secondary, background, surface, selectedSurface, text, muted, border;
	};

	ThemePalette CurrentTheme()
	{
		ThemePalette theme{};
		switch (std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_theme.GetValue(), 0, 5))
		{
		case 1: theme = {IM_COL32(174, 91, 255, 255), IM_COL32(69, 213, 255, 255), IM_COL32(31, 17, 52, 255), IM_COL32(54, 31, 80, 255), IM_COL32(91, 50, 130, 255), IM_COL32(250, 246, 255, 255), IM_COL32(210, 181, 232, 255), IM_COL32(143, 91, 190, 255)}; break;
		case 2: theme = {IM_COL32(255, 188, 57, 255), IM_COL32(238, 64, 74, 255), IM_COL32(47, 22, 36, 255), IM_COL32(78, 37, 50, 255), IM_COL32(119, 52, 59, 255), IM_COL32(255, 247, 226, 255), IM_COL32(247, 190, 143, 255), IM_COL32(219, 91, 72, 255)}; break;
		case 3: theme = {IM_COL32(91, 224, 113, 255), IM_COL32(52, 205, 230, 255), IM_COL32(16, 48, 43, 255), IM_COL32(28, 75, 60, 255), IM_COL32(45, 113, 76, 255), IM_COL32(245, 255, 235, 255), IM_COL32(177, 226, 183, 255), IM_COL32(69, 165, 102, 255)}; break;
		case 4: theme = {IM_COL32(240, 76, 177, 255), IM_COL32(145, 91, 255, 255), IM_COL32(43, 21, 61, 255), IM_COL32(73, 35, 88, 255), IM_COL32(114, 53, 123, 255), IM_COL32(255, 242, 252, 255), IM_COL32(231, 176, 220, 255), IM_COL32(174, 85, 170, 255)}; break;
		case 5:
		{
			const ImU32 base = ThemeBaseColor(5);
			std::array<float, 3> hsv{};
			ColorToHsv(base, hsv);
			const ImU32 secondary = HsvColor(std::fmod(hsv[0] + 0.10f, 1.0f),
				std::clamp(hsv[1] * 0.82f + 0.12f, 0.0f, 1.0f),
				std::clamp(hsv[2] * 1.18f + 0.08f, 0.0f, 1.0f));
			theme = {
				Lighten(base, 0.48f), secondary,
				BlendColor(base, IM_COL32(3, 7, 13, 255), 0.78f),
				BlendColor(base, IM_COL32(5, 10, 18, 255), 0.52f),
				base,
				BlendColor(IM_COL32(255, 255, 255, 255), base, 0.06f),
				Lighten(base, 0.58f), Lighten(base, 0.24f)};
			break;
		}
		default: theme = {IM_COL32(79, 211, 255, 255), IM_COL32(239, 64, 75, 255), IM_COL32(16, 43, 70, 255), IM_COL32(28, 70, 101, 255), IM_COL32(47, 124, 160, 255), IM_COL32(250, 247, 230, 255), IM_COL32(174, 216, 232, 255), IM_COL32(62, 158, 199, 255)}; break;
		}
		const int themeIndex = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_theme.GetValue(), 0, 5);
		const int accentIndex = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_accent.GetValue(), 0, 6);
		theme.accent = ThemeAccentColor(themeIndex, accentIndex);
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

	void DrawTitleText(ImDrawList* draw, const RiftLayout& layout, ImVec2 position, float size,
		std::string_view text, float accentMix = 0.14f)
	{
		const auto theme = CurrentTheme();
		DrawText(draw, {position.x + DrawerWidth(layout, 1.5f), position.y + 2.0f * layout.scale},
			WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * 0.58f), size, text);
		DrawText(draw, position, WithAlpha(BlendColor(theme.text, theme.accent, accentMix), layout.alpha),
			size, text);
	}

	void DrawGlow(ImDrawList* draw, ImVec2 center, float radius, ImU32 color, float alpha)
	{
		const int bloom = BloomLevel();
		if (bloom == 0 || alpha <= 0.0f)
			return;
		const float strength = bloom == 2 ? 1.08f : 0.88f;
		const int ringCount = bloom == 2 ? 12 : 8;
		const int segments = bloom == 2 ? 80 : 64;
		for (int ring = ringCount; ring >= 1; --ring)
		{
			const float fraction = static_cast<float>(ring) / ringCount;
			const float falloff = 1.0f - fraction;
			const float radiusScale = (0.42f + fraction * 0.68f) * (bloom == 2 ? 1.05f : 1.0f);
			const float layerOpacity = 0.006f + falloff * falloff * 0.019f;
			draw->AddCircleFilled(center, radius * radiusScale,
				WithAlpha(color, alpha * strength * layerOpacity), segments);
		}
	}

	void DrawCardAura(ImDrawList* draw, ImVec2 center, float radius, ImU32 color, float intensity, float alpha)
	{
		if (intensity < 0.01f || BloomLevel() == 0)
			return;
		const float time = static_cast<float>(ImGui::GetTime());
		const float pulse = AmbientMotionEnabled() ?
			0.5f + 0.5f * std::sin(time * 1.25f * AmbientTempo()) : 0.5f;
		draw->AddCircle(center, radius * (0.90f + pulse * 0.07f),
			WithAlpha(color, alpha * intensity * (0.12f + pulse * 0.08f)), 48,
			(1.0f + pulse * 0.8f) * intensity);
		const int particles = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_particle_level.GetValue(), 0, 3);
		if (!AmbientMotionEnabled() || particles == 0)
			return;
		const int sparkCount = particles == 1 ? 3 : particles == 2 ? 5 : 8;
		for (int spark = 0; spark < sparkCount; ++spark)
		{
			const float angle = time * (0.24f + spark * 0.014f) * AmbientTempo() +
				spark * (2.0f * kPi / sparkCount);
			const float orbit = radius * (0.78f + 0.10f *
				std::sin(time * 0.82f * AmbientTempo() + spark));
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
		const float surfaceAlpha = (strong ? 0.82f : 0.68f) * (0.16f + opacity * 0.84f);
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

	void DrawAnimatedControlSurface(ImDrawList* draw, const RiftLayout& layout, ImVec2 minimum, ImVec2 maximum,
		float focus, bool active, ImU32 accent)
	{
		const auto theme = CurrentTheme();
		focus = Clamp01(focus);
		const float emphasis = std::max(focus, active ? 0.34f : 0.0f);
		const float radius = CornerRadius(5.0f) * layout.scale;
		if (focus > 0.01f)
		{
			draw->AddRectFilled({minimum.x + DrawerWidth(layout, 4.0f), minimum.y + 6.0f * layout.scale},
				{maximum.x + DrawerWidth(layout, 4.0f), maximum.y + 6.0f * layout.scale},
				WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * focus * 0.30f), radius);
		}
		const ImU32 restingSurface = active ? BlendColor(theme.surface, accent, 0.10f) : theme.surface;
		const ImU32 fill = BlendColor(restingSurface, theme.selectedSurface, focus * 0.82f);
		draw->AddRectFilled(minimum, maximum,
			WithAlpha(fill, layout.alpha * (0.56f + emphasis * 0.30f)), radius);
		draw->AddRect(minimum, maximum,
			WithAlpha(emphasis > 0.0f ? accent : theme.border,
				layout.alpha * (0.24f + emphasis * 0.60f)),
			radius, 0, (1.0f + focus * 0.7f) * layout.scale);
	}

	void DrawSettingsChoiceSurface(ImDrawList* draw, const RiftLayout& layout, ImVec2 minimum,
		ImVec2 maximum, float focus)
	{
		const auto theme = CurrentTheme();
		focus = Clamp01(focus);
		const float radius = CornerRadius(4.0f) * layout.scale;
		if (focus > 0.01f)
			draw->AddRectFilled({minimum.x + 3.0f * layout.scale, minimum.y + 4.0f * layout.scale},
				{maximum.x + 3.0f * layout.scale, maximum.y + 4.0f * layout.scale},
				WithAlpha(IM_COL32(0, 2, 10, 255), layout.alpha * focus * 0.28f), radius);
		const ImU32 fill = BlendColor(theme.surface, theme.selectedSurface, focus * 0.34f);
		draw->AddRectFilled(minimum, maximum,
			WithAlpha(fill, layout.alpha * (0.52f + focus * 0.18f)), radius);
		draw->AddRect(minimum, maximum,
			WithAlpha(BlendColor(theme.border, theme.accent, focus),
				layout.alpha * (0.20f + focus * 0.56f)), radius, 0,
			(1.0f + focus * 0.8f) * layout.scale);
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

	void RotateDrawVertices(ImDrawList* draw, int firstVertex, ImVec2 center, float rotation)
	{
		if (std::abs(rotation) <= 0.0001f)
			return;
		const float cosine = std::cos(rotation);
		const float sine = std::sin(rotation);
		for (int index = firstVertex; index < draw->VtxBuffer.Size; ++index)
		{
			auto& position = draw->VtxBuffer[index].pos;
			const float x = position.x - center.x;
			const float y = position.y - center.y;
			position = {center.x + x * cosine - y * sine, center.y + x * sine + y * cosine};
		}
	}

	void DrawRoundedCardFill(ImDrawList* draw, ImVec2 center, float width, float height,
		float rotation, ImU32 color, float rounding)
	{
		const ImVec2 minimum{center.x - width * 0.5f, center.y - height * 0.5f};
		const ImVec2 maximum{center.x + width * 0.5f, center.y + height * 0.5f};
		const int firstVertex = draw->VtxBuffer.Size;
		draw->AddRectFilled(minimum, maximum, color, rounding);
		RotateDrawVertices(draw, firstVertex, center, rotation);
	}

	void DrawRoundedCardOutline(ImDrawList* draw, ImVec2 center, float width, float height,
		float rotation, ImU32 color, float rounding, float thickness)
	{
		const ImVec2 minimum{center.x - width * 0.5f, center.y - height * 0.5f};
		const ImVec2 maximum{center.x + width * 0.5f, center.y + height * 0.5f};
		const int firstVertex = draw->VtxBuffer.Size;
		draw->AddRect(minimum, maximum, color, rounding, 0, thickness);
		RotateDrawVertices(draw, firstVertex, center, rotation);
	}

	void DrawRoundedCardImage(ImDrawList* draw, const ArtworkTexture& texture, ImVec2 center,
		float width, float height, float rotation, ImU32 color, float rounding)
	{
		const ImVec2 minimum{center.x - width * 0.5f, center.y - height * 0.5f};
		const ImVec2 maximum{center.x + width * 0.5f, center.y + height * 0.5f};
		const int firstVertex = draw->VtxBuffer.Size;
		draw->AddImageRounded(texture.id, minimum, maximum, texture.uvMinimum, texture.uvMaximum,
			color, rounding);
		RotateDrawVertices(draw, firstVertex, center, rotation);
	}

	void DrawCard(ImDrawList* draw, const ArtworkTexture& texture, ImVec2 center, float height,
		float rotation, float focus, float alpha)
	{
		focus = Clamp01(focus);
		const float aspect = texture.id && texture.size.y > 0.0f ? texture.size.x / texture.size.y : 0.64f;
		const float width = height * aspect;
		const auto card = MakeCardQuad(center, width, height, rotation);
		const float rounding = CornerRadius(std::min(width, height) * 0.045f);
		const ImU32 accent = texture.id ? texture.accent : CurrentTheme().accent;
		const int effect = CardEffect();
		const int borderStyle = std::clamp<sint32>(GetConfig().emulated_usb_devices.skylander_card_border.GetValue(), 0, 1);
		for (int layer = 6; layer >= 1; --layer)
		{
			const float offsetX = 1.0f + layer * 0.85f;
			const float offsetY = 2.0f + layer * 1.1f;
			DrawRoundedCardFill(draw, {center.x + offsetX, center.y + offsetY}, width, height,
				rotation, WithAlpha(IM_COL32(0, 0, 0, 255), alpha * (0.034f + focus * 0.022f)), rounding);
		}
		if (focus > 0.001f && effect >= 1)
		{
			const int outer = effect == 1 ? 18 : 14;
			const float bloomStrength = BloomLevel() == 0 ? 0.72f : BloomLevel() == 2 ? 1.22f : 1.0f;
			for (int outline = outer; outline >= 3; outline -= 3)
			{
				const float closeness = 1.0f - static_cast<float>(outline) / outer;
				DrawRoundedCardOutline(draw, center, width, height, rotation,
					WithAlpha(accent, alpha * focus * bloomStrength * (0.032f + closeness * 0.080f)),
					rounding, static_cast<float>(outline));
			}
		}
		if (texture.state == ArtworkTexture::State::Ready && texture.id)
		{
			DrawRoundedCardImage(draw, texture, center, width, height, rotation,
				WithAlpha(IM_COL32_WHITE, alpha), rounding);
			if (focus > 0.001f && effect == 3)
			{
				const float sweep = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.085f, 1.5f) - 0.25f;
				auto edgePoint = [](ImVec2 from, ImVec2 to, float amount) {
					amount = Clamp01(amount);
					return ImVec2{from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount};
				};
				for (int layer = -4; layer <= 4; ++layer)
				{
					const float offset = layer * 0.020f;
					const float top = sweep + offset;
					const float bottom = sweep - 0.18f + offset;
					if (top < -0.04f && bottom < -0.04f || top > 1.04f && bottom > 1.04f)
						continue;
					const float halfWidth = 0.018f;
					const float intensity = 1.0f - std::abs(static_cast<float>(layer)) / 5.0f;
					const ImU32 sheen = BlendColor(accent, IM_COL32_WHITE, 0.66f + intensity * 0.22f);
					draw->AddQuadFilled(
						edgePoint(card.p0, card.p1, top - halfWidth),
						edgePoint(card.p0, card.p1, top + halfWidth),
						edgePoint(card.p3, card.p2, bottom + halfWidth),
						edgePoint(card.p3, card.p2, bottom - halfWidth),
						WithAlpha(sheen, alpha * focus * (0.012f + intensity * 0.032f)));
				}
			}
		}
		else
		{
			DrawRoundedCardFill(draw, center, width, height, rotation,
				WithAlpha(texture.state == ArtworkTexture::State::Loading ?
					IM_COL32(18, 43, 64, 255) : IM_COL32(24, 32, 49, 255), alpha), rounding);
			const bool loading = texture.state == ArtworkTexture::State::Loading;
			DrawTextCentered(draw, {center.x, center.y - height * 0.035f},
				WithAlpha(IM_COL32(166, 207, 230, 255), alpha * 0.82f),
				std::max(18.0f, height * (loading ? 0.12f : 0.18f)), loading ? "..." : "?");
			if (!loading && height >= 100.0f)
				DrawTextCentered(draw, {center.x, center.y + height * 0.17f},
					WithAlpha(IM_COL32(124, 158, 181, 255), alpha * 0.70f),
					std::max(8.0f, height * 0.042f), "NO ART");
		}
		const ImU32 borderColor = BlendColor(borderStyle == 0 ? CurrentTheme().border : accent,
			Lighten(accent, 0.18f), focus);
		const float restingOpacity = borderStyle == 0 ? 0.22f : 0.38f;
		const float restingWidth = borderStyle == 0 ? 1.0f : 1.25f;
		DrawRoundedCardOutline(draw, center, width, height, rotation,
			WithAlpha(borderColor, alpha * (restingOpacity + (0.82f - restingOpacity) * focus)),
			rounding, restingWidth + (2.0f - restingWidth) * focus);
	}

	void DrawFavoriteIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
	{
		DrawTextCentered(draw, center, color, size,
			reinterpret_cast<const char*>(ICON_FA_BOOKMARK));
	}

	void SetToast(std::string value, float seconds = 2.2f)
	{
		s_menu.toast = std::move(value);
		s_menu.toastLife = seconds;
	}

	void SetMenuOpen(bool open, bool pulse = true)
	{
		if (!open && s_menu.page == RiftPage::ColorEditor && s_menu.colorEditorDirty)
		{
			g_config.Save();
			s_menu.colorEditorDirty = false;
		}
		if (s_menu.requestedOpen == open)
			return;
		s_menu.requestedOpen = open;
		s_menu.neutralInputFrames = 0;
		if (open)
		{
			s_menu.page = RiftPage::Dashboard;
			const auto loaded = nsyshid::g_skyportal.GetLoadedSkylanders();
			const bool portalOccupied = std::any_of(loaded.begin(), loaded.end(),
				[](const auto& figure) { return figure.has_value(); });
			s_menu.focus = portalOccupied ? FocusArea::Portal : FocusArea::Library;
			if (portalOccupied)
				s_menu.selectedPortalRow = 0;
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
			s_textInputTarget.store(0, std::memory_order_release);
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
		s_menu.startWasDown = false;
		s_menu.mouseNavigationActive = true;
		s_menu.mousePositionInitialized = false;
		s_menu.ownerController = -1;
		s_menu.nextNavigationRepeat.fill(0.0);
		s_textInputTarget.store(0, std::memory_order_release);
		{
			const std::lock_guard lock(s_textInputMutex);
			s_textInputQueue.clear();
		}
		EmulatedController::SetRiftInputCaptured(false);
	}

	int PortalMode()
	{
		return std::clamp(GetConfig().emulated_usb_devices.skylander_portal_mode.GetValue(), 0, 2);
	}

	bool VirtualPortalAvailable()
	{
		return PortalMode() != 1;
	}

	constexpr int HeaderItemCount()
	{
		return 5;
	}

	void SetPortalMode(int mode)
	{
		mode = std::clamp(mode, 0, 2);
		auto& config = GetConfig().emulated_usb_devices;
		const bool alreadyActive = PortalMode() == mode;
		s_menu.headerSelection = mode;
		if (alreadyActive)
		{
			static constexpr std::array<std::string_view, 3> names{
				"VIRTUAL PORTAL", "PHYSICAL PORTAL", "HYBRID PORTAL"};
			SetToast(std::string(names[mode]) + " IS ALREADY ACTIVE", 1.8f);
			return;
		}
		config.emulate_skylander_portal = mode != 1;
		config.skylander_portal_mode = mode;
		g_config.Save();
		nsyshid::backend::SetSkylanderPortalMode(mode);
		s_menu.portalModeFlash = 1.0f;
		static constexpr std::array<std::string_view, 3> names{
			"VIRTUAL PORTAL ACTIVE", "PHYSICAL PORTAL ACTIVE", "HYBRID PORTAL ACTIVE"};
		SetToast(std::string(names[mode]), 2.4f);
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

	fs::path LabelFilePath()
	{
		const std::string configured = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
		return configured.empty() ? fs::path{} : _utf8ToPath(configured) / ".rift-labels.json";
	}

	std::string LabelKey(const fs::path& path)
	{
		const std::string configured = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
		if (configured.empty())
			return FavoriteKey(path);
		std::error_code error;
		const fs::path relative = fs::relative(path.lexically_normal(), _utf8ToPath(configured).lexically_normal(), error);
		bool valid = !error && !relative.empty() && !relative.is_absolute();
		for (const auto& part : relative)
			valid = valid && part != "..";
		return valid ? Lowercase(_pathToUtf8(relative)) : FavoriteKey(path);
	}

	void LoadLabels()
	{
		s_menu.labels.clear();
		const fs::path path = LabelFilePath();
		if (path.empty())
			return;
		auto data = FileStream::LoadIntoMemory(path);
		if (!data)
			return;
		rapidjson::Document document;
		document.Parse(reinterpret_cast<const char*>(data->data()), data->size());
		if (document.HasParseError() || !document.IsObject())
			return;
		const auto labels = document.FindMember("labels");
		if (labels == document.MemberEnd() || !labels->value.IsObject())
			return;
		for (auto entry = labels->value.MemberBegin(); entry != labels->value.MemberEnd(); ++entry)
		{
			if (!entry->value.IsString() || entry->name.GetStringLength() == 0 ||
				entry->value.GetStringLength() == 0 || entry->value.GetStringLength() >= s_menu.labelText.size())
				continue;
			s_menu.labels.emplace(Lowercase(entry->name.GetString()), entry->value.GetString());
		}
	}

	bool SaveLabels()
	{
		const fs::path path = LabelFilePath();
		if (path.empty())
			return false;
		std::vector<std::pair<std::string, std::string>> labels(s_menu.labels.begin(), s_menu.labels.end());
		std::sort(labels.begin(), labels.end());
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("version");
		writer.Int(1);
		writer.Key("labels");
		writer.StartObject();
		for (const auto& [key, value] : labels)
		{
			writer.Key(key.data(), static_cast<rapidjson::SizeType>(key.size()));
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		}
		writer.EndObject();
		writer.EndObject();
		std::unique_ptr<FileStream> file(FileStream::createFile2(path));
		return file && file->writeData(buffer.GetString(), static_cast<sint32>(buffer.GetSize())) ==
			static_cast<sint32>(buffer.GetSize());
	}

	std::string FigureLabel(const fs::path& path)
	{
		const auto label = s_menu.labels.find(LabelKey(path));
		return label == s_menu.labels.end() ? std::string{} : label->second;
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
		LoadLabels();
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
		s_cascadeCardOffsets.clear();
		s_cascadeCardFocusAmounts.clear();
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
			const std::string haystack = Lowercase(figure.name + " " + FigureLabel(figure.filePath) + " " +
				_pathToUtf8(figure.filePath.filename()));
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
		++s_libraryGeneration;
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
		std::vector<PortalRow> loadedRows;
		const auto loaded = nsyshid::g_skyportal.GetLoadedSkylanders();
		loadedRows.reserve(kPortalCapacity);
		for (int slot = 0; slot < static_cast<int>(loaded.size()); ++slot)
		{
			if (!loaded[slot])
				continue;
			const auto* definition = s_menu.catalog.Find(loaded[slot]->first, loaded[slot]->second);
			loadedRows.push_back({slot, loaded[slot]->first, loaded[slot]->second, definition, 0});
		}

		auto swapperKey = [](uint16 id) -> int {
			if (id >= 1000 && id <= 1015)
				return id;
			if (id >= 2000 && id <= 2015)
				return id - 1000;
			return -1;
		};
		auto isTop = [](uint16 id) { return id >= 2000 && id <= 2015; };
		std::vector<PortalRow> rows;
		rows.reserve(kPortalCapacity);
		std::vector<bool> placed(loadedRows.size(), false);
		for (size_t index = 0; index < loadedRows.size(); ++index)
		{
			if (placed[index])
				continue;
			const int key = swapperKey(loadedRows[index].id);
			if (key < 0)
			{
				rows.push_back(loadedRows[index]);
				placed[index] = true;
				continue;
			}
			size_t pairIndex = loadedRows.size();
			for (size_t candidate = index + 1; candidate < loadedRows.size(); ++candidate)
			{
				if (!placed[candidate] && swapperKey(loadedRows[candidate].id) == key &&
					isTop(loadedRows[candidate].id) != isTop(loadedRows[index].id) &&
					loadedRows[candidate].variant == loadedRows[index].variant)
				{
					pairIndex = candidate;
					break;
				}
			}
			if (pairIndex == loadedRows.size())
			{
				for (size_t candidate = index + 1; candidate < loadedRows.size(); ++candidate)
				{
					if (!placed[candidate] && swapperKey(loadedRows[candidate].id) >= 0 &&
						isTop(loadedRows[candidate].id) != isTop(loadedRows[index].id))
					{
						pairIndex = candidate;
						break;
					}
				}
			}
			if (pairIndex == loadedRows.size())
			{
				rows.push_back(loadedRows[index]);
				placed[index] = true;
				continue;
			}
			const size_t topIndex = isTop(loadedRows[index].id) ? index : pairIndex;
			const size_t bottomIndex = topIndex == index ? pairIndex : index;
			rows.push_back(loadedRows[topIndex]);
			rows.push_back(loadedRows[bottomIndex]);
			placed[index] = true;
			placed[pairIndex] = true;
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

	std::string_view FigureGameName(skylander_ui::FigureGame game)
	{
		using skylander_ui::FigureGame;
		switch (game)
		{
		case FigureGame::SpyrosAdventure: return "SPYRO'S ADVENTURE";
		case FigureGame::Giants: return "GIANTS";
		case FigureGame::SwapForce: return "SWAP FORCE";
		case FigureGame::TrapTeam: return "TRAP TEAM";
		case FigureGame::SuperChargers: return "SUPERCHARGERS";
		case FigureGame::Imaginators: return "IMAGINATORS";
		default: return "UNKNOWN";
		}
	}

	std::string_view ForgeGameFilterName()
	{
		switch (s_menu.forgeGameFilter)
		{
		case 1: return "SSA";
		case 2: return "GIANTS";
		case 3: return "SWAP FORCE";
		case 4: return "TRAP TEAM";
		case 5: return "SUPERCHARGERS";
		case 6: return "IMAGINATORS";
		default: return "ALL";
		}
	}

	int FigureGameRank(skylander_ui::FigureGame game)
	{
		using skylander_ui::FigureGame;
		switch (game)
		{
		case FigureGame::SpyrosAdventure: return 0;
		case FigureGame::Giants: return 1;
		case FigureGame::SwapForce: return 2;
		case FigureGame::TrapTeam: return 3;
		case FigureGame::SuperChargers: return 4;
		case FigureGame::Imaginators: return 5;
		default: return 6;
		}
	}

	std::string_view ForgeSortName()
	{
		switch (static_cast<ForgeSortMode>(s_menu.forgeSortMode))
		{
		case ForgeSortMode::NameDescending: return "NAME Z-A";
		case ForgeSortMode::Element: return "ELEMENT";
		case ForgeSortMode::FigureType: return "FIGURE TYPE";
		case ForgeSortMode::Game: return "GAME";
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
			if (s_menu.forgeGameFilter != 0 &&
				definition.game != static_cast<skylander_ui::FigureGame>(s_menu.forgeGameFilter))
				continue;
			const std::string haystack = Lowercase(fmt::format("{} {} {} {} {:04X} {:04X}",
				definition.name, ElementName(definition.element), FigureTypeName(definition.type),
				FigureGameName(definition.game),
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
		case ForgeSortMode::Game:
			std::sort(filtered.begin(), filtered.end(), [&](const auto* lhs, const auto* rhs) {
				const auto left = std::tuple{FigureGameRank(lhs->game), Lowercase(lhs->name), lhs->id, lhs->variant};
				const auto right = std::tuple{FigureGameRank(rhs->game), Lowercase(rhs->name), rhs->id, rhs->variant};
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

	void CycleForgeGameFilter()
	{
		s_menu.forgeGameFilter = WrapChoice(s_menu.forgeGameFilter + 1, 7);
		s_menu.selectedDefinition = 0;
		SetToast("CREATE GAME: " + std::string(ForgeGameFilterName()), 1.5f);
		PulseHaptics();
	}

	void ResetForgeFilters()
	{
		s_menu.forgeSearchText.fill(0);
		s_menu.forgeTypeFilter = 0;
		s_menu.forgeElementFilter = 0;
		s_menu.forgeGameFilter = 0;
		s_menu.selectedDefinition = 0;
		SetToast("CREATE FILTERS CLEARED", 1.5f);
		PulseHaptics();
	}

	std::array<char, 96>& ActiveSearchText()
	{
		if (s_menu.searchTarget == SearchTarget::Create)
			return s_menu.forgeSearchText;
		if (s_menu.searchTarget == SearchTarget::Label)
			return s_menu.labelText;
		return s_menu.searchText;
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
		else if (s_menu.searchTarget == SearchTarget::Create)
			s_menu.selectedDefinition = 0;
	}

	void OpenSearchKeyboard(SearchTarget target)
	{
		s_menu.searchTarget = target;
		s_textInputTarget.store(static_cast<int>(target), std::memory_order_release);
		s_menu.keyboardRow = 0;
		s_menu.keyboardColumn = 0;
		s_menu.keyboardVisibility = MotionEnabled() ? 0.0f : 1.0f;
		for (auto& row : s_menu.keyboardFocusAnimations)
			row.fill(0.0f);
		PulseHaptics(UiSound::Confirm);
	}

	void CloseSearchKeyboard()
	{
		s_menu.searchTarget = SearchTarget::None;
		s_textInputTarget.store(0, std::memory_order_release);
		s_menu.keyboardVisibility = 0.0f;
		PulseHaptics(UiSound::Back);
	}

	void CommitSearchKeyboard()
	{
		if (s_menu.searchTarget == SearchTarget::Label && s_menu.details.fromLibrary &&
			!s_menu.details.filePath.empty())
		{
			std::string value = s_menu.labelText.data();
			const auto first = std::find_if_not(value.begin(), value.end(),
				[](unsigned char c) { return std::isspace(c); });
			const auto last = std::find_if_not(value.rbegin(), value.rend(),
				[](unsigned char c) { return std::isspace(c); }).base();
			value = first < last ? std::string(first, last) : std::string{};
			const std::string key = LabelKey(s_menu.details.filePath);
			const auto previous = s_menu.labels.find(key);
			const std::optional<std::string> oldValue = previous == s_menu.labels.end() ?
				std::nullopt : std::optional<std::string>(previous->second);
			if (value.empty())
				s_menu.labels.erase(key);
			else
				s_menu.labels[key] = value;
			if (!SaveLabels())
			{
				if (oldValue)
					s_menu.labels[key] = *oldValue;
				else
					s_menu.labels.erase(key);
				SetToast("THE COLLECTION TAG COULD NOT BE SAVED", 2.8f);
				PulseHaptics(UiSound::Back);
				return;
			}
			RebuildLibrary();
			SetToast(value.empty() ? "COLLECTION TAG CLEARED" : "COLLECTION TAG SAVED", 2.0f);
		}
		s_menu.searchTarget = SearchTarget::None;
		s_textInputTarget.store(0, std::memory_order_release);
		s_menu.keyboardVisibility = 0.0f;
		PulseHaptics(UiSound::Confirm);
	}

	void OpenLabelKeyboard()
	{
		if (!s_menu.details.fromLibrary || s_menu.details.filePath.empty())
			return;
		s_menu.labelText.fill(0);
		const std::string label = FigureLabel(s_menu.details.filePath);
		memcpy(s_menu.labelText.data(), label.data(), std::min(label.size(), s_menu.labelText.size() - 1));
		OpenSearchKeyboard(SearchTarget::Label);
	}

	void BackspaceSearchText()
	{
		auto& text = ActiveSearchText();
		size_t length = std::char_traits<char>::length(text.data());
		if (length == 0)
			return;
		do
		{
			--length;
		} while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80);
		text[length] = '\0';
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
		default: CommitSearchKeyboard(); break;
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
		if (rows[targetRow].actualSlot >= 0)
		{
			const auto snapshot = nsyshid::g_skyportal.GetSkylanderSnapshot(
				static_cast<uint8>(rows[targetRow].actualSlot));
			if (snapshot && snapshot->physical)
			{
				SetToast("SELECT A VIRTUAL CARD OR LIFT THE PHYSICAL FIGURE", 2.8f);
				PulseHaptics(UiSound::Back);
				return;
			}
		}
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
		if (const auto snapshot = nsyshid::g_skyportal.GetSkylanderSnapshot(static_cast<uint8>(slot));
			snapshot && snapshot->physical)
		{
			SetToast("LIFT " + name + " FROM THE PHYSICAL PORTAL", 2.8f);
			PulseHaptics(UiSound::Back);
			return;
		}
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
		s_menu.labels.erase(LabelKey(source));
		SaveLabels();
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
			WithAlpha(theme.background, layout.alpha * (0.16f + opacity * 0.72f)));
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
		draw->AddLine({shellMinimum.x, shellMinimum.y + 2.0f * layout.scale},
			{shellMinimum.x, shellMaximum.y - 2.0f * layout.scale},
			WithAlpha(theme.accent, layout.alpha * 0.34f), 1.2f * layout.scale);
	}

	bool ClickedRect(const RiftLayout& layout, ImVec2 minimum, ImVec2 maximum)
	{
		const bool clicked = layout.alpha > 0.9f && ImGui::IsMouseClicked(0) &&
			ImGui::IsMouseHoveringRect(minimum, maximum);
		if (clicked)
			s_menu.mouseNavigationActive = true;
		return clicked;
	}

	void UpdateMouseNavigationMode()
	{
		const ImVec2 position = ImGui::GetIO().MousePos;
		if (position.x <= -FLT_MAX * 0.5f || position.y <= -FLT_MAX * 0.5f)
			return;
		if (!s_menu.mousePositionInitialized)
		{
			s_menu.lastMousePosition = position;
			s_menu.mousePositionInitialized = true;
			return;
		}
		const float x = position.x - s_menu.lastMousePosition.x;
		const float y = position.y - s_menu.lastMousePosition.y;
		if (x * x + y * y >= 9.0f)
			s_menu.mouseNavigationActive = true;
		s_menu.lastMousePosition = position;
	}

	void ActivateHeaderSelection()
	{
		if (s_menu.page == RiftPage::ColorEditor && s_menu.colorEditorDirty)
		{
			g_config.Save();
			s_menu.colorEditorDirty = false;
		}
		switch (s_menu.headerSelection)
		{
		case 0:
		case 1:
		case 2: SetPortalMode(s_menu.headerSelection); PulseHaptics(UiSound::Confirm); break;
		case 3:
			s_menu.page = RiftPage::Forge;
			s_menu.focus = FocusArea::Library;
			s_menu.forgeToolbarFocused = false;
			PulseHaptics(UiSound::Confirm);
			break;
		case 4:
			s_menu.page = RiftPage::Options;
			s_menu.focus = FocusArea::Library;
			s_menu.selectedOption = 0;
			PulseHaptics(UiSound::Confirm);
			break;
		}
	}

	void DrawHeaderTab(ImDrawList* draw, const RiftLayout& layout, int index, float x, float width,
		std::string_view label, bool active, bool disabled = false)
	{
		const auto theme = CurrentTheme();
		const ImVec2 minimum = Point(layout, x, 77);
		const ImVec2 maximum = Point(layout, x + width, 113);
		const bool focused = s_menu.focus == FocusArea::Header && s_menu.headerSelection == index;
		float& focusAnimation = s_menu.headerFocusAnimations[index];
		focusAnimation = AnimateFocus(focusAnimation, focused ? 1.0f : active ? 0.32f : 0.0f);
		const ImU32 accent = index == 1 ? IM_COL32(91, 224, 131, 255) :
			(index == 2 ? theme.secondary : theme.accent);
		const float radius = CornerRadius(2.0f) * layout.scale;
		draw->PushClipRect(Point(layout, 684, 77), Point(layout, 1244, 113), true);
		if (focusAnimation > 0.01f)
			draw->AddRectFilled({minimum.x + 3.0f * layout.scale, minimum.y + 4.0f * layout.scale},
				{maximum.x + 3.0f * layout.scale, maximum.y + 5.0f * layout.scale},
				WithAlpha(IM_COL32(0, 3, 12, 255), layout.alpha * focusAnimation * 0.22f), radius);
		if (focusAnimation > 0.01f)
			draw->AddRectFilled(minimum, maximum,
				WithAlpha(BlendColor(theme.surface, theme.selectedSurface, focused ? 0.34f : 0.16f),
					layout.alpha * focusAnimation * (disabled ? 0.22f : 0.72f)), radius);
		if (active || focused)
			draw->AddRectFilled(Point(layout, x + (index == 0 ? 1.0f : 0.0f), 109),
				Point(layout, x + width - (index == 4 ? 1.0f : 0.0f), 112),
				WithAlpha(accent, layout.alpha * std::max(focusAnimation, active ? 0.72f : 0.0f)),
				CornerRadius(1.0f) * layout.scale);
		draw->PopClipRect();
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
		const int portalMode = PortalMode();
		DrawTitleText(draw, layout, Point(layout, 321, 31), 27.0f * layout.scale, "RIFT", 0.18f);
		draw->AddLine(Point(layout, 321, 59), Point(layout, 1244, 59),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.24f), layout.scale);

		DrawHeaderTab(draw, layout, 0, 684, 110, "VIRTUAL", portalMode == 0);
		DrawHeaderTab(draw, layout, 1, 794, 112, "PHYSICAL", portalMode == 1);
		DrawHeaderTab(draw, layout, 2, 906, 104, "HYBRID", portalMode == 2);
		DrawHeaderTab(draw, layout, 3, 1010, 128, "CREATE .SKY", s_menu.page == RiftPage::Forge);
		DrawHeaderTab(draw, layout, 4, 1138, 106, "OPTIONS",
			s_menu.page == RiftPage::Options || s_menu.page == RiftPage::ColorEditor);
		draw->AddRect(Point(layout, 684, 77), Point(layout, 1244, 113),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.32f), CornerRadius(4.0f) * layout.scale);
	}

	std::array<char, 96>& SearchTextForTarget(SearchTarget target)
	{
		if (target == SearchTarget::Create)
			return s_menu.forgeSearchText;
		if (target == SearchTarget::Label)
			return s_menu.labelText;
		return s_menu.searchText;
	}

	void SearchTextChanged(SearchTarget target)
	{
		if (target == SearchTarget::Collection)
			RebuildLibrary();
		else if (target == SearchTarget::Create)
			s_menu.selectedDefinition = 0;
	}

	void ProcessTextInputQueue()
	{
		std::deque<std::pair<int, unsigned int>> pending;
		{
			const std::lock_guard lock(s_textInputMutex);
			pending.swap(s_textInputQueue);
		}
		for (const auto& [targetValue, codepoint] : pending)
		{
			if (!s_menu.requestedOpen || targetValue < static_cast<int>(SearchTarget::Collection) ||
				targetValue > static_cast<int>(SearchTarget::Label))
				continue;
			const auto target = static_cast<SearchTarget>(targetValue);
			auto& text = SearchTextForTarget(target);
			if (codepoint == 8 || codepoint == 127)
			{
				size_t length = std::char_traits<char>::length(text.data());
				if (length > 0)
				{
					do
					{
						--length;
					} while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80);
					text[length] = '\0';
					SearchTextChanged(target);
				}
				continue;
			}
			if (codepoint == 27)
			{
				if (s_menu.searchTarget != SearchTarget::None)
					CloseSearchKeyboard();
				else
					s_textInputTarget.store(0, std::memory_order_release);
				continue;
			}
			if (codepoint == 10 || codepoint == 13)
			{
				if (s_menu.searchTarget != SearchTarget::None)
					CommitSearchKeyboard();
				else
					s_textInputTarget.store(0, std::memory_order_release);
				continue;
			}
			if (codepoint < 32 || codepoint > 0x10FFFF)
				continue;

			std::array<char, 4> encoded{};
			size_t encodedLength = 0;
			if (codepoint <= 0x7F)
				encoded[encodedLength++] = static_cast<char>(codepoint);
			else if (codepoint <= 0x7FF)
			{
				encoded[encodedLength++] = static_cast<char>(0xC0 | (codepoint >> 6));
				encoded[encodedLength++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			else if (codepoint <= 0xFFFF)
			{
				encoded[encodedLength++] = static_cast<char>(0xE0 | (codepoint >> 12));
				encoded[encodedLength++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
				encoded[encodedLength++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			else
			{
				encoded[encodedLength++] = static_cast<char>(0xF0 | (codepoint >> 18));
				encoded[encodedLength++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
				encoded[encodedLength++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
				encoded[encodedLength++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			const size_t length = std::char_traits<char>::length(text.data());
			if (length + encodedLength >= text.size())
				continue;
			memcpy(text.data() + length, encoded.data(), encodedLength);
			text[length + encodedLength] = '\0';
			SearchTextChanged(target);
		}
	}

	void DrawPortalColumn(ImDrawList* draw, const RiftLayout& layout)
	{
		const bool virtualPortal = VirtualPortalAvailable();
		DrawTitleText(draw, layout, Point(layout, 334, 158), 18.0f * layout.scale, "CURRENT PORTAL");

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
			const ImVec2 minimum = Point(layout, x, 184);
			const ImVec2 maximum = Point(layout, x + width, 250);
			const bool focused = s_menu.focus == FocusArea::Portal && s_menu.selectedPortalRow == rowIndex;
			float& focusAnimation = s_menu.portalFocusAnimations[rowIndex];
			focusAnimation = AnimateFocus(focusAnimation, focused ? 1.0f : 0.0f);
			ImU32 accent = CurrentTheme().accent;
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
			const bool roomy = width >= 180.0f;
			const bool showGame = width >= 260.0f;
			const float artX = roomy ? x + 30.0f : x + 24.0f;
			const float textX = roomy ? x + 60.0f : x + 47.0f;
			s_menu.portalTargets[rowIndex] = Point(layout, artX, 218);
			if (rows[rowIndex].actualSlot >= 0)
			{
				const bool hiddenDuringPlacement =
					s_menu.placementAnimation < 1.0f && rows[rowIndex].actualSlot == s_menu.placementSlot;
				if (!hiddenDuringPlacement)
					DrawCard(draw, rowTexture,
						s_menu.portalTargets[rowIndex], (44.0f + focusAnimation * 2.0f) * layout.scale,
						0.0f, focusAnimation, layout.alpha);
				const std::string name = rows[rowIndex].definition ? rows[rowIndex].definition->name :
					fmt::format("Unknown ({}, {})", rows[rowIndex].id, rows[rowIndex].variant);
				DrawTextFit(draw, Point(layout, textX, 190), WithAlpha(CurrentTheme().muted, layout.alpha),
					9.0f * layout.scale, DrawerWidth(layout, std::max(35.0f, width - (textX - x) - 12.0f)), slotLabel);
				DrawTextFit(draw, Point(layout, textX, 207), WithAlpha(CurrentTheme().text, layout.alpha),
					11.5f * layout.scale, DrawerWidth(layout, std::max(35.0f, width - (textX - x) - (roomy ? 78.0f : 10.0f))), Uppercase(name));
				if (rows[rowIndex].definition)
					DrawText(draw, Point(layout, textX, 226), WithAlpha(accent, layout.alpha),
						9.5f * layout.scale, ElementName(rows[rowIndex].definition->element));
				if (roomy)
				{
					if (rows[rowIndex].definition && showGame)
						DrawTextRightAligned(draw, Point(layout, x + width - 12.0f, 190),
							WithAlpha(CurrentTheme().muted, layout.alpha * 0.82f), 8.5f * layout.scale,
							FigureGameName(rows[rowIndex].definition->game));
					DrawTextRightAligned(draw, Point(layout, x + width - 12.0f, 226),
						WithAlpha(accent, layout.alpha), 9.5f * layout.scale,
						fmt::format("SLOT {:02}", rows[rowIndex].actualSlot + 1));
				}
			}
			else
			{
				DrawText(draw, Point(layout, x + 12, 198), WithAlpha(CurrentTheme().text, layout.alpha),
					11.0f * layout.scale, virtualPortal ? "ADD CARD" : "PHYSICAL PORTAL");
				DrawTextFit(draw, Point(layout, x + 12, 220), WithAlpha(CurrentTheme().muted, layout.alpha * 0.82f),
					10.5f * layout.scale, DrawerWidth(layout, std::max(42.0f, width - 24.0f)),
					virtualPortal ? "CHOOSE FROM THE LIBRARY" : "PLACE A FIGURE ON YOUR PORTAL");
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
		draw->AddLine(Point(layout, 334, 260), Point(layout, 1244, 260),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.28f), layout.scale);
	}

	void DrawSearchIcon(ImDrawList* draw, const RiftLayout& layout, ImVec2 center)
	{
		draw->AddCircle(center, 6.0f * layout.scale, WithAlpha(CurrentTheme().muted, layout.alpha),
			24, 1.6f * layout.scale);
		draw->AddLine({center.x + 4.5f * layout.scale, center.y + 4.5f * layout.scale},
			{center.x + 10.0f * layout.scale, center.y + 10.0f * layout.scale},
			WithAlpha(CurrentTheme().muted, layout.alpha), 1.6f * layout.scale);
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
		const auto theme = CurrentTheme();
		DrawTitleText(draw, layout, Point(layout, 334, 271), 18.0f * layout.scale, "LIBRARY");
		if (!s_menu.library.empty())
		{
			std::string selectedName = Uppercase(ShortName(s_menu.library[s_menu.selectedLibrary].name, 36));
			const std::string label = FigureLabel(s_menu.library[s_menu.selectedLibrary].filePath);
			if (!label.empty())
				selectedName += "  /  " + Uppercase(ShortName(label, 28));
			DrawTextFit(draw, Point(layout, 425, 274), WithAlpha(theme.accent, layout.alpha),
				12.0f * layout.scale, DrawerWidth(layout, 610.0f),
				selectedName);
		}
		DrawText(draw, Point(layout, 1202, 275), WithAlpha(theme.muted, layout.alpha),
			11.0f * layout.scale, fmt::format("{:02}", s_menu.library.size()));

		const ImVec2 toolbarMin = Point(layout, 334, 294);
		const ImVec2 toolbarMax = Point(layout, 1244, 333);
		draw->AddRectFilled(toolbarMin, toolbarMax,
			WithAlpha(theme.surface, layout.alpha * 0.76f), 5.0f * layout.scale);
		draw->AddRect(toolbarMin, toolbarMax,
			WithAlpha(CurrentTheme().border, layout.alpha * 0.28f), 5.0f * layout.scale);
		draw->AddLine(Point(layout, 650, 299), Point(layout, 650, 328),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.22f), layout.scale);
		draw->AddLine(Point(layout, 930, 299), Point(layout, 930, 328),
			WithAlpha(CurrentTheme().border, layout.alpha * 0.22f), layout.scale);
		const ImVec2 searchMin = Point(layout, 334, 294);
		const ImVec2 searchMax = Point(layout, 650, 333);
		if (s_menu.focus == FocusArea::Search)
			DrawElevatedSurface(draw, layout, searchMin, searchMax, true);
		DrawSearchIcon(draw, layout, Point(layout, 355, 312));
		ImGui::SetCursorScreenPos(Point(layout, 375, 298));
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
		if (ImGui::IsItemActive())
			s_textInputTarget.store(static_cast<int>(SearchTarget::Collection), std::memory_order_release);
		if (searchFont)
			ImGui::PopFont();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();

		const ImVec2 sortMin = Point(layout, 650, 294);
		const ImVec2 sortMax = Point(layout, 930, 333);
		DrawInlineGlyph(draw, layout, Point(layout, 675, 313), "LB", 24.0f);
		DrawTextFit(draw, Point(layout, 698, 302), WithAlpha(CurrentTheme().text, layout.alpha),
			12.0f * layout.scale, DrawerWidth(layout, 216.0f), SortModeName());
		if (ClickedRect(layout, sortMin, sortMax))
			OpenLibraryOrder();

		const ImVec2 favoriteMin = Point(layout, 930, 294);
		const ImVec2 favoriteMax = Point(layout, 1244, 333);
		const bool favorite = !s_menu.library.empty() && IsFavorite(s_menu.library[s_menu.selectedLibrary].filePath);
		if (favorite)
			draw->AddRectFilled(favoriteMin, favoriteMax,
				WithAlpha(IM_COL32(91, 69, 18, 220), layout.alpha * 0.52f), 0.0f);
		DrawInlineGlyph(draw, layout, Point(layout, 955, 313), "RB", 24.0f);
		if (favorite)
			DrawFavoriteIcon(draw, Point(layout, 988, 313), 13.0f * layout.scale,
				WithAlpha(IM_COL32(255, 210, 91, 255), layout.alpha));
		DrawText(draw, Point(layout, favorite ? 1003 : 980, 302),
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

	struct CascadeRowFilter
	{
		int type{};
		int game{};
		int element{};
	};

	struct CascadeRowData
	{
		CascadeRowFilter filter;
		std::string label;
		std::vector<int> indices;
	};

	std::string_view CascadeTypeName(int type)
	{
		switch (type)
		{
		case 1: return "SKYLANDERS";
		case 2: return "TRAPS";
		case 3: return "VEHICLES";
		case 4: return "ITEMS & CRYSTALS";
		default: return "ALL TYPES";
		}
	}

	std::string_view CascadeGameName(int game)
	{
		if (game <= 0)
			return "ALL GAMES";
		return FigureGameName(static_cast<skylander_ui::FigureGame>(std::clamp(game, 1, 6)));
	}

	std::vector<CascadeRowFilter> ParseCustomCascadeRows()
	{
		std::vector<CascadeRowFilter> rows;
		std::stringstream stream(GetConfig().emulated_usb_devices.skylander_cascade_rows.GetValue());
		std::string rowText;
		while (rows.size() < kMaximumCascadeRows && std::getline(stream, rowText, ';'))
		{
			std::stringstream rowStream(rowText);
			std::string value;
			std::array<int, 3> fields{};
			int field = 0;
			while (field < static_cast<int>(fields.size()) && std::getline(rowStream, value, ','))
			{
				try { fields[field] = std::stoi(value); }
				catch (...) { fields[field] = 0; }
				++field;
			}
			if (field > 0)
				rows.push_back({std::clamp(fields[0], 0, 4), std::clamp(fields[1], 0, 6),
					std::clamp(fields[2], 0, 11)});
		}
		if (rows.empty())
			rows = {{1, 0, 0}, {2, 0, 0}, {0, 4, 0}};
		return rows;
	}

	void SaveCustomCascadeRows(const std::vector<CascadeRowFilter>& rows)
	{
		std::string value;
		for (size_t index = 0; index < rows.size(); ++index)
		{
			if (index)
				value += ';';
			value += fmt::format("{},{},{}", rows[index].type, rows[index].game, rows[index].element);
		}
		GetConfig().emulated_usb_devices.skylander_cascade_rows = value;
	}

	std::string CascadeRowLabel(const CascadeRowFilter& filter)
	{
		std::string label;
		auto append = [&](std::string_view part) {
			if (!label.empty())
				label += "  /  ";
			label += part;
		};
		if (filter.game)
			append(CascadeGameName(filter.game));
		if (filter.type)
			append(CascadeTypeName(filter.type));
		if (filter.element)
			append(ElementChoiceName(filter.element, "ALL"));
		return label.empty() ? "ALL FIGURES" : label;
	}

	bool MatchesCascadeRow(const skylander_ui::CollectionFigure& figure, const CascadeRowFilter& filter)
	{
		using skylander_ui::FigureType;
		if (filter.type == 1 && figure.type != FigureType::Skylander)
			return false;
		if (filter.type == 2 && figure.type != FigureType::Trap)
			return false;
		if (filter.type == 3 && figure.type != FigureType::Vehicle)
			return false;
		if (filter.type == 4 && figure.type != FigureType::Item &&
			figure.type != FigureType::CreationCrystal && figure.type != FigureType::RacingDriver)
			return false;
		if (filter.element && figure.element != ElementForChoice(filter.element))
			return false;
		if (filter.game)
		{
			const auto* definition = s_menu.catalog.Find(figure.id, figure.variant);
			if (!definition || static_cast<int>(definition->game) != filter.game)
				return false;
		}
		return true;
	}

	const std::vector<CascadeRowData>& BuildCascadeRows()
	{
		auto& config = GetConfig().emulated_usb_devices;
		const int layoutMode = std::clamp<sint32>(config.skylander_cascade_rows_layout.GetValue(), 0, 3);
		const std::string rowSpecification = config.skylander_cascade_rows.GetValue();
		static uint64 cachedGeneration = static_cast<uint64>(-1);
		static int cachedLayout = -1;
		static std::string cachedSpecification;
		static std::vector<CascadeRowData> rows;
		if (cachedGeneration == s_libraryGeneration && cachedLayout == layoutMode &&
			cachedSpecification == rowSpecification)
			return rows;
		cachedGeneration = s_libraryGeneration;
		cachedLayout = layoutMode;
		cachedSpecification = rowSpecification;
		rows.clear();
		std::vector<CascadeRowFilter> filters;
		if (layoutMode == 1)
			filters = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
		else if (layoutMode == 2)
			filters = {{0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}};
		else if (layoutMode == 3)
			filters = ParseCustomCascadeRows();
		else
			filters = {{0, 0, 0}};

		for (const auto& filter : filters)
		{
			CascadeRowData row{filter, CascadeRowLabel(filter), {}};
			for (int index = 0; index < static_cast<int>(s_menu.library.size()); ++index)
			{
				if (MatchesCascadeRow(s_menu.library[index], filter))
					row.indices.push_back(index);
			}
			if (!row.indices.empty() || layoutMode == 3 || layoutMode == 0)
				rows.push_back(std::move(row));
		}
		if (rows.empty())
			rows.push_back({{}, "ALL FIGURES", {}});
		return rows;
	}

	void SelectCascadeRowCard(const std::vector<CascadeRowData>& rows, int rowIndex)
	{
		if (rows.empty())
			return;
		s_menu.selectedCascadeRow = std::clamp(rowIndex, 0, static_cast<int>(rows.size()) - 1);
		const auto& indices = rows[s_menu.selectedCascadeRow].indices;
		if (indices.empty())
			return;
		int& position = s_menu.cascadeRowPositions[s_menu.selectedCascadeRow];
		position = std::clamp(position, 0, static_cast<int>(indices.size()) - 1);
		s_menu.selectedLibrary = indices[position];
	}

	void DrawCascadeRows(ImDrawList* draw, const RiftLayout& layout, const std::vector<CascadeRowData>& rows)
	{
		const auto theme = CurrentTheme();
		s_menu.selectedCascadeRow = std::clamp(s_menu.selectedCascadeRow, 0,
			std::max(0, static_cast<int>(rows.size()) - 1));
		const int visibleCount = std::min(3, static_cast<int>(rows.size()));
		const int firstVisible = std::clamp(s_menu.selectedCascadeRow - 1, 0,
			std::max(0, static_cast<int>(rows.size()) - visibleCount));
		const float rowSpacing = visibleCount == 2 ? 118.0f : 75.0f;
		const float targetViewport = visibleCount == 1 ? static_cast<float>(s_menu.selectedCascadeRow) :
			(visibleCount == 2 ? 0.5f : static_cast<float>(firstVisible + 1));
		if (s_menu.cascadeViewport < 0.0f || !MotionEnabled())
			s_menu.cascadeViewport = targetViewport;
		else
			s_menu.cascadeViewport = SmoothTowards(s_menu.cascadeViewport, targetViewport,
				MotionValue(16.0f, 6.5f, 4.8f, 3.8f));
		const int requestedDensity = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_card_density.GetValue(), 5, 15);
		const int density = requestedDensity % 2 == 0 ? requestedDensity + 1 : requestedDensity;
		const int cascadeStyle = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_cascade_style.GetValue(), 0, 3);
		const int cardEffect = CardEffect();
		const float time = static_cast<float>(ImGui::GetTime());

		for (int rowIndex = 0; rowIndex < static_cast<int>(rows.size()) && rowIndex < kMaximumCascadeRows; ++rowIndex)
		{
			const auto& row = rows[rowIndex];
			const bool selectedRow = rowIndex == s_menu.selectedCascadeRow;
			float& rowFocus = s_menu.cascadeRowFocusAnimations[rowIndex];
			rowFocus = AnimateFocus(rowFocus, selectedRow ? 1.0f : 0.0f);
			const float rowDrift = AmbientMotionEnabled() ?
				std::sin(time * 0.58f * AmbientTempo() + rowIndex * 0.71f) *
					1.4f * AmbientStrength() * rowFocus : 0.0f;
			const float rowBaselineY = 528.0f + (static_cast<float>(rowIndex) - s_menu.cascadeViewport) *
				rowSpacing + rowDrift;
			if (rowBaselineY < 440.0f || rowBaselineY > 616.0f)
				continue;
			if (rowFocus > 0.01f)
				draw->AddRectFilled(Point(layout, 340, rowBaselineY - 78.0f), Point(layout, 1238, rowBaselineY + 7.0f),
					WithAlpha(theme.selectedSurface, layout.alpha * rowFocus * 0.13f), CornerRadius(5.0f) * layout.scale);
			DrawTextFit(draw, Point(layout, 350, rowBaselineY - 75.0f),
				WithAlpha(BlendColor(theme.muted, theme.accent, rowFocus),
					layout.alpha * (0.62f + rowFocus * 0.30f)),
				9.5f * layout.scale, DrawerWidth(layout, 190.0f), row.label);
			DrawTextRightAligned(draw, Point(layout, 1228, rowBaselineY - 75.0f),
				WithAlpha(BlendColor(theme.muted, theme.secondary, rowFocus),
					layout.alpha * (0.42f + rowFocus * 0.30f)),
				8.5f * layout.scale, fmt::format("{} CARDS", row.indices.size()));
			if (row.indices.empty())
			{
				DrawText(draw, Point(layout, 350, rowBaselineY - 43.0f), WithAlpha(theme.muted, layout.alpha * 0.60f),
					10.5f * layout.scale, "NO CARDS MATCH THIS ROW");
				continue;
			}

			int& selectedPosition = s_menu.cascadeRowPositions[rowIndex];
			const auto current = std::find(row.indices.begin(), row.indices.end(), s_menu.selectedLibrary);
			if (selectedRow && current != row.indices.end())
				selectedPosition = static_cast<int>(current - row.indices.begin());
			selectedPosition = std::clamp(selectedPosition, 0, static_cast<int>(row.indices.size()) - 1);
			const int visibleCardCount = std::min(density, static_cast<int>(row.indices.size()));
			const float spacing = visibleCardCount <= 5 ? 88.0f : visibleCardCount <= 7 ? 80.0f :
				visibleCardCount <= 11 ? 65.0f : visibleCardCount <= 13 ? 56.0f : 49.0f;

			struct RowCard
			{
				int position;
				float offset;
			};
			std::array<RowCard, 17> cards{};
			int cardCount = 0;
			auto addCard = [&](int relative) {
				if (cardCount >= visibleCardCount)
					return;
				int position = (selectedPosition + relative) % static_cast<int>(row.indices.size());
				if (position < 0) position += static_cast<int>(row.indices.size());
				bool duplicate = false;
				for (int existing = 0; existing < cardCount; ++existing)
					duplicate |= cards[existing].position == position;
				if (duplicate)
					return;
				const int libraryIndex = row.indices[position];
				const std::string key = fmt::format("{}:{}", rowIndex,
					FavoriteKey(s_menu.library[libraryIndex].filePath));
				auto existing = s_cascadeCardOffsets.find(key);
				if (existing == s_cascadeCardOffsets.end())
					existing = s_cascadeCardOffsets.emplace(key, static_cast<float>(relative)).first;
				const float resetDistance = static_cast<float>(std::max(2, visibleCardCount / 2 + 1));
				if (std::abs(existing->second - relative) > resetDistance)
					existing->second = static_cast<float>(relative);
				existing->second = MotionEnabled() ? SmoothTowards(existing->second,
					static_cast<float>(relative), CarouselResponse()) : static_cast<float>(relative);
				cards[cardCount++] = {position, existing->second};
			};
			addCard(0);
			for (int distance = 1; cardCount < visibleCardCount; ++distance)
			{
				addCard(-distance);
				addCard(distance);
			}
			std::sort(cards.begin(), cards.begin() + cardCount, [](const RowCard& left, const RowCard& right) {
				return std::abs(left.offset) > std::abs(right.offset);
			});

			for (int card = 0; card < cardCount; ++card)
			{
				const int libraryIndex = row.indices[cards[card].position];
				const float distance = std::abs(cards[card].offset);
				const bool selectedCard = selectedRow && cards[card].position == selectedPosition;
				const std::string focusKey = fmt::format("{}:{}", rowIndex,
					FavoriteKey(s_menu.library[libraryIndex].filePath));
				float& focus = s_cascadeCardFocusAmounts[focusKey];
				focus = AnimateFocus(focus, selectedCard ? 1.0f : 0.0f);
				auto texture = GetArtworkTexture(s_menu.library[libraryIndex].imagePath);
				texture.accent = ElementColor(s_menu.library[libraryIndex].element, texture.accent);
				const float magicPulse = AmbientMotionEnabled() ?
					0.5f + 0.5f * std::sin(time * 1.18f * AmbientTempo() + libraryIndex * 0.21f) : 0.5f;
				const float lift = focus * (3.5f +
					(magicPulse - 0.5f) * 0.6f * AmbientStrength());
				const float visualDistance = std::min(distance, 4.0f);
				const float depthStep = cascadeStyle == 2 ? 0.0f : cascadeStyle == 3 ? 4.2f :
					cascadeStyle == 1 ? 0.7f : 1.4f;
				const float height = 64.0f + rowFocus * 4.0f - visualDistance * depthStep + focus * 4.0f;
				const float cascadeDrop = cascadeStyle == 1 ? visualDistance * 0.7f : cascadeStyle == 2 ? 0.0f :
					cascadeStyle == 3 ? visualDistance * 1.8f : visualDistance * visualDistance * 0.55f;
				const ImVec2 center = Point(layout, 790.0f + cards[card].offset * spacing,
					rowBaselineY - height * 0.5f + cascadeDrop - lift);
				if (focus > 0.01f && cardEffect >= 1)
				{
					DrawGlow(draw, center, 67.0f * layout.scale, texture.accent,
						layout.alpha * focus * (0.36f + magicPulse * 0.11f));
					if (cardEffect >= 2)
						DrawCardAura(draw, center, 58.0f * layout.scale, texture.accent, focus, layout.alpha);
				}
				const float fanOffset = std::clamp(cards[card].offset, -4.0f, 4.0f);
				const float fanTilt = fanOffset * (cascadeStyle == 1 ? 0.048f : cascadeStyle == 2 ? 0.0f :
					cascadeStyle == 3 ? 0.012f : 0.025f);
				const float focusTilt = AmbientMotionEnabled() ? focus *
					std::sin(time * 0.64f * AmbientTempo() + libraryIndex * 0.17f) *
					0.006f * AmbientStrength() : 0.0f;
				DrawCard(draw, texture, center, height * layout.scale, fanTilt + focusTilt,
					focus, layout.alpha * (0.60f + rowFocus * 0.36f));
				const float width = height * (texture.id && texture.size.y > 0.0f ? texture.size.x / texture.size.y : 0.64f);
				const ImVec2 minimum{center.x - width * 0.60f * layout.scale, center.y - height * 0.60f * layout.scale};
				const ImVec2 maximum{center.x + width * 0.60f * layout.scale, center.y + height * 0.66f * layout.scale};
				if (ClickedRect(layout, minimum, maximum))
				{
					const int old = s_menu.selectedLibrary;
					s_menu.selectedCascadeRow = rowIndex;
					selectedPosition = cards[card].position;
					s_menu.selectedLibrary = libraryIndex;
					s_menu.focus = FocusArea::Library;
					UpdateSelectedAnimation(old);
					PulseHaptics();
				}
				if (selectedCard)
					s_menu.selectedCardCenter = center;
			}
		}
		SelectCascadeRowCard(rows, s_menu.selectedCascadeRow);
	}

	void DrawLibraryGrid(ImDrawList* draw, const RiftLayout& layout)
	{
		const auto theme = CurrentTheme();
		const int activeElement = GetConfig().emulated_usb_devices.skylander_element_filter.GetValue();
		constexpr float filterStartX = 334.0f;
		constexpr float filterEndX = 1244.0f;
		constexpr float filterGap = 5.5f;
		constexpr float filterWidth = (filterEndX - filterStartX - filterGap * 11.0f) / 12.0f;
		for (int choice = 0; choice < 12; ++choice)
		{
			const float x = filterStartX + choice * (filterWidth + filterGap);
			const ImVec2 lo = Point(layout, x, 338);
			const ImVec2 hi = Point(layout, x + filterWidth, 357);
			const bool active = choice == activeElement;
			const bool focused = s_menu.focus == FocusArea::ElementFilter &&
				s_menu.selectedElementFilter == choice;
			float& filterFocus = s_menu.elementFilterFocus[choice];
			filterFocus = AnimateFocus(filterFocus, focused ? 1.0f : active ? 0.48f : 0.0f);
			const ImU32 color = choice == 0 ? CurrentTheme().accent : ElementColor(ElementForChoice(choice), CurrentTheme().accent);
			const float radius = CornerRadius(4.0f) * layout.scale;
			draw->AddRectFilled(lo, hi, WithAlpha(BlendColor(CurrentTheme().surface,
				CurrentTheme().selectedSurface, filterFocus * 0.42f), layout.alpha * 0.78f), radius);
			draw->AddRect(lo, hi, WithAlpha(BlendColor(CurrentTheme().border, color, filterFocus),
				layout.alpha * (0.24f + filterFocus * 0.68f)), radius, 0,
				(1.0f + filterFocus) * layout.scale);
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
		const ImVec2 railMin = Point(layout, 334, 371);
		const ImVec2 railMax = Point(layout, 1244, 616);
		const float railInset = DrawerWidth(layout, 18.0f);
		const float railSpan = DrawerWidth(layout, 72.0f);
		const float verticalSpan = 38.0f * layout.scale;
		const ImU32 railColor = WithAlpha(theme.border, layout.alpha * 0.14f);
		const ImU32 horizontalAccent = WithAlpha(theme.secondary, layout.alpha * 0.20f);
		const ImU32 verticalAccent = WithAlpha(theme.accent, layout.alpha * 0.18f);
		draw->AddLine({railMin.x + railInset, railMin.y}, {railMin.x + railInset + railSpan, railMin.y}, railColor, layout.scale);
		draw->AddLine({railMax.x - railInset - railSpan, railMin.y}, {railMax.x - railInset, railMin.y}, railColor, layout.scale);
		draw->AddLine({railMin.x + railInset, railMax.y}, {railMin.x + railInset + railSpan, railMax.y}, railColor, layout.scale);
		draw->AddLine({railMax.x - railInset - railSpan, railMax.y}, {railMax.x - railInset, railMax.y}, railColor, layout.scale);
		const float middleX = (railMin.x + railMax.x) * 0.5f;
		draw->AddLine({middleX - railSpan * 0.55f, railMin.y}, {middleX + railSpan * 0.55f, railMin.y}, horizontalAccent, layout.scale);
		draw->AddLine({middleX - railSpan * 0.55f, railMax.y}, {middleX + railSpan * 0.55f, railMax.y}, horizontalAccent, layout.scale);
		const float middleY = (railMin.y + railMax.y) * 0.5f;
		draw->AddLine({railMin.x, middleY - verticalSpan}, {railMin.x, middleY + verticalSpan}, verticalAccent, layout.scale);
		draw->AddLine({railMax.x, middleY - verticalSpan}, {railMax.x, middleY + verticalSpan}, verticalAccent, layout.scale);
		if (s_menu.library.empty())
		{
			const std::string catalogError = s_menu.catalog.GetError();
			const std::string configuredFolder = GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
			std::error_code folderError;
			const bool folderExists = !configuredFolder.empty() &&
				fs::is_directory(_utf8ToPath(configuredFolder), folderError) && !folderError;
			const char* heading = "NO FIGURES MATCH";
			std::string guidance = s_menu.searchText[0] != '\0' ?
				"CLEAR THE SEARCH OR CHANGE YOUR FILTERS" :
				"SET FIGURE FILTER TO EVERYTHING AND ELEMENT TO ALL";
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
			else if (s_menu.catalog.GetCollection().empty())
			{
				heading = "NO SUPPORTED FIGURE FILES FOUND";
				guidance = "RIFT CHECKED THIS FOLDER AND ITS SUBFOLDERS";
			}
			DrawTextCentered(draw, Point(layout, 790, 456),
				WithAlpha(theme.text, layout.alpha),
				20.0f * layout.scale, heading);
			DrawTextCentered(draw, Point(layout, 790, 489),
				WithAlpha(theme.muted, layout.alpha),
				13.0f * layout.scale, Uppercase(guidance));
			return;
		}
		s_menu.selectedLibrary = std::clamp(s_menu.selectedLibrary, 0, static_cast<int>(s_menu.library.size()) - 1);
		const auto& cascadeRows = BuildCascadeRows();
		const bool multipleRows = cascadeRows.size() > 1;
		if (ImGui::IsMouseHoveringRect(railMin, railMax) && std::abs(ImGui::GetIO().MouseWheel) > 0.01f)
		{
			const int oldSelection = s_menu.selectedLibrary;
			const int direction = ImGui::GetIO().MouseWheel < 0.0f ? 1 : -1;
			if (multipleRows)
			{
				s_menu.selectedCascadeRow = std::clamp(s_menu.selectedCascadeRow + direction, 0,
					static_cast<int>(cascadeRows.size()) - 1);
				SelectCascadeRowCard(cascadeRows, s_menu.selectedCascadeRow);
			}
			else
			{
				const int count = static_cast<int>(s_menu.library.size());
				s_menu.selectedLibrary = (s_menu.selectedLibrary + direction + count) % count;
			}
			s_menu.focus = FocusArea::Library;
			UpdateSelectedAnimation(oldSelection);
			PulseHaptics();
		}
		if (multipleRows)
		{
			DrawCascadeRows(draw, layout, cascadeRows);
			return;
		}


		struct CarouselEntry
		{
			int index{};
			float offset{};
			float focus{};
			ArtworkTexture texture;
		};
		std::array<CarouselEntry, 19> visible{};
		int visibleCount = 0;
		const int count = static_cast<int>(s_menu.library.size());
		const int requestedDensity = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_card_density.GetValue(), 5, 15);
		const int cardDensity = requestedDensity % 2 == 0 ? requestedDensity + 1 : requestedDensity;
		const int layoutDensity = std::min(cardDensity, count);
		const int cascadeStyle = std::clamp<sint32>(
			GetConfig().emulated_usb_devices.skylander_cascade_style.GetValue(), 0, 3);
		const int visibleRadius = cardDensity / 2;
		std::array<int, 19> candidateIndices{};
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
				SmoothTowards(existing->second, static_cast<float>(targetOffset), CarouselResponse()) :
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
			const float magicPulse = AmbientMotionEnabled() ?
				0.5f + 0.5f * std::sin(time * 1.18f * AmbientTempo() + entry.index * 0.21f) : 0.5f;
			const float lift = entry.focus * (7.0f +
				(magicPulse - 0.5f) * 1.4f * AmbientStrength());
			const float depthStep = cascadeStyle == 2 ? 0.0f : cascadeStyle == 3 ? 12.0f :
				cascadeStyle == 1 ? 2.0f : 4.0f;
			const float designHeight = 182.0f - std::min(distance, 4.0f) * depthStep + entry.focus * 6.0f;
			const float cardSpacing = layoutDensity <= 5 ? 142.0f : layoutDensity <= 7 ? 110.0f :
				layoutDensity <= 9 ? 86.0f : layoutDensity <= 11 ? 72.0f : layoutDensity <= 13 ? 60.0f : 52.0f;
			const float cascadeDrop = cascadeStyle == 1 ? distance * 2.0f : cascadeStyle == 2 ? 0.0f :
				cascadeStyle == 3 ? distance * 5.0f : distance * distance * 1.7f;
			const ImVec2 center = Point(layout, 790.0f + entry.offset * cardSpacing,
				493.0f + cascadeDrop - lift);
			const float height = designHeight * layout.scale;
			const int cardEffect = CardEffect();
			if (entry.focus > 0.01f && cardEffect >= 1)
			{
				DrawGlow(draw, center, 116.0f * layout.scale, entry.texture.accent,
					layout.alpha * entry.focus * (0.42f + magicPulse * 0.13f));
				if (cardEffect >= 2)
					DrawCardAura(draw, center, 98.0f * layout.scale, entry.texture.accent, entry.focus, layout.alpha);
			}
			const float fanTilt = entry.offset * (cascadeStyle == 1 ? 0.048f : cascadeStyle == 2 ? 0.0f :
				cascadeStyle == 3 ? 0.012f : 0.025f);
			const float focusTilt = AmbientMotionEnabled() ?
				entry.focus * std::sin(time * 0.62f * AmbientTempo() + entry.index * 0.17f) *
					0.006f * AmbientStrength() : 0.0f;
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
				DrawFavoriteIcon(draw, badge, 12.0f * layout.scale,
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
		const auto* definition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant);
		if (definition)
			texture.accent = ElementColor(definition->element, texture.accent);
		const ImVec2 cardCenter = Point(layout, 490, 390);
		if (CardEffect() >= 1)
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
			const std::string label = FigureLabel(s_menu.details.filePath);
			const bool trap = definition && definition->type == skylander_ui::FigureType::Trap;
			DrawText(draw, Point(layout, 660, 431), WithAlpha(CurrentTheme().muted, layout.alpha),
				12.0f * layout.scale, trap ? "VILLAIN TAG" : "COLLECTION TAG");
			DrawTextFit(draw, Point(layout, 812, 431),
				WithAlpha(label.empty() ? CurrentTheme().muted : texture.accent, layout.alpha),
				14.0f * layout.scale, DrawerWidth(layout, 390.0f), label.empty() ? "NOT SET" : label);
			const bool favorite = IsFavorite(s_menu.details.filePath);
			DrawText(draw, Point(layout, 660, 469), WithAlpha(CurrentTheme().muted, layout.alpha),
				12.0f * layout.scale, "FAVORITE");
			if (favorite)
				DrawFavoriteIcon(draw, Point(layout, 820, 478), 14.0f * layout.scale,
					WithAlpha(IM_COL32(255, 209, 79, 255), layout.alpha));
			DrawText(draw, Point(layout, favorite ? 837 : 812, 469),
				WithAlpha(favorite ? IM_COL32(255, 217, 111, 255) : IM_COL32(174, 199, 217, 255), layout.alpha),
				14.0f * layout.scale, favorite ? "YES" : "NO");
			DrawText(draw, Point(layout, 660, 507), WithAlpha(CurrentTheme().muted, layout.alpha),
				12.0f * layout.scale, "FILE");
			DrawTextFit(draw, Point(layout, 660, 531), WithAlpha(IM_COL32(207, 223, 235, 255), layout.alpha),
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
		DrawTitleText(draw, layout, Point(layout, 338, 163), 23.0f * layout.scale,
			"CREATE A FIGURE");
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
				17.0f * layout.scale, DrawerWidth(layout, 350.0f), selected.name);
			DrawTextRightAligned(draw, Point(layout, 1238, 170),
				WithAlpha(ElementColor(selected.element, theme.accent), layout.alpha),
				10.5f * layout.scale, fmt::format("{}  /  {}  /  {}", ElementName(selected.element),
					FigureTypeName(selected.type), FigureGameName(selected.game)));
		}

		const ImVec2 searchMin = Point(layout, 338, 211);
		const ImVec2 searchMax = Point(layout, 620, 253);
		const ImVec2 sortMin = Point(layout, 630, 211);
		const ImVec2 sortMax = Point(layout, 772, 253);
		const ImVec2 typeMin = Point(layout, 782, 211);
		const ImVec2 typeMax = Point(layout, 924, 253);
		const ImVec2 elementMin = Point(layout, 934, 211);
		const ImVec2 elementMax = Point(layout, 1086, 253);
		const ImVec2 gameMin = Point(layout, 1096, 211);
		const ImVec2 gameMax = Point(layout, 1238, 253);
		const ImU32 sortAccent = BlendColor(IM_COL32(255, 190, 74, 255), theme.accent, 0.18f);
		const ImU32 typeAccent = BlendColor(IM_COL32(183, 128, 255, 255), theme.accent, 0.18f);
		const ImU32 elementAccent = s_menu.forgeElementFilter == 0 ?
			BlendColor(IM_COL32(93, 225, 158, 255), theme.accent, 0.18f) :
			ElementColor(ElementForChoice(s_menu.forgeElementFilter), theme.accent);
		const ImU32 gameAccent = BlendColor(IM_COL32(255, 133, 91, 255), theme.accent, 0.18f);
		const std::array<bool, 5> toolbarActive{
			s_menu.forgeSearchText[0] != '\0', s_menu.forgeSortMode != 0, s_menu.forgeTypeFilter != 0,
			s_menu.forgeElementFilter != 0, s_menu.forgeGameFilter != 0};
		const std::array<ImU32, 5> toolbarAccents{theme.accent, sortAccent, typeAccent, elementAccent, gameAccent};
		for (int index = 0; index < 5; ++index)
		{
			const bool focused = s_menu.forgeToolbarFocused && s_menu.forgeToolbarSelection == index;
			s_menu.forgeToolbarFocusAnimations[index] = AnimateFocus(
				s_menu.forgeToolbarFocusAnimations[index], focused ? 1.0f : 0.0f);
		}
		DrawAnimatedControlSurface(draw, layout, searchMin, searchMax,
			s_menu.forgeToolbarFocusAnimations[0], toolbarActive[0], toolbarAccents[0]);
		DrawAnimatedControlSurface(draw, layout, sortMin, sortMax,
			s_menu.forgeToolbarFocusAnimations[1], toolbarActive[1], toolbarAccents[1]);
		DrawAnimatedControlSurface(draw, layout, typeMin, typeMax,
			s_menu.forgeToolbarFocusAnimations[2], toolbarActive[2], toolbarAccents[2]);
		DrawAnimatedControlSurface(draw, layout, elementMin, elementMax,
			s_menu.forgeToolbarFocusAnimations[3], toolbarActive[3], toolbarAccents[3]);
		DrawAnimatedControlSurface(draw, layout, gameMin, gameMax,
			s_menu.forgeToolbarFocusAnimations[4], toolbarActive[4], toolbarAccents[4]);
		DrawSearchIcon(draw, layout, Point(layout, 359, 231));
		ImGui::SetCursorScreenPos(Point(layout, 378, 215));
		ImGui::SetNextItemWidth(DrawerWidth(layout, 228.0f));
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
		if (ImGui::IsItemActive())
			s_textInputTarget.store(static_cast<int>(SearchTarget::Create), std::memory_order_release);
		if (searchFont)
			ImGui::PopFont();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
		if (ClickedRect(layout, searchMin, searchMax))
		{
			s_menu.focus = FocusArea::Search;
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 0;
		}
		DrawTextFit(draw, Point(layout, 643, 226), WithAlpha(theme.text, layout.alpha),
			10.5f * layout.scale, DrawerWidth(layout, 116.0f), fmt::format("SORT  {}", ForgeSortName()));
		DrawTextFit(draw, Point(layout, 795, 226), WithAlpha(theme.text, layout.alpha),
			10.5f * layout.scale, DrawerWidth(layout, 116.0f), fmt::format("TYPE  {}", ForgeTypeFilterName()));
		DrawTextFit(draw, Point(layout, 947, 226), WithAlpha(theme.text, layout.alpha),
			10.5f * layout.scale, DrawerWidth(layout, 126.0f),
			fmt::format("ELEMENT  {}", ElementChoiceName(s_menu.forgeElementFilter, "ALL")));
		DrawTextFit(draw, Point(layout, 1109, 226), WithAlpha(theme.text, layout.alpha),
			10.5f * layout.scale, DrawerWidth(layout, 116.0f),
			fmt::format("GAME  {}", ForgeGameFilterName()));
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
		if (ClickedRect(layout, gameMin, gameMax))
		{
			s_menu.forgeToolbarFocused = true;
			s_menu.forgeToolbarSelection = 4;
			CycleForgeGameFilter();
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
			if (focus > 0.01f && CardEffect() >= 1)
				DrawGlow(draw, center, 102.0f * layout.scale, texture.accent, layout.alpha * focus * 0.50f);
			DrawCard(draw, texture, center, (138.0f + focus * 16.0f) * layout.scale,
				side * (1.0f - focus) * 0.012f, focus, layout.alpha * (0.80f + focus * 0.20f));
			const ImVec2 minimum = Point(layout, 356.0f + column * 220.0f, 278.0f + row * 170.0f);
			const ImVec2 maximum = Point(layout, 504.0f + column * 220.0f, 430.0f + row * 170.0f);
			if (s_menu.mouseNavigationActive && layout.alpha > 0.9f &&
				ImGui::IsMouseHoveringRect(minimum, maximum) &&
				index != s_menu.selectedDefinition)
				s_menu.selectedDefinition = index;
			if (s_menu.mouseNavigationActive && ImGui::IsMouseHoveringRect(minimum, maximum))
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

	void OpenColorEditor()
	{
		auto& config = GetConfig().emulated_usb_devices;
		const int theme = std::clamp<sint32>(config.skylander_theme.GetValue(), 0, 5);
		const int accentIndex = std::clamp<sint32>(config.skylander_accent.GetValue(), 0, 6);
		const ImU32 accent = ThemeAccentColor(theme, accentIndex);
		ColorToHsv(accent, s_menu.colorEditorHsv[0]);
		ColorToHsv(ThemeBaseColor(theme), s_menu.colorEditorHsv[1]);
		for (int target = 0; target < 2; ++target)
		{
			const auto& hsv = s_menu.colorEditorHsv[target];
			const float angle = hsv[0] * 2.0f * kPi - kPi * 0.5f;
			s_menu.colorEditorCursor[target] = {
				std::cos(angle) * hsv[1], std::sin(angle) * hsv[1]};
		}
		s_menu.colorEditorTarget = 0;
		s_menu.colorEditorDirty = false;
		s_menu.page = RiftPage::ColorEditor;
		PulseHaptics(UiSound::Confirm);
	}

	void ApplyColorEditorValue()
	{
		auto& config = GetConfig().emulated_usb_devices;
		auto& hsv = s_menu.colorEditorHsv[std::clamp(s_menu.colorEditorTarget, 0, 1)];
		const uint32 color = ColorToConfig(HsvColor(hsv[0], hsv[1], hsv[2]));
		if (s_menu.colorEditorTarget == 0)
		{
			config.skylander_custom_accent = color;
			config.skylander_accent = 6;
		}
		else
		{
			config.skylander_custom_theme = color;
			config.skylander_theme = 5;
		}
		s_menu.colorEditorDirty = true;
	}

	void MoveColorEditorCursor(float x, float y, bool pulse = true)
	{
		const int target = std::clamp(s_menu.colorEditorTarget, 0, 1);
		auto& cursor = s_menu.colorEditorCursor[target];
		cursor.x += x;
		cursor.y += y;
		float distance = std::sqrt(cursor.x * cursor.x + cursor.y * cursor.y);
		if (distance > 1.0f)
		{
			cursor.x /= distance;
			cursor.y /= distance;
			distance = 1.0f;
		}
		auto& hsv = s_menu.colorEditorHsv[target];
		hsv[1] = distance;
		if (distance > 0.001f)
			hsv[0] = std::fmod(std::atan2(cursor.y, cursor.x) / (2.0f * kPi) + 1.25f, 1.0f);
		ApplyColorEditorValue();
		if (pulse)
			PulseHaptics();
	}

	void AdjustColorEditorBrightness(float value, bool pulse = true)
	{
		auto& hsv = s_menu.colorEditorHsv[std::clamp(s_menu.colorEditorTarget, 0, 1)];
		hsv[2] = std::clamp(hsv[2] + value, 0.12f, 1.0f);
		ApplyColorEditorValue();
		if (pulse)
			PulseHaptics();
	}

	void FinishColorEditor()
	{
		if (s_menu.colorEditorDirty)
		{
			g_config.Save();
			s_menu.colorEditorDirty = false;
			SetToast("CUSTOM COLORS SAVED", 1.8f);
		}
		s_menu.page = RiftPage::Options;
	}

	std::string OptionValue(int option)
	{
		auto& config = GetConfig().emulated_usb_devices;
		switch (option)
		{
		case 0:
		{
			static constexpr std::array names{"OFF", "REDUCED", "SMOOTH", "FLOATING", "CINEMATIC"};
			return names[std::clamp<sint32>(config.skylander_motion_level.GetValue(), 0, 4)];
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
			const int density = std::clamp<sint32>(config.skylander_card_density.GetValue(), 5, 15);
			return fmt::format("{} CARDS", density % 2 == 0 ? density + 1 : density);
		}
		case 6:
		{
			static constexpr std::array names{"CLASSIC", "DARK RIFT", "EMBER", "VERDANT", "ARCANE", "CUSTOM"};
			return names[std::clamp<sint32>(config.skylander_theme.GetValue(), 0, 5)];
		}
		case 7:
		{
			static constexpr std::array names{"THEME", "CYAN", "VIOLET", "GOLD", "GREEN", "CORAL", "CUSTOM"};
			return names[std::clamp<sint32>(config.skylander_accent.GetValue(), 0, 6)];
		}
		case 8: return "OPEN";
		case 9:
		{
			static constexpr std::array names{"CLEAN", "GLOW", "RIFT", "FOIL"};
			return names[std::clamp<sint32>(config.skylander_card_effect.GetValue(), 0, 3)];
		}
		case 10:
		{
			static constexpr std::array names{"OFF", "LOW", "NORMAL", "HIGH"};
			return names[std::clamp<sint32>(config.skylander_particle_level.GetValue(), 0, 3)];
		}
		case 11:
		{
			static constexpr std::array names{"SOFT", "TACTILE", "ARCANE", "BRIGHT"};
			return names[std::clamp<sint32>(config.skylander_sound_profile.GetValue(), 0, 3)];
		}
		case 12:
		{
			static constexpr std::array names{"OFF", "SOFT", "MEDIUM", "STRONG"};
			return names[std::clamp<sint32>(config.skylander_haptic_strength.GetValue(), 0, 3)];
		}
		case 13:
		{
			static constexpr std::array names{"SQUARE", "SUBTLE", "ROUND"};
			return names[std::clamp<sint32>(config.skylander_corner_style.GetValue(), 0, 2)];
		}
		case 14:
		{
			static constexpr std::array names{"CLEAN", "SUBTLE EDGE"};
			return names[std::clamp<sint32>(config.skylander_card_border.GetValue(), 0, 1)];
		}
		case 15:
		{
			static constexpr std::array names{"ARC", "FAN", "FLAT", "DEPTH"};
			return names[std::clamp<sint32>(config.skylander_cascade_style.GetValue(), 0, 3)];
		}
		case 16: return GetConfig().check_update.GetValue() ? "ON" : "OFF";
		case 17: return "CHECK NOW";
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
		const auto customRows = ParseCustomCascadeRows();
		const int editorRow = std::clamp(s_menu.selectedCascadeEditorRow, 0,
			static_cast<int>(customRows.size()) - 1);
		switch (option)
		{
		case 0: return std::string(SortModeName());
		case 1: return std::string(PriorityModeName());
		case 2: return std::string(ElementChoiceName(
			std::clamp<sint32>(config.skylander_element_priority.GetValue(), 0, 11), "NONE"));
		case 3: return std::string(FilterModeName());
		case 4: return std::string(ElementChoiceName(
			std::clamp<sint32>(config.skylander_element_filter.GetValue(), 0, 11), "ALL ELEMENTS"));
		case 5:
		{
			static constexpr std::array names{"SINGLE", "TOY TYPES", "GAMES", "CUSTOM"};
			return names[std::clamp<sint32>(config.skylander_cascade_rows_layout.GetValue(), 0, 3)];
		}
		case 6: return fmt::format("{} ROWS", customRows.size());
		case 7: return fmt::format("ROW {}", editorRow + 1);
		case 8: return std::string(CascadeTypeName(customRows[editorRow].type));
		case 9: return std::string(CascadeGameName(customRows[editorRow].game));
		case 10: return std::string(ElementChoiceName(customRows[editorRow].element, "ALL ELEMENTS"));
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
		case 5:
			config.skylander_cascade_rows_layout = WrapChoice(
				config.skylander_cascade_rows_layout.GetValue() + direction, 4);
			s_menu.selectedCascadeRow = 0;
			s_menu.cascadeRowPositions.fill(0);
			s_menu.cascadeViewport = -1.0f;
			s_cascadeCardOffsets.clear();
			s_cascadeCardFocusAmounts.clear();
			break;
		case 6:
		{
			auto rows = ParseCustomCascadeRows();
			const int count = 1 + WrapChoice(static_cast<int>(rows.size()) - 1 + direction,
				kMaximumCascadeRows);
			rows.resize(count);
			SaveCustomCascadeRows(rows);
			config.skylander_cascade_rows_layout = 3;
			s_menu.selectedCascadeEditorRow = std::min(s_menu.selectedCascadeEditorRow, count - 1);
			s_menu.selectedCascadeRow = 0;
			s_menu.cascadeRowPositions.fill(0);
			s_menu.cascadeViewport = -1.0f;
			s_cascadeCardOffsets.clear();
			s_cascadeCardFocusAmounts.clear();
			break;
		}
		case 7:
		{
			const auto rows = ParseCustomCascadeRows();
			s_menu.selectedCascadeEditorRow = WrapChoice(
				s_menu.selectedCascadeEditorRow + direction, static_cast<int>(rows.size()));
			break;
		}
		case 8:
		case 9:
		case 10:
		{
			auto rows = ParseCustomCascadeRows();
			s_menu.selectedCascadeEditorRow = std::clamp(s_menu.selectedCascadeEditorRow, 0,
				static_cast<int>(rows.size()) - 1);
			auto& row = rows[s_menu.selectedCascadeEditorRow];
			if (option == 8) row.type = WrapChoice(row.type + direction, 5);
			if (option == 9) row.game = WrapChoice(row.game + direction, 7);
			if (option == 10) row.element = WrapChoice(row.element + direction, 12);
			SaveCustomCascadeRows(rows);
			config.skylander_cascade_rows_layout = 3;
			s_menu.selectedCascadeRow = 0;
			s_menu.cascadeRowPositions.fill(0);
			s_menu.cascadeViewport = -1.0f;
			s_cascadeCardOffsets.clear();
			s_cascadeCardFocusAmounts.clear();
			break;
		}
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
			config.skylander_motion_level = WrapChoice(config.skylander_motion_level.GetValue() + direction, 5);
			break;
		case 1:
			config.skylander_bloom_level = WrapChoice(config.skylander_bloom_level.GetValue() + direction, 3);
			break;
		case 2:
		{
			int opacity = std::clamp<sint32>(config.skylander_drawer_opacity.GetValue(), 10, 100);
			opacity = ((opacity + 2) / 5) * 5 - direction * 5;
			if (opacity < 10)
				opacity = 100;
			else if (opacity > 100)
				opacity = 10;
			config.skylander_drawer_opacity = opacity;
			break;
		}
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
			const int density = std::clamp<sint32>(config.skylander_card_density.GetValue(), 5, 15);
			const int densityIndex = std::clamp((density - 5) / 2, 0, 5);
			config.skylander_card_density = 5 + WrapChoice(densityIndex + direction, 6) * 2;
			break;
		}
		case 6: config.skylander_theme = WrapChoice(config.skylander_theme.GetValue() + direction, 6); break;
		case 7: config.skylander_accent = WrapChoice(config.skylander_accent.GetValue() + direction, 7); break;
		case 8: OpenColorEditor(); return;
		case 9: config.skylander_card_effect = WrapChoice(config.skylander_card_effect.GetValue() + direction, 4); break;
		case 10: config.skylander_particle_level = WrapChoice(config.skylander_particle_level.GetValue() + direction, 4); break;
		case 11: config.skylander_sound_profile = WrapChoice(config.skylander_sound_profile.GetValue() + direction, 4); break;
		case 12:
			config.skylander_haptic_strength = WrapChoice(config.skylander_haptic_strength.GetValue() + direction, 4);
			config.skylander_ui_haptics = config.skylander_haptic_strength.GetValue() > 0;
			if (!config.skylander_ui_haptics.GetValue()) StopMenuHaptics();
			break;
		case 13: config.skylander_corner_style = WrapChoice(config.skylander_corner_style.GetValue() + direction, 3); break;
		case 14: config.skylander_card_border = WrapChoice(config.skylander_card_border.GetValue() + direction, 2); break;
		case 15: config.skylander_cascade_style = WrapChoice(config.skylander_cascade_style.GetValue() + direction, 4); break;
		case 16:
			GetConfig().check_update = !GetConfig().check_update.GetValue();
			break;
		case 17:
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
		DrawTitleText(draw, layout, Point(layout, 337, 188), 24.0f * layout.scale,
			"DISPLAY, FEEDBACK & UPDATES");
		DrawText(draw, Point(layout, 337, 222), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "USE THE D-PAD TO CHOOSE. PRESS A TO CHANGE THE SELECTED OPTION.");

		const std::array<std::pair<std::string_view, std::string_view>, kOptionCount> rows{{
			{"MOTION", "Five speeds for drawer, rows, card lift, and shimmer"},
			{"BLOOM", "Portal and selected-card light"},
			{"GLASS OPACITY", "Press A to lower it; wraps after 10%"},
			{"UI SOUND", "Navigation and confirmation clicks"},
			{"CONTROLLER HAPTICS", "Short focus and action pulses"},
			{"CARD CASCADE", "Visible library density"},
			{"COLOR SCHEME", "Complete Rift palette"},
			{"ACCENT COLOR", "Highlight and portal glow"},
			{"CUSTOM COLORS", "Choose exact theme and accent colors"},
			{"CARD EFFECT", "Clean, glow, rift, or foil"},
			{"PARTICLES", "Ambient magical texture"},
			{"SOUND PROFILE", "Soft through bright tactile clicks"},
			{"HAPTIC STRENGTH", "Off through strong feedback"},
			{"CARD & PANEL CORNERS", "Square, subtle, or round edges"},
			{"CARD BORDER", "Clean or a subtle element edge"},
			{"CASCADE SHAPE", "Arc, fan, flat, or deep stack"},
			{"AUTO UPDATE CHECK", "Check quietly whenever Rift starts"},
			{"CHECK FOR UPDATES", "Compare this build with the latest GitHub release"}}};
		for (int option = 0; option < static_cast<int>(rows.size()); ++option)
		{
			const int column = option / 9;
			const int row = option % 9;
			const float x = 338.0f + column * 454.0f;
			const float y = 248.0f + row * 41.0f;
			const ImVec2 minimum = Point(layout, x, y);
			const ImVec2 maximum = Point(layout, x + 440.0f, y + 35.0f);
			const bool selected = option == s_menu.selectedOption;
			float& focusAnimation = s_menu.optionFocusAnimations[option];
			focusAnimation = AnimateFocus(focusAnimation, selected ? 1.0f : 0.0f);
			DrawSettingsChoiceSurface(draw, layout, minimum, maximum, focusAnimation);
			DrawText(draw, Point(layout, x + 12.0f, y + 2.5f),
				WithAlpha(BlendColor(Lighten(CurrentTheme().muted, 0.15f), CurrentTheme().text, focusAnimation), layout.alpha),
				11.5f * layout.scale, rows[option].first);
			DrawTextFit(draw, Point(layout, x + 12.0f, y + 18.5f), WithAlpha(CurrentTheme().muted, layout.alpha),
				8.8f * layout.scale, DrawerWidth(layout, 290.0f), rows[option].second);
			DrawTextCentered(draw, Point(layout, x + 372.0f, y + 17.5f),
				WithAlpha(BlendColor(CurrentTheme().text, CurrentTheme().accent, focusAnimation), layout.alpha),
				11.5f * layout.scale, OptionValue(option));
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

	void DrawColorEditorPage(ImDrawList* draw, const RiftLayout& layout)
	{
		DrawHeader(draw, layout);
		const auto theme = CurrentTheme();
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		DrawText(draw, Point(layout, 338, 164), WithAlpha(theme.accent, layout.alpha),
			12.0f * layout.scale, "COLORS");
		DrawTitleText(draw, layout, Point(layout, 337, 188), 24.0f * layout.scale,
			"CUSTOM PALETTE");
		DrawText(draw, Point(layout, 337, 222), WithAlpha(theme.muted, layout.alpha),
			12.0f * layout.scale, "D-PAD OR LEFT STICK TO MOVE THE CURSOR");

		const std::array<std::string_view, 2> targetNames{"ACCENT", "THEME BASE"};
		for (int target = 0; target < 2; ++target)
		{
			const float x = 810.0f + target * 205.0f;
			const ImVec2 minimum = Point(layout, x, 266);
			const ImVec2 maximum = Point(layout, x + 188.0f, 310);
			const bool selected = s_menu.colorEditorTarget == target;
			DrawSettingsChoiceSurface(draw, layout, minimum, maximum, selected ? 1.0f : 0.0f);
			DrawTextCentered(draw, Point(layout, x + 94.0f, 288),
				WithAlpha(selected ? theme.text : theme.muted, layout.alpha),
				12.0f * layout.scale, targetNames[target]);
			if (ClickedRect(layout, minimum, maximum))
			{
				s_menu.colorEditorTarget = target;
				PulseHaptics();
			}
		}

		auto& hsv = s_menu.colorEditorHsv[std::clamp(s_menu.colorEditorTarget, 0, 1)];
		const ImVec2 wheelCenter = Point(layout, 570, 397);
		const float wheelRadius = 124.0f * layout.scale;
		constexpr int wheelSegments = 48;
		constexpr int wheelRings = 8;
		for (int ring = 0; ring < wheelRings; ++ring)
		{
			const float innerRadius = wheelRadius * static_cast<float>(ring) / wheelRings;
			const float outerRadius = wheelRadius * static_cast<float>(ring + 1) / wheelRings;
			const float innerSaturation = static_cast<float>(ring) / wheelRings;
			const float outerSaturation = static_cast<float>(ring + 1) / wheelRings;
			for (int segment = 0; segment < wheelSegments; ++segment)
			{
				const float hue0 = static_cast<float>(segment) / wheelSegments;
				const float hue1 = static_cast<float>(segment + 1) / wheelSegments;
				const float angle0 = static_cast<float>(segment) / wheelSegments * 2.0f * kPi - kPi * 0.5f;
				const float angle1 = static_cast<float>(segment + 1) / wheelSegments * 2.0f * kPi - kPi * 0.5f;
				const ImVec2 inner0{wheelCenter.x + std::cos(angle0) * innerRadius,
					wheelCenter.y + std::sin(angle0) * innerRadius};
				const ImVec2 outer0{wheelCenter.x + std::cos(angle0) * outerRadius,
					wheelCenter.y + std::sin(angle0) * outerRadius};
				const ImVec2 outer1{wheelCenter.x + std::cos(angle1) * outerRadius,
					wheelCenter.y + std::sin(angle1) * outerRadius};
				const ImVec2 inner1{wheelCenter.x + std::cos(angle1) * innerRadius,
					wheelCenter.y + std::sin(angle1) * innerRadius};
				DrawGradientQuad(draw, inner0, outer0, outer1, inner1,
					WithAlpha(HsvColor(hue0, innerSaturation, hsv[2]), layout.alpha),
					WithAlpha(HsvColor(hue0, outerSaturation, hsv[2]), layout.alpha),
					WithAlpha(HsvColor(hue1, outerSaturation, hsv[2]), layout.alpha),
					WithAlpha(HsvColor(hue1, innerSaturation, hsv[2]), layout.alpha));
			}
		}
		draw->AddCircle(wheelCenter, wheelRadius, WithAlpha(theme.border, layout.alpha * 0.78f),
			wheelSegments, 2.0f * layout.scale);
		const ImVec2 cursor = s_menu.colorEditorCursor[std::clamp(s_menu.colorEditorTarget, 0, 1)];
		const ImVec2 marker{wheelCenter.x + cursor.x * wheelRadius,
			wheelCenter.y + cursor.y * wheelRadius};
		draw->AddCircleFilled(marker, 8.0f * layout.scale, WithAlpha(IM_COL32(0, 3, 10, 255), layout.alpha), 20);
		draw->AddCircle(marker, 6.0f * layout.scale, WithAlpha(IM_COL32_WHITE, layout.alpha), 20,
			2.0f * layout.scale);

		const ImVec2 valueMin{wheelCenter.x - wheelRadius, Point(layout, 570, 548).y};
		const ImVec2 valueMax{wheelCenter.x + wheelRadius, Point(layout, 570, 572).y};
		const ImU32 fullValue = HsvColor(hsv[0], hsv[1], 1.0f);
		draw->AddRectFilledMultiColor(valueMin, valueMax,
			WithAlpha(IM_COL32(8, 8, 10, 255), layout.alpha), WithAlpha(fullValue, layout.alpha),
			WithAlpha(fullValue, layout.alpha), WithAlpha(IM_COL32(8, 8, 10, 255), layout.alpha));
		draw->AddRect(valueMin, valueMax, WithAlpha(theme.border, layout.alpha * 0.72f),
			CornerRadius(3.0f) * layout.scale, 0, 1.5f * layout.scale);
		const float valueX = valueMin.x + (valueMax.x - valueMin.x) * hsv[2];
		draw->AddLine({valueX, valueMin.y - 3.0f * layout.scale},
			{valueX, valueMax.y + 3.0f * layout.scale}, WithAlpha(IM_COL32_WHITE, layout.alpha),
			2.0f * layout.scale);
		DrawTextCentered(draw, Point(layout, 570, 591), WithAlpha(theme.muted, layout.alpha),
			10.0f * layout.scale, "X / Y  BRIGHTNESS");

		const ImU32 selectedColor = HsvColor(hsv[0], hsv[1], hsv[2]);
		const ImU32 previewText = ContrastingTextColor(selectedColor);
		const ImU32 previewShadow = ContrastingTextColor(previewText);
		const ImVec2 previewMin = Point(layout, 810, 342);
		const ImVec2 previewMax = Point(layout, 1203, 448);
		draw->AddRectFilled({previewMin.x + 5.0f * layout.scale, previewMin.y + 7.0f * layout.scale},
			{previewMax.x + 5.0f * layout.scale, previewMax.y + 7.0f * layout.scale},
			WithAlpha(IM_COL32(0, 2, 9, 255), layout.alpha * 0.34f), CornerRadius(7.0f) * layout.scale);
		draw->AddRectFilled(previewMin, previewMax, WithAlpha(selectedColor, layout.alpha),
			CornerRadius(7.0f) * layout.scale);
		draw->AddRect(previewMin, previewMax, WithAlpha(Lighten(selectedColor, 0.44f), layout.alpha * 0.82f),
			CornerRadius(7.0f) * layout.scale, 0, 2.0f * layout.scale);
		DrawText(draw, Point(layout, 833.5f, 366.5f), WithAlpha(previewShadow, layout.alpha * 0.28f),
			12.0f * layout.scale, targetNames[s_menu.colorEditorTarget]);
		DrawText(draw, Point(layout, 832, 365), WithAlpha(previewText, layout.alpha),
			12.0f * layout.scale, targetNames[s_menu.colorEditorTarget]);
		DrawText(draw, Point(layout, 834.0f, 399.0f), WithAlpha(previewShadow, layout.alpha * 0.28f),
			22.0f * layout.scale, fmt::format("#{:06X}", ColorToConfig(selectedColor)));
		DrawText(draw, Point(layout, 832, 397), WithAlpha(previewText, layout.alpha),
			22.0f * layout.scale, fmt::format("#{:06X}", ColorToConfig(selectedColor)));
		DrawText(draw, Point(layout, 810, 486), WithAlpha(theme.text, layout.alpha),
			12.0f * layout.scale, "LB / RB  ACCENT / THEME");
		DrawText(draw, Point(layout, 810, 513), WithAlpha(theme.muted, layout.alpha),
			10.5f * layout.scale, "PRESETS ARE STILL AVAILABLE.");

		const bool pointerActive = layout.alpha > 0.9f && ImGui::IsMouseDown(0);
		if (pointerActive)
		{
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const float dx = mouse.x - wheelCenter.x;
			const float dy = mouse.y - wheelCenter.y;
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance <= wheelRadius)
			{
				auto& activeCursor = s_menu.colorEditorCursor[
					std::clamp(s_menu.colorEditorTarget, 0, 1)];
				activeCursor = {dx / wheelRadius, dy / wheelRadius};
				MoveColorEditorCursor(0.0f, 0.0f, false);
				if (ImGui::IsMouseClicked(0))
					PulseHaptics();
			}
			else if (ImGui::IsMouseHoveringRect(valueMin, valueMax))
			{
				hsv[2] = std::clamp((mouse.x - valueMin.x) / (valueMax.x - valueMin.x), 0.12f, 1.0f);
				ApplyColorEditorValue();
				if (ImGui::IsMouseClicked(0))
					PulseHaptics();
			}
		}
	}

	void DrawLibraryOrderPage(ImDrawList* draw, const RiftLayout& layout)
	{
		const auto theme = CurrentTheme();
		DrawHeader(draw, layout);
		const ImVec2 panelMin = Point(layout, 316, 142);
		const ImVec2 panelMax = Point(layout, 1262, 630);
		DrawGlassPanel(draw, layout, panelMin, panelMax, 8.0f, true);
		DrawText(draw, Point(layout, 338, 164), WithAlpha(CurrentTheme().accent, layout.alpha),
			12.0f * layout.scale, "LIBRARY ORDER");
		DrawTitleText(draw, layout, Point(layout, 337, 188), 24.0f * layout.scale,
			"PUT THE NEXT FIGURE ONE MOVE AWAY");
		DrawText(draw, Point(layout, 337, 222), WithAlpha(CurrentTheme().muted, layout.alpha),
			12.0f * layout.scale, "SORT, PIN, OR NARROW THE CASCADE WITHOUT LEAVING THE GAME.");
		DrawText(draw, Point(layout, 1158, 188), WithAlpha(CurrentTheme().accent, layout.alpha),
			13.0f * layout.scale, fmt::format("{} MATCH", s_menu.library.size()));

		const std::array<std::pair<std::string_view, std::string_view>, kLibraryOptionCount> rows{{
			{"SORT BY", "Base order inside the card cascade"},
			{"PIN FIRST", "Favorites, traps, figures, or items"},
			{"ELEMENT FIRST", "Bring one element to the front"},
			{"SHOW", "Limit the library by figure family"},
			{"ELEMENT FILTER", "Show only one element"},
			{"ROW PRESET", "Single, toy types, games, or custom"},
			{"CUSTOM ROW COUNT", "Configure between one and six rows"},
			{"EDIT CUSTOM ROW", "Choose the row changed below"},
			{"ROW FIGURE TYPE", "All figures, Skylanders, traps, or gear"},
			{"ROW GAME", "Show figures from one game in this row"},
			{"ROW ELEMENT", "Optionally narrow this row by element"}}};
		for (int option = 0; option < static_cast<int>(rows.size()); ++option)
		{
			const int column = option / 6;
			const int row = option % 6;
			const float x = 338.0f + column * 454.0f;
			const float y = 264.0f + row * 52.0f;
			const ImVec2 minimum = Point(layout, x, y);
			const ImVec2 maximum = Point(layout, x + 440.0f, y + 44.0f);
			const bool selected = option == s_menu.selectedLibraryOption;
			float& focusAnimation = s_menu.libraryOptionFocusAnimations[option];
			focusAnimation = AnimateFocus(focusAnimation, selected ? 1.0f : 0.0f);
			DrawSettingsChoiceSurface(draw, layout, minimum, maximum, focusAnimation);
			DrawText(draw, Point(layout, x + 12.0f, y + 5.0f),
				WithAlpha(selected ? theme.text : BlendColor(theme.muted, theme.text, 0.40f), layout.alpha),
				11.5f * layout.scale, rows[option].first);
			DrawTextFit(draw, Point(layout, x + 12.0f, y + 23.0f), WithAlpha(theme.muted, layout.alpha),
				8.6f * layout.scale, DrawerWidth(layout, 285.0f), rows[option].second);
			DrawTextFit(draw, Point(layout, x + 304.0f, y + 13.0f),
				WithAlpha(selected ? theme.accent : theme.text, layout.alpha),
				11.0f * layout.scale, DrawerWidth(layout, 122.0f), LibraryOptionValue(option));
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
				DrawActionHint(draw, layout, 430, "Y", "EDIT TAG");
			if (s_menu.details.fromLibrary)
				DrawActionHint(draw, layout, 560, "X", "DELETE FILE");
			else if (s_menu.details.actualSlot >= 0)
				DrawActionHint(draw, layout, 430, "X", "REMOVE");
			if (s_menu.details.fromLibrary)
				DrawActionHint(draw, layout, 720, "RB", "FAVORITE");
			DrawActionHint(draw, layout, s_menu.details.fromLibrary ? 865.0f : 570.0f, "B", "BACK");
			if (FooterClicked(layout, 310, 420) && s_menu.details.fromLibrary)
			{
				const auto* definition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant);
				BeginPlacement({s_menu.details.filePath, s_menu.details.id, s_menu.details.variant,
					s_menu.details.name, s_menu.details.imagePath,
					definition ? definition->element : skylander_ui::FigureElement::Unknown,
					definition ? definition->type : skylander_ui::FigureType::Unknown});
			}
			else if (FooterClicked(layout, 422, 545) && s_menu.details.fromLibrary)
				OpenLabelKeyboard();
			else if (FooterClicked(layout, s_menu.details.fromLibrary ? 547.0f : 422.0f,
				s_menu.details.fromLibrary ? 705.0f : 535.0f))
			{
				if (s_menu.details.fromLibrary)
					RequestDeleteDetailFigure();
				else if (s_menu.details.actualSlot >= 0)
					RemoveActualSlot(s_menu.details.actualSlot);
			}
			else if (FooterClicked(layout, 707, 850) && s_menu.details.fromLibrary)
				ToggleFavoritePath(s_menu.details.filePath, s_menu.details.name);
			else if (FooterClicked(layout, s_menu.details.fromLibrary ? 852.0f : 562.0f,
				s_menu.details.fromLibrary ? 960.0f : 685.0f))
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
		else if (s_menu.page == RiftPage::ColorEditor)
		{
			DrawActionHint(draw, layout, 318, "A", "DONE");
			DrawActionHint(draw, layout, 420, "X", "DARKER");
			DrawActionHint(draw, layout, 538, "Y", "BRIGHTER");
			DrawActionHint(draw, layout, 670, "LB", "ACCENT");
			DrawActionHint(draw, layout, 790, "RB", "THEME");
			DrawActionHint(draw, layout, 905, "B", "BACK");
			if (FooterClicked(layout, 310, 410))
			{
				FinishColorEditor();
				PulseHaptics(UiSound::Confirm);
			}
			else if (FooterClicked(layout, 412, 528))
				AdjustColorEditorBrightness(-0.035f);
			else if (FooterClicked(layout, 530, 660))
				AdjustColorEditorBrightness(0.035f);
			else if (FooterClicked(layout, 662, 780))
			{
				s_menu.colorEditorTarget = 0;
				PulseHaptics();
			}
			else if (FooterClicked(layout, 782, 895))
			{
				s_menu.colorEditorTarget = 1;
				PulseHaptics();
			}
			else if (FooterClicked(layout, 897, 1015))
			{
				FinishColorEditor();
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
				else if (s_menu.forgeToolbarSelection == 3)
					CycleForgeElementFilter();
				else
					CycleForgeGameFilter();
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
		const auto theme = CurrentTheme();
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
		draw->AddRectFilled(minimum, maximum,
			WithAlpha(BlendColor(theme.background, theme.surface, 0.72f), layout.alpha * fade * 0.96f),
			9.0f * layout.scale);
		draw->AddRectFilled({minimum.x + DrawerWidth(layout, 3.0f), minimum.y + 9.0f * layout.scale},
			{minimum.x + DrawerWidth(layout, 6.0f), maximum.y - 9.0f * layout.scale},
			WithAlpha(theme.accent, layout.alpha * fade * 0.82f), 2.0f * layout.scale);
		draw->AddCircleFilled({minimum.x + DrawerWidth(layout, 15.0f), center.y}, 2.6f * layout.scale,
			WithAlpha(theme.secondary, layout.alpha * fade * 0.86f), 12);
		draw->AddRect(minimum, maximum, WithAlpha(theme.border, layout.alpha * fade * 0.72f),
			9.0f * layout.scale, 0, 1.2f * layout.scale);
		DrawTextFit(draw, {minimum.x + DrawerWidth(layout, 22.0f), center.y - 8.0f * layout.scale},
			WithAlpha(theme.text, layout.alpha * fade),
			14.0f * layout.scale, width - DrawerWidth(layout, 44.0f), s_menu.toast);
	}

	void DrawOnScreenKeyboard(ImDrawList* draw, const RiftLayout& layout)
	{
		if (s_menu.searchTarget == SearchTarget::None)
			return;
		s_menu.keyboardVisibility = AnimateFocus(s_menu.keyboardVisibility, 1.0f);
		RiftLayout keyboardLayout = layout;
		keyboardLayout.alpha *= Clamp01(s_menu.keyboardVisibility);
		keyboardLayout.origin.y += (1.0f - EaseOutCubic(s_menu.keyboardVisibility)) * 18.0f * layout.scale;
		const auto theme = CurrentTheme();
		draw->AddRectFilled(Point(layout, kDrawerX, 0), Point(layout, 1280, 720),
			WithAlpha(IM_COL32(0, 2, 9, 255), layout.alpha * s_menu.keyboardVisibility * 0.68f));
		const ImVec2 panelMin = Point(keyboardLayout, 376, 142);
		const ImVec2 panelMax = Point(keyboardLayout, 1248, 620);
		DrawGlassPanel(draw, keyboardLayout, panelMin, panelMax, 9.0f, true);
		const auto* detailDefinition = s_menu.catalog.Find(s_menu.details.id, s_menu.details.variant);
		const bool trapLabel = s_menu.searchTarget == SearchTarget::Label && detailDefinition &&
			detailDefinition->type == skylander_ui::FigureType::Trap;
		const std::string_view title = s_menu.searchTarget == SearchTarget::Create ? "SEARCH FIGURES" :
			(s_menu.searchTarget == SearchTarget::Label ?
				(trapLabel ? "LABEL THIS TRAP" : "LABEL THIS FIGURE") : "SEARCH COLLECTION");
		DrawTitleText(draw, keyboardLayout, Point(keyboardLayout, 412, 169),
			24.0f * keyboardLayout.scale, title);
		DrawTextRightAligned(draw, Point(keyboardLayout, 1216, 176), WithAlpha(theme.muted, keyboardLayout.alpha),
			10.0f * keyboardLayout.scale, "CONTROLLER + PC KEYBOARD");

		const ImVec2 fieldMin = Point(keyboardLayout, 412, 210);
		const ImVec2 fieldMax = Point(keyboardLayout, 1216, 258);
		DrawAnimatedControlSurface(draw, keyboardLayout, fieldMin, fieldMax, 0.72f, true, theme.accent);
		const auto& searchText = ActiveSearchText();
		const std::string_view value = searchText[0] ? std::string_view(searchText.data()) :
			(s_menu.searchTarget == SearchTarget::Label ? std::string_view("No label yet") :
				std::string_view("Search is empty"));
		DrawTextFit(draw, Point(keyboardLayout, 433, 224),
			WithAlpha(searchText[0] ? theme.text : theme.muted, keyboardLayout.alpha),
			15.0f * keyboardLayout.scale, DrawerWidth(keyboardLayout, 760.0f), value);

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
				const ImVec2 minimum = Point(keyboardLayout, startX + column * (keyWidth + gap), y);
				const ImVec2 maximum = Point(keyboardLayout, startX + column * (keyWidth + gap) + keyWidth, y + 42.0f);
				const bool selected = row == s_menu.keyboardRow && column == s_menu.keyboardColumn;
				float& focus = s_menu.keyboardFocusAnimations[row][column];
				focus = AnimateFocus(focus, selected ? 1.0f : 0.0f);
				ImU32 accent = theme.accent;
				if (row == 4)
				{
					static constexpr std::array<ImU32, 4> actionColors{
						IM_COL32(91, 216, 255, 255), IM_COL32(255, 190, 74, 255),
						IM_COL32(255, 102, 91, 255), IM_COL32(91, 231, 151, 255)};
					accent = BlendColor(actionColors[column], theme.accent, 0.16f);
				}
				DrawAnimatedControlSurface(draw, keyboardLayout, minimum, maximum, focus, false, accent);
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
					WithAlpha(selected ? accent : theme.text, keyboardLayout.alpha),
					(row == 4 ? 11.0f : 14.0f) * keyboardLayout.scale, label);
				if (ClickedRect(keyboardLayout, minimum, maximum))
				{
					s_menu.keyboardRow = row;
					s_menu.keyboardColumn = column;
					ActivateKeyboardKey();
				}
			}
		}
		DrawTextCentered(draw, Point(keyboardLayout, 814, 589), WithAlpha(theme.muted, keyboardLayout.alpha),
			10.5f * keyboardLayout.scale, s_menu.searchTarget == SearchTarget::Label ?
				"A  SELECT    X  BACKSPACE    Y  SPACE    START  SAVE    B  CANCEL" :
				"A  SELECT    X  BACKSPACE    Y  SPACE    START  DONE    B  CLOSE");
	}

	void MoveLibrarySelection(int delta)
	{
		if (s_menu.library.empty())
			return;
		const int old = s_menu.selectedLibrary;
		const auto& rows = BuildCascadeRows();
		if (rows.size() > 1)
		{
			s_menu.selectedCascadeRow = std::clamp(s_menu.selectedCascadeRow, 0,
				static_cast<int>(rows.size()) - 1);
			const auto& indices = rows[s_menu.selectedCascadeRow].indices;
			if (indices.empty())
				return;
			int& position = s_menu.cascadeRowPositions[s_menu.selectedCascadeRow];
			position = (position + delta) % static_cast<int>(indices.size());
			if (position < 0)
				position += static_cast<int>(indices.size());
			s_menu.selectedLibrary = indices[position];
			UpdateSelectedAnimation(old);
			if (old != s_menu.selectedLibrary)
				PulseHaptics();
			return;
		}
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
				s_menu.headerSelection = (s_menu.headerSelection + HeaderItemCount() - 1) % HeaderItemCount();
				PulseHaptics();
			}
			if (next)
			{
				s_menu.headerSelection = (s_menu.headerSelection + 1) % HeaderItemCount();
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
				s_menu.headerSelection = PortalMode();
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

		const auto& cascadeRows = BuildCascadeRows();
		if (up && cascadeRows.size() > 1 && s_menu.selectedCascadeRow > 0)
		{
			const int old = s_menu.selectedLibrary;
			--s_menu.selectedCascadeRow;
			SelectCascadeRowCard(cascadeRows, s_menu.selectedCascadeRow);
			UpdateSelectedAnimation(old);
			PulseHaptics();
		}
		else if (up)
		{
			s_menu.focus = FocusArea::ElementFilter;
			s_menu.selectedElementFilter = std::clamp<sint32>(
				GetConfig().emulated_usb_devices.skylander_element_filter.GetValue(), 0, 11);
			PulseHaptics();
		}
		if (down && cascadeRows.size() > 1 &&
			s_menu.selectedCascadeRow + 1 < static_cast<int>(cascadeRows.size()))
		{
			const int old = s_menu.selectedLibrary;
			++s_menu.selectedCascadeRow;
			SelectCascadeRowCard(cascadeRows, s_menu.selectedCascadeRow);
			UpdateSelectedAnimation(old);
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
			bool start{};
			bool dpadUp{};
			bool dpadDown{};
			bool dpadLeft{};
			bool dpadRight{};
			glm::vec2 axis{};

			bool AnyControlDown() const
			{
				return open || back || up || down || left || right || accept || remove || details || sort ||
					favorite || start;
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
			snapshot.dpadUp = controller->is_up_down();
			snapshot.dpadDown = controller->is_down_down();
			snapshot.dpadLeft = controller->is_left_down();
			snapshot.dpadRight = controller->is_right_down();
			snapshot.up = snapshot.dpadUp;
			snapshot.down = snapshot.dpadDown;
			snapshot.left = snapshot.dpadLeft;
			snapshot.right = snapshot.dpadRight;
			snapshot.accept = controller->is_a_down();
			snapshot.remove = controller->is_x_down();
			snapshot.details = controller->is_y_down();
			snapshot.sort = controller->is_l_down();
			snapshot.favorite = controller->is_r_down();
			snapshot.start = controller->is_start_down();
			snapshot.axis = controller->get_axis();
			snapshot.left |= snapshot.axis.x < -0.55f;
			snapshot.right |= snapshot.axis.x > 0.55f;
			snapshot.up |= snapshot.axis.y > 0.55f;
			snapshot.down |= snapshot.axis.y < -0.55f;
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
		bool startDown = false;
		glm::vec2 colorEditorAxis{};
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
			startDown = snapshot.start;
			colorEditorAxis = snapshot.axis;
			if (s_menu.page == RiftPage::ColorEditor)
			{
				upDown = snapshot.dpadUp;
				downDown = snapshot.dpadDown;
				leftDown = snapshot.dpadLeft;
				rightDown = snapshot.dpadRight;
			}
		}
		if (s_menu.requestedOpen && s_menu.ownerController >= 0 &&
			snapshots[s_menu.ownerController].AnyControlDown())
		{
			s_menu.mouseNavigationActive = false;
			if (s_menu.searchTarget == SearchTarget::None)
				s_textInputTarget.store(0, std::memory_order_release);
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
				else if (startDown && !s_menu.startWasDown)
					CommitSearchKeyboard();
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
				else if (s_menu.page == RiftPage::ColorEditor)
					FinishColorEditor();
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
				if (detailsDown && !s_menu.detailsWasDown && s_menu.details.fromLibrary)
					OpenLabelKeyboard();
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
							s_menu.forgeToolbarSelection + (next ? 1 : -1), 5);
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
						case 3: CycleForgeElementFilter(); break;
						default: CycleForgeGameFilter(); break;
						}
					}
				}
				else
				{
					if (up && (definitions.empty() || s_menu.selectedDefinition < kLibraryColumns))
					{
						s_menu.forgeToolbarFocused = true;
						if (!definitions.empty())
						{
							static constexpr std::array<int, kLibraryColumns> nearestTool{0, 1, 2, 4};
							s_menu.forgeToolbarSelection = nearestTool[s_menu.selectedDefinition % kLibraryColumns];
						}
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
			else if (s_menu.page == RiftPage::ColorEditor)
			{
				auto shapeAxis = [](float value) {
					const float magnitude = std::abs(value);
					if (magnitude <= 0.28f)
						return 0.0f;
					return std::copysign(std::pow((magnitude - 0.28f) / 0.72f, 1.35f), value);
				};
				const float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
				const float cursorX = shapeAxis(colorEditorAxis.x) * 0.72f * delta;
				const float cursorY = -shapeAxis(colorEditorAxis.y) * 0.72f * delta;
				if (cursorX != 0.0f || cursorY != 0.0f)
					MoveColorEditorCursor(cursorX, cursorY, false);
				if (previous)
					MoveColorEditorCursor(-0.055f, 0.0f);
				if (next)
					MoveColorEditorCursor(0.055f, 0.0f);
				if (up)
					MoveColorEditorCursor(0.0f, -0.055f);
				if (down)
					MoveColorEditorCursor(0.0f, 0.055f);
				if (removeDown && !s_menu.removeWasDown)
					AdjustColorEditorBrightness(-0.035f);
				if (detailsDown && !s_menu.detailsWasDown)
					AdjustColorEditorBrightness(0.035f);
				if (sortDown && !s_menu.sortWasDown)
				{
					s_menu.colorEditorTarget = 0;
					PulseHaptics();
				}
				if (favoriteDown && !s_menu.favoriteWasDown)
				{
					s_menu.colorEditorTarget = 1;
					PulseHaptics();
				}
				if (acceptDown && !s_menu.acceptWasDown)
				{
					FinishColorEditor();
					PulseHaptics(UiSound::Confirm);
				}
			}
			else if (s_menu.page == RiftPage::Options)
			{
				if (up || down)
				{
					const int column = s_menu.selectedOption / 9;
					const int count = column == 0 ? 9 : kOptionCount - 9;
					const int row = WrapChoice(s_menu.selectedOption % 9 + (down ? 1 : -1), count);
					s_menu.selectedOption = column * 9 + row;
					PulseHaptics();
				}
				if (previous || next)
				{
					const int old = s_menu.selectedOption;
					const int row = s_menu.selectedOption % 9;
					const int column = next ? 1 : 0;
					s_menu.selectedOption = column * 9 + std::min(row, column == 0 ? 8 : kOptionCount - 10);
					if (old != s_menu.selectedOption) PulseHaptics();
				}
				if (acceptDown && !s_menu.acceptWasDown)
					AdjustOption(s_menu.selectedOption, 1);
			}
			else if (s_menu.page == RiftPage::LibraryOrder)
			{
				if (up || down)
				{
					const int column = s_menu.selectedLibraryOption / 6;
					const int count = column == 0 ? 6 : kLibraryOptionCount - 6;
					const int row = WrapChoice(s_menu.selectedLibraryOption % 6 + (down ? 1 : -1), count);
					s_menu.selectedLibraryOption = column * 6 + row;
					PulseHaptics();
				}
				if (previous || next)
				{
					const int old = s_menu.selectedLibraryOption;
					const int row = s_menu.selectedLibraryOption % 6;
					const int column = next ? 1 : 0;
					s_menu.selectedLibraryOption = column * 6 +
						std::min(row, column == 0 ? 5 : kLibraryOptionCount - 7);
					if (old != s_menu.selectedLibraryOption) PulseHaptics();
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
		s_menu.startWasDown = startDown;
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
		PumpArtworkTextureUploads();
		UpdateMouseNavigationMode();
		HandleControllerInput();
		ProcessTextInputQueue();
		if (s_menu.searchTarget == SearchTarget::None && ImGui::IsMouseClicked(0))
			s_textInputTarget.store(0, std::memory_order_release);
		UpdateHaptics();
		ImGui_SetGamepadNavigationEnabled(!s_menu.requestedOpen);

		const float targetVisibility = s_menu.requestedOpen ? 1.0f : 0.0f;
		const int motion = MotionLevel();
		if (motion == 0)
			s_menu.visibility = targetVisibility;
		else
		{
			const float speed = s_menu.requestedOpen ?
				MotionValue(18.0f, 6.0f, 4.6f, 3.7f) :
				MotionValue(24.0f, 9.0f, 7.0f, 5.6f);
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
		s_menu.selectionFlash = motion == 0 ? 0.0f : SmoothTowards(s_menu.selectionFlash, 0.0f,
			MotionValue(15.0f, 7.5f, 5.8f, 4.6f));
		s_menu.portalModeFlash = motion == 0 ? 0.0f : SmoothTowards(s_menu.portalModeFlash, 0.0f,
			MotionValue(10.0f, 5.5f, 4.2f, 3.4f));
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
			MotionValue(15.0f, 7.0f, 5.4f, 4.3f)) : 1.0f;
		if (s_menu.visibility < 0.01f && !s_menu.requestedOpen)
			return;

		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float baseScale = std::max(0.55f, std::min(display.x / 1280.0f, display.y / 720.0f));
		const float easedVisibility = EaseOutCubic(s_menu.visibility);
		const float zoom = motion >= 2 ? 0.975f + easedVisibility * 0.025f :
			(motion == 1 ? 0.995f + easedVisibility * 0.005f : 1.0f);
		RiftLayout layout;
		layout.scale = baseScale * zoom;
		layout.origin = {display.x - 1280.0f * layout.scale, (display.y - 720.0f * layout.scale) * 0.5f};
		layout.alpha = easedVisibility;
		layout.slideX = (1.0f - layout.alpha) * (motion >= 2 ? 82.0f : motion == 1 ? 12.0f : 0.0f);

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
			pageLayout.slideX += (1.0f - pageEase) * (MotionLevel() >= 2 ? 22.0f : 8.0f);
			switch (s_menu.page)
			{
			case RiftPage::Dashboard: DrawDashboard(draw, pageLayout); break;
			case RiftPage::Details: DrawDetailsPage(draw, pageLayout); break;
			case RiftPage::Forge: DrawForgePage(draw, pageLayout); break;
			case RiftPage::Options: DrawOptionsPage(draw, pageLayout); break;
			case RiftPage::ColorEditor: DrawColorEditorPage(draw, pageLayout); break;
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

bool SkylanderQuickMenu_HandleCharacter(unsigned int codepoint)
{
	const int target = s_textInputTarget.load(std::memory_order_acquire);
	if (target < static_cast<int>(SearchTarget::Collection) ||
		target > static_cast<int>(SearchTarget::Label))
		return false;
	const bool text = codepoint >= 32 && codepoint <= 0x10FFFF;
	const bool command = codepoint == 8 || codepoint == 10 || codepoint == 13 ||
		codepoint == 27 || codepoint == 127;
	if (!text && !command)
		return false;
	{
		const std::lock_guard lock(s_textInputMutex);
		s_textInputQueue.emplace_back(target, codepoint);
	}
	return true;
}

bool SkylanderQuickMenu_ConsumeUpdateCheckRequest()
{
	return s_updateCheckRequested.exchange(false, std::memory_order_acq_rel);
}
