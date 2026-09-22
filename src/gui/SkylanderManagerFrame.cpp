#include "SkylanderManagerFrame.h"

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/listbox.h>
#include <wx/log.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/vlbox.h>

#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Cafe/OS/libs/nsyshid/nsyshid.h"
#include "Common/FileStream.h"
#include "config/CemuConfig.h"
#include "gui/helpers/wxHelpers.h"
#include "gui/wxHelper.h"

namespace
{
	const wxColour kBackground(18, 21, 28);
	const wxColour kPanel(27, 31, 41);
	const wxColour kControl(38, 44, 57);
	const wxColour kText(238, 241, 247);
	const wxColour kMuted(157, 166, 184);
	const wxColour kAccent(84, 129, 255);

	wxBitmap MakeArtworkPlaceholder()
	{
		wxBitmap canvas(300, 300, 32);
		wxMemoryDC dc(canvas);
		dc.SetBackground(wxBrush(kPanel));
		dc.Clear();
		dc.SetTextForeground(kMuted);
		const wxString label = _("Artwork unavailable");
		const wxSize extent = dc.GetTextExtent(label);
		dc.DrawText(label, (300 - extent.x) / 2, (300 - extent.y) / 2);
		return canvas;
	}
}

class SkylanderListBox final : public wxVListBox
{
  public:
	SkylanderListBox(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
	{
		SetBackgroundColour(kControl);
		SetForegroundColour(kText);
	}
	void Clear() { m_items.clear(); SetItemCount(0); RefreshAll(); }
	void Append(const wxString& text) { m_items.push_back(text); SetItemCount(m_items.size()); RefreshRow(m_items.size() - 1); }
  protected:
	wxCoord OnMeasureItem(size_t) const override { return FromDIP(30); }
	void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override
	{
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(IsSelected(n) ? kAccent : (n % 2 ? wxColour(34, 39, 51) : kControl)));
		dc.DrawRectangle(rect);
	}
	void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override
	{
		dc.SetTextForeground(kText);
		dc.DrawText(m_items[n], rect.x + FromDIP(10), rect.y + (rect.height - dc.GetCharHeight()) / 2);
	}
  private:
	std::vector<wxString> m_items;
};

SkylanderManagerFrame::SkylanderManagerFrame(wxWindow* parent)
	: wxFrame(parent, wxID_ANY, _("Skylander Manager"), wxDefaultPosition, wxSize(1120, 720),
		wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL)
{
	SetMinSize(wxSize(920, 680));
	SetBackgroundColour(kBackground);

	auto* root = new wxBoxSizer(wxVERTICAL);
	auto* header = new wxBoxSizer(wxHORIZONTAL);
	auto* titleBlock = new wxBoxSizer(wxVERTICAL);
	auto* title = new wxStaticText(this, wxID_ANY, _("SKYLANDER MANAGER"));
	wxFont titleFont = title->GetFont();
	titleFont.SetPointSize(18);
	titleFont.SetWeight(wxFONTWEIGHT_BOLD);
	title->SetFont(titleFont);
	title->SetForegroundColour(kText);
	titleBlock->Add(title);
	auto* subtitle = new wxStaticText(this, wxID_ANY, _("Collection, portal slots, and live portal switching"));
	subtitle->SetForegroundColour(kMuted);
	titleBlock->Add(subtitle, 0, wxTOP, 3);
	header->Add(titleBlock, 1, wxALIGN_CENTER_VERTICAL);

	m_physicalButton = new wxButton(this, wxID_ANY, _("Physical Portal"));
	m_virtualButton = new wxButton(this, wxID_ANY, _("Virtual Portal"));
	header->Add(m_physicalButton, 0, wxRIGHT, 8);
	header->Add(m_virtualButton);
	m_physicalButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetPortalMode(false); });
	m_virtualButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetPortalMode(true); });
	root->Add(header, 0, wxEXPAND | wxALL, 20);
	root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 20);

	auto* content = new wxBoxSizer(wxHORIZONTAL);

	auto* collectionPanel = new wxPanel(this);
	collectionPanel->SetBackgroundColour(kPanel);
	auto* collectionSizer = new wxBoxSizer(wxVERTICAL);
	auto* collectionTitle = new wxStaticText(collectionPanel, wxID_ANY, _("YOUR COLLECTION"));
	collectionTitle->SetForegroundColour(kMuted);
	collectionSizer->Add(collectionTitle, 0, wxALL, 14);
	m_search = new wxSearchCtrl(collectionPanel, wxID_ANY);
	m_search->SetHint(_("Search Skylanders..."));
	collectionSizer->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
	m_collectionList = new SkylanderListBox(collectionPanel);
	collectionSizer->Add(m_collectionList, 1, wxEXPAND | wxLEFT | wxRIGHT, 14);
	auto* reload = new wxButton(collectionPanel, wxID_ANY, _("Refresh Collection"));
	auto* create = new wxButton(collectionPanel, wxID_ANY, _("+  Create a Skylander File"));
	collectionSizer->Add(create, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
	collectionSizer->Add(reload, 0, wxEXPAND | wxALL, 14);
	collectionPanel->SetSizer(collectionSizer);
	content->Add(collectionPanel, 4, wxEXPAND | wxRIGHT, 12);

	auto* previewPanel = new wxPanel(this);
	previewPanel->SetBackgroundColour(kPanel);
	auto* previewSizer = new wxBoxSizer(wxVERTICAL);
	m_artwork = new wxStaticBitmap(previewPanel, wxID_ANY, wxBitmap(300, 300));
	m_artwork->SetMinSize(wxSize(300, 300));
	previewSizer->Add(m_artwork, 0, wxALIGN_CENTER | wxALL, 14);
	m_name = new wxStaticText(previewPanel, wxID_ANY, _("Choose a Skylander"));
	wxFont nameFont = m_name->GetFont();
	nameFont.SetPointSize(15);
	nameFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_name->SetFont(nameFont);
	m_name->SetForegroundColour(kText);
	previewSizer->Add(m_name, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 16);
	m_details = new wxStaticText(previewPanel, wxID_ANY, _("Artwork and figure details appear here"));
	m_details->SetForegroundColour(kMuted);
	previewSizer->Add(m_details, 0, wxALIGN_CENTER | wxALL, 10);
	previewSizer->AddStretchSpacer(1);
	m_targetSlot = new wxChoice(previewPanel, wxID_ANY);
	for (unsigned i = 0; i < 16; ++i)
		m_targetSlot->Append(wxString::Format(_("Portal Slot %u"), i + 1));
	m_targetSlot->SetSelection(0);
	previewSizer->Add(m_targetSlot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);
	auto* place = new wxButton(previewPanel, wxID_ANY, _("Place on Virtual Portal"));
	previewSizer->Add(place, 0, wxEXPAND | wxALL, 18);
	previewPanel->SetSizer(previewSizer);
	content->Add(previewPanel, 5, wxEXPAND | wxRIGHT, 12);

	auto* portalPanel = new wxPanel(this);
	portalPanel->SetBackgroundColour(kPanel);
	auto* portalSizer = new wxBoxSizer(wxVERTICAL);
	auto* portalTitle = new wxStaticText(portalPanel, wxID_ANY, _("VIRTUAL PORTAL"));
	portalTitle->SetForegroundColour(kMuted);
	portalSizer->Add(portalTitle, 0, wxALL, 14);
	m_portalList = new SkylanderListBox(portalPanel);
	portalSizer->Add(m_portalList, 1, wxEXPAND | wxLEFT | wxRIGHT, 14);
	auto* remove = new wxButton(portalPanel, wxID_ANY, _("Remove Selected"));
	portalSizer->Add(remove, 0, wxEXPAND | wxALL, 14);
	portalPanel->SetSizer(portalSizer);
	content->Add(portalPanel, 4, wxEXPAND);

	root->Add(content, 1, wxEXPAND | wxALL, 20);
	SetSizer(root);
	ApplyDarkTheme(this);

	m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { ApplyFilter(); });
	m_collectionList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowSelectedFigure(); });
	reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ReloadCatalog(); });
	create->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		wxArrayString choices;
		const auto& definitions = m_catalog.GetCreatableDefinitions();
		if (definitions.empty())
		{
			wxMessageBox(_("No figures with valid artwork are available. Check the assets folder."),
				_("Create Skylander"), wxOK | wxICON_INFORMATION, this);
			return;
		}
		for (const auto& figure : definitions) choices.Add(wxHelper::FromUtf8(figure.name));
		wxSingleChoiceDialog picker(this, _("Choose the character or item to create."), _("Create Skylander File"), choices);
		ApplyDarkTheme(&picker);
		if (picker.ShowModal() != wxID_OK || picker.GetSelection() == wxNOT_FOUND) return;
		const auto& definition = definitions[picker.GetSelection()];
		wxString filename = wxHelper::FromUtf8(definition.name);
		for (const wxChar c : wxString("<>:\"/\\|?*")) filename.Replace(wxString(c), "-");
		const fs::path folder = _utf8ToPath(GetConfig().emulated_usb_devices.skylander_collection_path.GetValue());
		std::error_code ec; fs::create_directories(folder, ec);
		fs::path output = folder / _utf8ToPath(filename.ToStdString() + ".sky");
		for (unsigned copy = 2; fs::exists(output); ++copy)
			output = folder / _utf8ToPath(fmt::format("{} ({}).sky", filename.ToStdString(), copy));
		if (!nsyshid::g_skyportal.CreateSkylander(output, definition.id, definition.variant))
			wxMessageBox(_("The Skylander file could not be created."), _("Create Skylander"), wxOK | wxICON_ERROR, this);
		else ReloadCatalog();
	});
	place->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlaceSelectedFigure(); });
	remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveSelectedSlot(); });

	const bool virtualPortal = GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue();
	m_virtualButton->SetBackgroundColour(virtualPortal ? kAccent : kControl);
	m_physicalButton->SetBackgroundColour(virtualPortal ? kControl : kAccent);
	ReloadCatalog();
	UpdatePortalSlots();
	Centre();
}

void SkylanderManagerFrame::ApplyDarkTheme(wxWindow* window)
{
	window->SetBackgroundColour(window == this ? kBackground : kPanel);
	window->SetForegroundColour(kText);
	for (auto* child : window->GetChildren())
	{
		if (dynamic_cast<wxButton*>(child) || dynamic_cast<wxChoice*>(child) ||
			dynamic_cast<wxListBox*>(child) || dynamic_cast<wxSearchCtrl*>(child))
			child->SetBackgroundColour(kControl);
		child->SetForegroundColour(kText);
		ApplyDarkTheme(child);
	}
}

void SkylanderManagerFrame::ReloadCatalog()
{
	if (!m_catalog.Reload())
		wxMessageBox(wxHelper::FromUtf8(m_catalog.GetError()), _("Skylander Manager"), wxOK | wxICON_ERROR, this);
	ApplyFilter();
}

void SkylanderManagerFrame::ApplyFilter()
{
	const wxString search = m_search->GetValue().Lower();
	m_collectionList->Clear();
	m_visibleFigures.clear();
	const auto& figures = m_catalog.GetCollection();
	for (size_t i = 0; i < figures.size(); ++i)
	{
		const wxString name = wxHelper::FromUtf8(figures[i].name);
		if (!search.empty() && !name.Lower().Contains(search)) continue;
		m_visibleFigures.push_back(i);
		m_collectionList->Append(name);
	}
	if (!m_visibleFigures.empty())
	{
		m_collectionList->SetSelection(0);
		ShowSelectedFigure();
	}
}

void SkylanderManagerFrame::ShowSelectedFigure()
{
	const int selected = m_collectionList->GetSelection();
	if (selected == wxNOT_FOUND || static_cast<size_t>(selected) >= m_visibleFigures.size()) return;
	const auto& figure = m_catalog.GetCollection()[m_visibleFigures[selected]];
	m_name->SetLabel(wxHelper::FromUtf8(figure.name));
	m_details->SetLabel(wxString::Format("ID %04X  |  Variant %04X", figure.id, figure.variant));
	m_artwork->SetBitmap(MakeArtworkPlaceholder());
	std::error_code imageError;
	if (!figure.imagePath.empty() && fs::is_regular_file(figure.imagePath, imageError))
	{
		wxLogNull suppressImageErrors;
		wxImage image;
		if (image.LoadFile(wxHelper::FromPath(figure.imagePath)) && image.IsOk())
		{
			const double scale = std::min(300.0 / image.GetWidth(), 300.0 / image.GetHeight());
			image.Rescale(std::max(1, (int)std::round(image.GetWidth() * scale)),
				std::max(1, (int)std::round(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
			wxBitmap canvas(300, 300, 32);
			{
				wxMemoryDC dc(canvas);
				dc.SetBackground(wxBrush(kPanel)); dc.Clear();
				dc.DrawBitmap(wxBitmap(image), (300 - image.GetWidth()) / 2, (300 - image.GetHeight()) / 2, true);
			}
			m_artwork->SetBitmap(canvas);
		}
	}
}

void SkylanderManagerFrame::PlaceSelectedFigure()
{
	const int selected = m_collectionList->GetSelection();
	const int target = m_targetSlot->GetSelection();
	if (selected == wxNOT_FOUND || target == wxNOT_FOUND) return;
	const auto& figure = m_catalog.GetCollection()[m_visibleFigures[selected]];
	std::unique_ptr<FileStream> file(FileStream::openFile2(figure.filePath, true));
	if (!file) return;
	std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data{};
	if (file->readData(data.data(), data.size()) != data.size()) return;
	if (m_portalFigures[target])
		nsyshid::g_skyportal.RemoveSkylander(std::get<0>(*m_portalFigures[target]));
	const uint8 portalSlot = nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(file));
	if (portalSlot == 0xFF) return;
	m_portalFigures[target] = std::tuple(portalSlot, figure.id, figure.variant);
	UpdatePortalSlots();
}

void SkylanderManagerFrame::RemoveSelectedSlot()
{
	const int selected = m_portalList->GetSelection();
	if (selected == wxNOT_FOUND || !m_portalFigures[selected]) return;
	nsyshid::g_skyportal.RemoveSkylander(std::get<0>(*m_portalFigures[selected]));
	m_portalFigures[selected].reset();
	UpdatePortalSlots();
}

void SkylanderManagerFrame::UpdatePortalSlots()
{
	m_portalList->Clear();
	for (size_t i = 0; i < m_portalFigures.size(); ++i)
	{
		wxString label = wxString::Format("%02zu   Empty", i + 1);
		if (m_portalFigures[i])
		{
			const auto [portalSlot, id, variant] = *m_portalFigures[i];
			const auto* definition = m_catalog.Find(id, variant);
			label = wxString::Format("%02zu   %s", i + 1,
				definition ? wxHelper::FromUtf8(definition->name) : _("Unknown"));
		}
		m_portalList->Append(label);
	}
}

void SkylanderManagerFrame::SetPortalMode(bool virtualPortal)
{
	const int requestedMode = virtualPortal ? 0 : 1;
	if (GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue() != virtualPortal ||
		GetConfig().emulated_usb_devices.skylander_portal_mode.GetValue() != requestedMode)
	{
		GetConfig().emulated_usb_devices.skylander_portal_mode = requestedMode;
		GetConfig().emulated_usb_devices.emulate_skylander_portal = virtualPortal;
		g_config.Save();
		nsyshid::backend::SetSkylanderPortalEmulation(virtualPortal);
	}
	m_virtualButton->SetBackgroundColour(virtualPortal ? kAccent : kControl);
	m_physicalButton->SetBackgroundColour(virtualPortal ? kControl : kAccent);
}
