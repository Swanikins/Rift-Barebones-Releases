void OpenLibraryOrder();

void HubSelect(int index)
{
    if (s_menu.library.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(s_menu.library.size()) - 1);
    const int old = s_menu.selectedLibrary;
    s_menu.selectedLibrary = index;
    UpdateSelectedAnimation(old);
    if (old != index) PulseHaptics();
}

void NavigateRiftHub(bool previous, bool next, bool up, bool down)
{
    const auto oldFocus = s_menu.focus;
    if (s_menu.focus == FocusArea::Header)
    {
        const int old = s_menu.headerSelection;
        s_menu.headerSelection = std::clamp(old + (next ? 1 : 0) - (previous ? 1 : 0), 0, HeaderItemCount() - 1);
        if (old != s_menu.headerSelection) PulseHaptics();
        if (down) s_menu.focus = FocusArea::Library;
    }
    else if (s_menu.focus == FocusArea::Portal)
    {
        const auto rows=BuildPortalRows();
        const int count=static_cast<int>(std::count_if(rows.begin(),rows.end(),[](const PortalRow& row){return row.actualSlot>=0;}));
        const int old = s_menu.selectedPortalRow;
        s_menu.selectedPortalRow = std::clamp(old + (next ? 1 : 0) - (previous ? 1 : 0), 0, std::max(0, count - 1));
        if (old != s_menu.selectedPortalRow) PulseHaptics();
        if (up) s_menu.focus = FocusArea::Library;
    }
    else
    {
        if (previous) HubSelect(s_menu.selectedLibrary - 1);
        if (next) HubSelect(s_menu.selectedLibrary + 1);
        if (up) s_menu.focus=FocusArea::Header;
        if (down)
        {
            const auto rows=BuildPortalRows();
            if(std::any_of(rows.begin(),rows.end(),[](const PortalRow& row){return row.actualSlot>=0;}))
                s_menu.focus=FocusArea::Portal;
        }
    }
    if (oldFocus != s_menu.focus) PulseHaptics();
}

void HubFrame(ImDrawList* draw, const RiftLayout& layout, float x, float y, float w, float h, ImU32 color, float opacity)
{
    const auto lo = Point(layout, x, y), hi = Point(layout, x + w, y + h);
    draw->AddRectFilled(lo, hi, WithAlpha(CurrentTheme().surface, layout.alpha * 0.18f), CornerRadius(6) * layout.scale);
    draw->AddRect(lo, hi, WithAlpha(color, layout.alpha * opacity * 0.55f), CornerRadius(6) * layout.scale, 0, layout.scale);
    draw->AddLine(Point(layout, x + 14, y), Point(layout, x + std::min(w - 14, 78.0f), y),
        WithAlpha(color, layout.alpha * opacity), 2.0f * layout.scale);
}

void DrawHubShowcase(ImDrawList* draw, const RiftLayout& layout)
{
    if (s_menu.library.empty()) return;
    auto& config = GetConfig().emulated_usb_devices;
    const auto theme = CurrentTheme();
    const float spacing = std::clamp<int>(config.skylander_hub_spacing.GetValue(), 75, 125) * (config.skylander_hub_layout.GetValue() == 0 ? 1.08f : 1.32f);
    const float size = std::clamp<int>(config.skylander_hub_card_size.GetValue(), 75, 115) / 100.0f;
    struct Entry { int index; float offset; float travel; };
    std::vector<Entry> visible;
    const float reach=std::clamp<int>(config.skylander_card_density.GetValue(),5,11)*0.5f;
    for (int i = 0; i < static_cast<int>(s_menu.library.size()); ++i)
    {
        const auto key = FavoriteKey(s_menu.library[i].filePath);
        const float target = static_cast<float>(i - s_menu.selectedLibrary);
        auto [it, inserted] = s_cardCarouselOffsets.try_emplace(key, target);
        const float oldOffset=it->second;
        it->second = MotionEnabled() ? SmoothTowards(it->second, target, s_hubWorkspace.response) : target;
        const float exposure=std::min(1.0f,0.008f/std::max(0.001f,ImGui::GetIO().DeltaTime));
        if (std::abs(it->second) < reach) visible.push_back({i, it->second,(oldOffset-it->second)*exposure});
    }
    std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return std::abs(a.offset) > std::abs(b.offset); });
    const auto lo = Point(layout, 344, 145), hi = Point(layout, 1248, 486);
    draw->PushClipRect(lo, hi, true);
    int clicked = -1;
    for (const auto& entry : visible)
    {
        const auto& figure = s_menu.library[entry.index];
        float& focus = s_cardFocusAmounts[FavoriteKey(figure.filePath)];
        focus = AnimateFocus(focus, entry.index == s_menu.selectedLibrary && s_menu.focus == FocusArea::Library ? 1 : 0);
        auto art = GetArtworkTexture(figure.imagePath); art.accent = ElementColor(figure.element, theme.accent);
        const float distance = std::abs(entry.offset);
        const int style = s_hubWorkspace.animation;
        const float h = (224 - std::min(distance, 3.0f)*(style == 2 ? 15.0f : 27.0f)) * size;
        const float x = style == 1 ? 794 + std::sin(entry.offset*0.35f)*spacing*2.8f :
            style == 2 ? 794+entry.offset*spacing*0.40f : style == 3 ? 794 : 794+entry.offset*spacing;
        const float y = style == 1 ? 275+(1-std::cos(entry.offset*0.35f))*40 :
            style == 2 ? 283+std::min(distance,3.0f)*10 : 283+std::min(distance,3.0f)*18;
        const float motionFade = style == 3 ? std::pow(Clamp01(1-distance), 2.0f) : SmoothStep(Clamp01(reach-distance));
        const float edgeFade = SmoothStep(Clamp01(std::min(x-344.0f,1248.0f-x)/92.0f));
        const float fade = motionFade*edgeFade;
        if(fade < 0.005f) continue;
        DrawGlow(draw, Point(layout, x, y+55), h*0.5f*layout.scale, art.accent, layout.alpha*fade*(0.06f+0.16f*focus));
        const float dx=style==3?0:style==2?spacing*0.4f:style==1?std::cos(entry.offset*0.35f)*spacing*0.98f:spacing;
        DrawHubCard(draw, art, Point(layout, x, y-focus*5), h*layout.scale, focus, layout.alpha*fade,{entry.travel*dx*layout.scale,0});
        const float w = h * (art.id && art.size.y > 0 ? art.size.x / art.size.y : 0.64f);
        if (ImGui::IsMouseHoveringRect(lo, hi) && ClickedRect(layout, Point(layout, x-w*0.5f, y-h*0.5f), Point(layout, x+w*0.5f, y+h*0.5f))) clicked = entry.index;
    }
    draw->PopClipRect();
    if (clicked >= 0) { s_menu.focus=FocusArea::Library; HubSelect(clicked); if(ImGui::IsMouseDoubleClicked(0)) BeginPlacement(s_menu.library[clicked]); }
    if (ImGui::IsMouseHoveringRect(lo, hi) && ImGui::GetIO().MouseWheel != 0)
    { s_menu.focus=FocusArea::Library; HubSelect(s_menu.selectedLibrary + (ImGui::GetIO().MouseWheel < 0 ? 1 : -1)); }
}


void DrawHubHeader(ImDrawList* draw, const RiftLayout& layout)
{
    const auto theme = CurrentTheme();
    auto text = [&](float x, float y, float size, ImU32 color, std::string_view value) {
        DrawText(draw, Point(layout, x, y), WithAlpha(color, layout.alpha), size * layout.scale, value);
    };
    const float time = static_cast<float>(ImGui::GetTime());
    const float pulse = MotionEnabled() ? 0.88f + std::sin(time * 2.15f) * 0.12f : 1.0f;
    const ImVec2 markCenter = Point(layout, 94, 55);
    DrawGlow(draw, markCenter, 91 * layout.scale, theme.accent, layout.alpha * (0.38f + 0.13f * pulse + 0.32f * s_menu.riftCelebration));
    DrawGlow(draw, markCenter, 48 * layout.scale, theme.secondary, layout.alpha * (0.22f + 0.20f * s_menu.riftCelebration));
    draw->AddLine(Point(layout, 41, 76), Point(layout, 151, 76), WithAlpha(theme.border, layout.alpha * 0.48f), layout.scale);
    draw->AddLine(Point(layout, 41, 76), Point(layout, 87 + 48 * pulse, 76), WithAlpha(theme.accent, layout.alpha * 0.92f), 2.2f * layout.scale);
    text(44, 32, 38, WithAlpha(theme.accent, 0.42f), "Rift");
    text(42, 30, 38, theme.text, "Rift");
    if (MotionEnabled())
    {
        const float passive = static_cast<float>(std::fmod(time * 0.20f, 1.0));
        const float sweep = s_menu.riftGlint < 1.0f ? SmoothStep(s_menu.riftGlint) : passive;
        const float glintX = 20.0f + sweep * 158.0f;
        draw->PushClipRect(Point(layout, glintX, 24), Point(layout, glintX + 17, 77), true);
        text(42, 30, 38, BlendColor(theme.text, IM_COL32_WHITE, 0.82f), "Rift");
        draw->PopClipRect();
        DrawGlow(draw, Point(layout, glintX + 8, 52), 25 * layout.scale, theme.text,
            layout.alpha * (s_menu.riftGlint < 1.0f ? 0.72f : 0.22f));
    }
    if (s_menu.riftCelebration > 0.0f)
    {
        const float progress = 1.0f - s_menu.riftCelebration;
        for (int i = 0; i < 26; ++i)
        {
            const float angle = i * 2.399963f + progress * (0.35f + (i % 4) * 0.08f);
            const float distance = (20.0f + progress * (62.0f + (i % 7) * 8.0f)) * layout.scale;
            const float trail = (7.0f + (i % 4) * 2.5f) * layout.scale;
            const ImVec2 point{markCenter.x + std::cos(angle) * distance, markCenter.y + std::sin(angle) * distance * 0.62f};
            const ImVec2 tail{point.x - std::cos(angle) * trail, point.y - std::sin(angle) * trail * 0.62f};
            const ImU32 color = i % 3 == 0 ? theme.secondary : s_menu.placementAccent;
            draw->AddLine(tail, point, WithAlpha(color, layout.alpha * s_menu.riftCelebration * 0.82f),
                (0.8f + (i % 3) * 0.35f) * layout.scale);
            if (i % 5 == 0)
                DrawGlow(draw, point, 13 * layout.scale, color, layout.alpha * s_menu.riftCelebration * 0.48f);
        }
    }

    const bool hybridEnabled = GetConfig().emulated_usb_devices.skylander_hybrid_mode_enabled.GetValue();
    const int tabCount = HeaderItemCount();
    const float railX = hybridEnabled ? 484.0f : 626.0f;
    const float railWidth = 1248.0f - railX;
    const float tabWidth = railWidth / tabCount;
    const float railRight = railX + railWidth;
    draw->AddRectFilled(Point(layout, railX, 34), Point(layout, railRight, 82),
        WithAlpha(theme.surface, layout.alpha * 0.56f), CornerRadius(7) * layout.scale);
    draw->AddRect(Point(layout, railX, 34), Point(layout, railRight, 82),
        WithAlpha(theme.border, layout.alpha * 0.42f), CornerRadius(7) * layout.scale, 0, layout.scale);
    for (int i = 0; i < tabCount; ++i)
    {
        const float x = railX + i * tabWidth;
        float& focus = s_menu.headerFocusAnimations[i];
        focus = AnimateFocus(focus, s_menu.focus == FocusArea::Header && s_menu.headerSelection == i ? 1.0f : 0.0f);
        const bool portalTab = i < (hybridEnabled ? 3 : 2);
        const int forgeIndex = hybridEnabled ? 3 : 2;
        const int optionsIndex = hybridEnabled ? 4 : 3;
        const bool active = (portalTab && i == PortalMode()) || (i == forgeIndex && s_menu.page == RiftPage::Forge) ||
            (i == optionsIndex && s_menu.page == RiftPage::Options);
        const std::string_view label = i == 0 ? "VIRTUAL" : i == 1 ? "PHYSICAL" :
            hybridEnabled && i == 2 ? "HYBRID" : i == forgeIndex ? "CREATE .SKY" : "SETTINGS";
        if (i > 0)
            draw->AddLine(Point(layout, x, 43), Point(layout, x, 73), WithAlpha(theme.border, layout.alpha * 0.24f), layout.scale);
        draw->AddRectFilled(Point(layout, x + 3, 37), Point(layout, x + tabWidth - 3, 78),
            WithAlpha(BlendColor(theme.surface, theme.selectedSurface, active ? 0.88f : focus * 0.7f),
                layout.alpha * (active ? 0.72f : focus * 0.54f)), CornerRadius(5) * layout.scale);
        if (active || focus > 0.02f)
            DrawGlow(draw, Point(layout, x + tabWidth * 0.5f, 79), 41 * layout.scale, theme.accent,
                layout.alpha * (active ? 0.38f : focus * 0.25f));
        DrawTextCentered(draw, Point(layout, x + tabWidth * 0.5f, 58),
            WithAlpha(BlendColor(active ? theme.accent : theme.muted, theme.text, focus), layout.alpha),
            10.5f * layout.scale, label);
        draw->AddLine(Point(layout, x + 11, 80), Point(layout, x + tabWidth - 11, 80),
            WithAlpha(theme.accent, layout.alpha * (active ? 0.94f : focus * 0.72f)),
            (active ? 2.7f : 1.4f) * layout.scale);
        if (ClickedRect(layout, Point(layout, x, 34), Point(layout, x + tabWidth, 82)))
        { s_menu.headerSelection = i; ActivateHeaderSelection(); }
    }
}

void DrawHubHero(ImDrawList* draw, const RiftLayout& parent)
{
    RiftLayout layout = parent;
    const auto theme = CurrentTheme();
    auto text = [&](float x, float y, float size, ImU32 color, std::string_view value) {
        DrawText(draw, Point(layout, x, y), WithAlpha(color, layout.alpha), size * layout.scale, value);
    };
    HubFrame(draw, layout, 32, 108, 284, 515, theme.accent, 0.48f);
    text(54, 130, 10, theme.muted, "SELECTED FIGURE");
    if (!s_menu.library.empty())
    {
        const auto& target = s_menu.library[s_menu.selectedLibrary];
        auto& state = s_hubWorkspace;
        if (!state.hero) state.hero=target;
        if (state.hero->filePath != target.filePath)
        {
            state.heroVisibility = MotionEnabled() ? SmoothTowards(state.heroVisibility,0,24) : 0;
            if (state.heroVisibility < 0.025f) state.hero=target;
        }
        else state.heroVisibility = MotionEnabled() ? SmoothTowards(state.heroVisibility,1,16) : 1;
        layout.alpha *= SmoothStep(state.heroVisibility);
        const auto& figure = *state.hero;
        auto art = GetArtworkTexture(figure.imagePath);
        art.accent = ElementColor(figure.element, theme.accent);
        DrawGlow(draw, Point(layout, 174, 260), 116 * layout.scale, art.accent, layout.alpha * 0.13f);
        DrawHubCard(draw, art, Point(layout, 174, 252), 205 * layout.scale, 0.55f, layout.alpha);
        draw->AddRectFilled(Point(layout,45,375),Point(layout,303,600),WithAlpha(theme.surface,layout.alpha*.66f),CornerRadius(5)*layout.scale);
        DrawTextFit(draw, Point(layout, 58, 392), WithAlpha(theme.text, layout.alpha), 19 * layout.scale, 232 * layout.scale, figure.name);
        const bool onPortal=FindLoadedSlot(figure.id, figure.variant)>=0;
        draw->AddRectFilled(Point(layout,58,430),Point(layout,290,466),WithAlpha(art.accent,layout.alpha*.12f),CornerRadius(4)*layout.scale);
        text(72,442,10,art.accent,fmt::format("{}  /  {}",ElementName(figure.element),onPortal?"ON PORTAL":"IN COLLECTION"));
        draw->AddLine(Point(layout,58,488),Point(layout,290,488),WithAlpha(theme.border,layout.alpha*.35f),layout.scale);
        text(58,507,11,theme.text,"A  Place on portal");
        text(58,536,11,theme.text,"Y  Open figure details");
        text(58,572,9,theme.muted,"Stats, source data and backups live in Details.");
        s_menu.selectedCardCenter = Point(layout, 174, 252);
    }
    else
    {
        text(55, 255, 17, theme.text, "Your next adventure");
        text(55, 287, 11, theme.muted, "Create a .sky or change your filters.");
    }

}

void DrawHubCollection(ImDrawList* draw, const RiftLayout& layout)
{
    const auto theme = CurrentTheme();
    auto text = [&](float x, float y, float size, ImU32 color, std::string_view value) {
        DrawText(draw, Point(layout, x, y), WithAlpha(color, layout.alpha), size * layout.scale, value);
    };
    text(350, 109, 23, theme.text, "Collection");
    text(487, 116, 11, theme.muted, fmt::format("{} / {} FIGURES", s_menu.library.size(), s_menu.catalog.GetCollection().size()));
    ImGui::SetCursorScreenPos(Point(layout, 770, 104));
    ImGui::SetNextItemWidth(310 * layout.scale);
    ImFont* searchFont = ImGui_GetFont(12 * layout.scale);
    ImGui::PushFont(searchFont ? searchFont : ImGui::GetFont());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8*layout.scale,6*layout.scale});
    if (ImGui::InputTextWithHint("##hub-search", "Search your Skylanders...", s_menu.searchText.data(), s_menu.searchText.size()))
    { RebuildLibrary(); s_menu.hubScroll = 0; }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    text(1104, 115, 11, theme.accent, "FILTER / SORT");
    if (ClickedRect(layout, Point(layout, 1090, 100), Point(layout, 1240, 138))) OpenLibraryOrder();
    DrawHubShowcase(draw, layout);

}

void DrawHubPortal(ImDrawList* draw, const RiftLayout& layout)
{
    const auto theme = CurrentTheme();
    auto text = [&](float x, float y, float size, ImU32 color, std::string_view value) {
        DrawText(draw, Point(layout, x, y), WithAlpha(color, layout.alpha), size * layout.scale, value);
    };
    const auto rows = BuildPortalRows();
    const int activeCount=static_cast<int>(std::count_if(rows.begin(),rows.end(),[](const PortalRow& row){return row.actualSlot>=0;}));
    text(350, 497, 10, theme.accent, "PORTAL OF POWER");
    const int portalMode = PortalMode();
    const bool virtualPortal = portalMode != 1;
    text(490,497,10,theme.muted,fmt::format("{}  |  {} FIGURE{}",portalMode==0?"VIRTUAL":portalMode==1?"USB":"HYBRID (EXPERIMENTAL)",activeCount,activeCount==1?"":"S"));
    const float breathe=MotionLevel()==2?.5f+.5f*std::sin(static_cast<float>(ImGui::GetTime())*1.45f):.65f;
    draw->AddLine(Point(layout,438,603),Point(layout,1152,603),WithAlpha(theme.border,layout.alpha*.22f),layout.scale);
    draw->AddLine(Point(layout,590,608),Point(layout,1000,608),WithAlpha(theme.accent,layout.alpha*(.18f+breathe*.12f)),2*layout.scale);
    if(activeCount==0)
    {
        draw->AddRectFilled(Point(layout,360,535),Point(layout,1152,596),WithAlpha(theme.surface,layout.alpha*.72f),CornerRadius(5)*layout.scale);
        DrawTextCentered(draw,Point(layout,756,553),WithAlpha(theme.text,layout.alpha),15*layout.scale,
            virtualPortal?"Ready for a figure":"USB portal selected");
        DrawTextCentered(draw,Point(layout,756,579),WithAlpha(theme.muted,layout.alpha),10*layout.scale,
            virtualPortal?"Select a card and press A":"Live figure reading requires the hardware bridge");
        return;
    }
    const int count = std::min(7, activeCount);
    s_menu.selectedPortalRow = std::clamp(s_menu.selectedPortalRow, 0, std::max(0, activeCount-1));
    const int start = std::clamp(s_menu.selectedPortalRow-3, 0, std::max(0, activeCount-count));
    const float gap = 10.0f;
    const float targetSlotWidth = std::min(214.0f, (890.0f-gap*(count-1))/std::max(1,count));
    const float rowWidth = targetSlotWidth*count+gap*(count-1);
    const float rowStart = 350+(890-rowWidth)*0.5f;
    for (int j = 0; j < count; ++j)
    {
        const int index = start+j;
        const auto& row = rows[index];
        if(row.actualSlot<0) continue;
        const int key = row.actualSlot < 0 ? kPortalCapacity : row.actualSlot;
        float& slotWidth = s_menu.portalCardWidth[key];
        float& x = s_menu.portalCardX[key];
        const float targetX = rowStart+j*(targetSlotWidth+gap);
        if (slotWidth <= 0 || !MotionEnabled()) { slotWidth=targetSlotWidth; x=targetX; }
        else { slotWidth=SmoothTowards(slotWidth, targetSlotWidth, 12); x=SmoothTowards(x, targetX, 12); }
        float& focus = s_menu.portalFocusAnimations[index];
        focus = AnimateFocus(focus, s_menu.focus == FocusArea::Portal && index == s_menu.selectedPortalRow ? 1.0f : 0.0f);
        const ImU32 accent=row.definition?ElementColor(row.definition->element,theme.accent):theme.accent;
        if (row.actualSlot >= 0)
        {
            const float hover=MotionLevel()==2?std::sin(static_cast<float>(ImGui::GetTime())*1.8f+row.actualSlot)*1.5f:0;
            const auto center=Point(layout,x+(slotWidth-8)*0.5f,558+hover-focus*5);
            s_menu.portalTargets[row.actualSlot] = center;
            DrawGlow(draw,center,(44+focus*14)*layout.scale,accent,layout.alpha*(0.25f+focus*.28f));
            if(row.definition)
            {
                auto art=GetArtworkTexture(row.definition->imagePath); art.accent=accent;
                DrawHubCard(draw,art,center,(76+focus*10)*layout.scale,focus,layout.alpha);
            }
        }
        if(row.definition && focus>.45f)
            DrawTextCentered(draw,Point(layout,x+(slotWidth-8)*.5f,616),WithAlpha(theme.text,layout.alpha*focus),10*layout.scale,ShortName(row.definition->name,18));
        if(row.definition && ImGui::IsMouseHoveringRect(Point(layout,x,518),Point(layout,x+slotWidth-8,623))) ImGui::SetTooltip("%s",row.definition->name.c_str());
        if (ClickedRect(layout, Point(layout, x, 518), Point(layout, x+slotWidth-8, 628)))
        { s_menu.selectedPortalRow=index; s_menu.focus=FocusArea::Portal; if(row.actualSlot < 0) s_menu.focus=FocusArea::Library; else OpenPortalDetails(); PulseHaptics(); }
    }
}

void DrawRiftHub(ImDrawList* draw, const RiftLayout& layout)
{
    LoadHubWorkspace();
    DrawHubAtmosphere(draw, layout);
    DrawHubHeader(draw, HubPanelLayout(layout, 0));
    DrawHubHero(draw, HubPanelLayout(layout, 1));
    DrawHubCollection(draw, HubPanelLayout(layout, 2));
    DrawHubPortal(draw, HubPanelLayout(layout, 3));
}
