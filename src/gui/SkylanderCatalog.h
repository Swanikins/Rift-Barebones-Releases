#pragma once

#include <optional>

namespace skylander_ui
{
	enum class FigureElement : uint8
	{
		Unknown,
		Air,
		Earth,
		Fire,
		Water,
		Magic,
		Tech,
		Life,
		Undead,
		Light,
		Dark,
		Kaos,
		None,
	};

	enum class FigureType : uint8
	{
		Unknown,
		Skylander,
		Trap,
		Vehicle,
		Item,
		CreationCrystal,
		RacingDriver,
	};

	struct FigureDefinition
	{
		uint16 id{};
		uint16 variant{};
		std::string name;
		fs::path imagePath;
		FigureElement element{FigureElement::Unknown};
		FigureType type{FigureType::Unknown};
	};

	struct CollectionFigure
	{
		fs::path filePath;
		uint16 id{};
		uint16 variant{};
		std::string name;
		fs::path imagePath;
		FigureElement element{FigureElement::Unknown};
		FigureType type{FigureType::Unknown};
	};

	class SkylanderCatalog
	{
	  public:
		bool Reload();
		const std::vector<FigureDefinition>& GetDefinitions() const { return m_definitions; }
		const std::vector<FigureDefinition>& GetCreatableDefinitions() const { return m_creatableDefinitions; }
		const std::vector<CollectionFigure>& GetCollection() const { return m_collection; }
		const fs::path& GetAssetsPath() const { return m_assetsPath; }
		const std::string& GetError() const { return m_error; }
		const FigureDefinition* Find(uint16 id, uint16 variant) const;
		std::optional<fs::path> FindAssetsPath() const;

	  private:
		bool LoadDefinitions();
		void LoadMetadata();
		void ScanCollection();

		fs::path m_assetsPath;
		std::vector<FigureDefinition> m_definitions;
		std::vector<FigureDefinition> m_creatableDefinitions;
		std::vector<CollectionFigure> m_collection;
		std::string m_error;
	};
}
