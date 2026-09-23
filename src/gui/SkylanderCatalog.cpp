#include "SkylanderCatalog.h"

#include <rapidjson/document.h>

#include "Common/FileStream.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

namespace skylander_ui
{
	static std::string NormalizeMetadataValue(std::string_view text)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (const unsigned char c : text)
		{
			if (std::isalnum(c))
				normalized.push_back(static_cast<char>(std::tolower(c)));
		}
		return normalized;
	}

	static FigureElement ParseElement(std::string_view text)
	{
		const std::string value = NormalizeMetadataValue(text);
		if (value == "air") return FigureElement::Air;
		if (value == "earth") return FigureElement::Earth;
		if (value == "fire") return FigureElement::Fire;
		if (value == "water") return FigureElement::Water;
		if (value == "magic") return FigureElement::Magic;
		if (value == "tech") return FigureElement::Tech;
		if (value == "life") return FigureElement::Life;
		if (value == "undead") return FigureElement::Undead;
		if (value == "light") return FigureElement::Light;
		if (value == "dark") return FigureElement::Dark;
		if (value == "kaos") return FigureElement::Kaos;
		if (value == "none") return FigureElement::None;
		return FigureElement::Unknown;
	}

	static FigureType ParseFigureType(std::string_view text)
	{
		const std::string value = NormalizeMetadataValue(text);
		if (value == "skylander") return FigureType::Skylander;
		if (value == "trap") return FigureType::Trap;
		if (value == "vehicle") return FigureType::Vehicle;
		if (value == "item") return FigureType::Item;
		if (value == "creationcrystal") return FigureType::CreationCrystal;
		if (value == "racingdriver") return FigureType::RacingDriver;
		return FigureType::Unknown;
	}

	static std::optional<uint16> ParseHexId(std::string_view text)
	{
		if (text.empty() || text.size() > 8)
			return std::nullopt;
		uint32 value = 0;
		for (char c : text)
		{
			value <<= 4;
			if (c >= '0' && c <= '9') value |= c - '0';
			else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
			else return std::nullopt;
		}
		if (value > 0xFFFF)
			return std::nullopt;
		return static_cast<uint16>(value);
	}

	static fs::path ExistingArtworkPath(const fs::path& path)
	{
		std::error_code error;
		return fs::is_regular_file(path, error) ? path : fs::path{};
	}

	std::optional<fs::path> SkylanderCatalog::FindAssetsPath() const
	{
		const fs::path executable = ActiveSettings::GetExecutablePath();
		const std::array candidates{
			executable.parent_path() / "assets",
			executable.parent_path().parent_path() / "assets",
			executable.parent_path().parent_path().parent_path() / "assets",
			fs::current_path() / "assets"
		};
		std::error_code ec;
		for (const auto& candidate : candidates)
		{
			if (fs::is_regular_file(candidate / "skylanders_db.json", ec))
				return candidate;
			ec.clear();
		}
		return std::nullopt;
	}

	bool SkylanderCatalog::Reload()
	{
		m_error.clear();
		m_definitions.clear();
		m_creatableDefinitions.clear();
		m_collection.clear();
		auto assets = FindAssetsPath();
		if (!assets)
		{
			m_error = "Could not find assets/skylanders_db.json";
			return false;
		}
		m_assetsPath = *assets;
		if (!LoadDefinitions())
			return false;
		LoadMetadata();
		for (const auto& definition : m_definitions)
			if (!definition.imagePath.empty())
				m_creatableDefinitions.push_back(definition);
		ScanCollection();
		return true;
	}

	bool SkylanderCatalog::LoadDefinitions()
	{
		auto jsonData = FileStream::LoadIntoMemory(m_assetsPath / "skylanders_db.json");
		if (!jsonData)
		{
			m_error = "Failed to read skylanders_db.json";
			return false;
		}
		rapidjson::Document document;
		document.Parse(reinterpret_cast<const char*>(jsonData->data()), jsonData->size());
		if (document.HasParseError() || !document.IsObject())
		{
			m_error = "skylanders_db.json is invalid";
			return false;
		}

		std::set<std::pair<uint16, uint16>> seen;
		auto addDefinition = [&](uint16 id, uint16 variant, const rapidjson::Value& name, const rapidjson::Value& image) {
			if (!name.IsString() || !image.IsString() || name.GetStringLength() == 0 ||
				!seen.emplace(id, variant).second)
				return;
			const fs::path relative = _utf8ToPath(image.GetString());
			fs::path artwork;
			if (!relative.empty() && !relative.has_root_path() &&
				std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
				artwork = ExistingArtworkPath(m_assetsPath / relative);
			m_definitions.push_back({id, variant, name.GetString(), artwork});
		};
		const auto& knownFigures = nsyshid::SkylanderUSB::GetListSkylanders();
		for (auto figure = document.MemberBegin(); figure != document.MemberEnd(); ++figure)
		{
			auto id = ParseHexId(figure->name.GetString());
			if (!id || !figure->value.IsObject())
				continue;
			const auto baseName = figure->value.FindMember("name");
			const auto baseImage = figure->value.FindMember("base_image");
			if (baseName != figure->value.MemberEnd() && baseImage != figure->value.MemberEnd() &&
				baseName->value.IsString() && baseImage->value.IsString() &&
				knownFigures.find({*id, 0}) != knownFigures.end())
			{
				addDefinition(*id, 0, baseName->value, baseImage->value);
			}
			const auto variants = figure->value.FindMember("variants");
			if (variants == figure->value.MemberEnd() || !variants->value.IsObject())
				continue;
			for (auto variant = variants->value.MemberBegin(); variant != variants->value.MemberEnd(); ++variant)
			{
				auto variantId = ParseHexId(variant->name.GetString());
				if (!variantId || !variant->value.IsObject())
					continue;
				const auto name = variant->value.FindMember("name");
				const auto image = variant->value.FindMember("image");
				if (name == variant->value.MemberEnd() || image == variant->value.MemberEnd() ||
					!name->value.IsString() || !image->value.IsString())
					continue;
				addDefinition(*id, *variantId, name->value, image->value);
			}
		}
		std::sort(m_definitions.begin(), m_definitions.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.name < rhs.name;
		});
		if (m_definitions.empty())
		{
			m_error = "skylanders_db.json contains no valid hexadecimal figure/variant entries";
			return false;
		}
		return true;
	}

	void SkylanderCatalog::LoadMetadata()
	{
		auto jsonData = FileStream::LoadIntoMemory(m_assetsPath / "skylander_metadata.json");
		if (!jsonData)
			return;

		rapidjson::Document document;
		document.Parse(reinterpret_cast<const char*>(jsonData->data()), jsonData->size());
		if (document.HasParseError() || !document.IsObject())
			return;

		const auto figures = document.FindMember("figures");
		if (figures == document.MemberEnd() || !figures->value.IsObject())
			return;

		for (auto metadata = figures->value.MemberBegin(); metadata != figures->value.MemberEnd(); ++metadata)
		{
			const auto id = ParseHexId(metadata->name.GetString());
			if (!id || !metadata->value.IsObject())
				continue;

			FigureElement element = FigureElement::Unknown;
			FigureType type = FigureType::Unknown;
			const auto elementMember = metadata->value.FindMember("element");
			if (elementMember != metadata->value.MemberEnd() && elementMember->value.IsString())
				element = ParseElement(elementMember->value.GetString());
			const auto kindMember = metadata->value.FindMember("kind");
			if (kindMember != metadata->value.MemberEnd() && kindMember->value.IsString())
				type = ParseFigureType(kindMember->value.GetString());

			for (auto& definition : m_definitions)
			{
				if (definition.id != *id)
					continue;
				definition.element = element;
				definition.type = type;
			}
		}
	}

	const FigureDefinition* SkylanderCatalog::Find(uint16 id, uint16 variant) const
	{
		const auto it = std::find_if(m_definitions.begin(), m_definitions.end(), [id, variant](const auto& entry) {
			return entry.id == id && entry.variant == variant;
		});
		if (it != m_definitions.end())
			return &*it;

		const FigureDefinition* onlyMatch = nullptr;
		const FigureDefinition* baseMatch = nullptr;
		int matches = 0;
		for (const auto& definition : m_definitions)
		{
			if (definition.id != id)
				continue;
			++matches;
			onlyMatch = &definition;
			if (variant == 0 && definition.variant != 0 && (definition.variant & 0x0FFF) == 0 &&
				(!baseMatch || definition.variant < baseMatch->variant))
				baseMatch = &definition;
		}
		if (baseMatch)
			return baseMatch;
		return matches == 1 ? onlyMatch : nullptr;
	}

	void SkylanderCatalog::ScanCollection()
	{
		const fs::path collectionPath = _utf8ToPath(GetConfig().emulated_usb_devices.skylander_collection_path.GetValue());
		std::error_code ec;
		if (!fs::is_directory(collectionPath, ec))
			return;
		for (fs::recursive_directory_iterator it(collectionPath, fs::directory_options::skip_permission_denied, ec), end;
			 it != end; it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (it->is_directory(ec))
			{
				auto name = _pathToUtf8(it->path().filename());
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
				if (name == ".rift-trash" || name == "backups") it.disable_recursion_pending();
				continue;
			}
			if (!it->is_regular_file(ec)) continue;
			const uintmax_t fileSize = it->file_size(ec);
			if (ec || fileSize != nsyshid::SKY_FIGURE_SIZE) { ec.clear(); continue; }
			auto extension = _pathToUtf8(it->path().extension());
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
			if (extension != ".sky" && extension != ".bin" && extension != ".dump" && extension != ".dmp") continue;
			auto data = FileStream::LoadIntoMemory(it->path());
			if (!data || data->size() != nsyshid::SKY_FIGURE_SIZE) continue;
			const uint8 bcc = (*data)[0] ^ (*data)[1] ^ (*data)[2] ^ (*data)[3];
			if ((*data)[4] != bcc || (*data)[5] != 0x81 || (*data)[6] != 0x01 || (*data)[7] != 0x0F)
				continue;
			const uint16 id = (*data)[0x10] | (uint16((*data)[0x11]) << 8);
			const uint16 variant = (*data)[0x1C] | (uint16((*data)[0x1D]) << 8);
			const auto* definition = Find(id, variant);
			std::string displayName = definition ? definition->name : _pathToUtf8(it->path().stem());
			if (displayName.empty())
				displayName = "Unknown figure";
			m_collection.push_back({it->path(), id, variant,
				displayName,
				definition ? ExistingArtworkPath(definition->imagePath) : fs::path{},
				definition ? definition->element : FigureElement::Unknown,
				definition ? definition->type : FigureType::Unknown});
		}
		std::sort(m_collection.begin(), m_collection.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.name < rhs.name;
		});
	}
}
