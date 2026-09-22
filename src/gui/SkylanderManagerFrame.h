#pragma once

#include <wx/frame.h>

#include "gui/SkylanderCatalog.h"

class wxBitmap;
class wxButton;
class wxChoice;
class SkylanderListBox;
class wxSearchCtrl;
class wxStaticBitmap;
class wxStaticText;

class SkylanderManagerFrame final : public wxFrame
{
  public:
	explicit SkylanderManagerFrame(wxWindow* parent);

  private:
	void ReloadCatalog();
	void ApplyFilter();
	void ShowSelectedFigure();
	void PlaceSelectedFigure();
	void RemoveSelectedSlot();
	void UpdatePortalSlots();
	void SetPortalMode(bool virtualPortal);
	void ApplyDarkTheme(wxWindow* window);

	skylander_ui::SkylanderCatalog m_catalog;
	std::vector<size_t> m_visibleFigures;
	std::array<std::optional<std::tuple<uint8, uint16, uint16>>, 16> m_portalFigures;
	wxSearchCtrl* m_search{};
	SkylanderListBox* m_collectionList{};
	wxStaticBitmap* m_artwork{};
	wxStaticText* m_name{};
	wxStaticText* m_details{};
	wxChoice* m_targetSlot{};
	SkylanderListBox* m_portalList{};
	wxButton* m_physicalButton{};
	wxButton* m_virtualButton{};
};
