void HubPageHeading(ImDrawList* draw, const RiftLayout& layout, std::string_view title, std::string_view subtitle)
{
    LoadHubWorkspace();
    DrawHubAtmosphere(draw,layout);
    const auto theme=CurrentTheme();
    draw->AddRectFilled(Point(layout,32,24),Point(layout,1248,92),WithAlpha(theme.surface,layout.alpha*.56f),CornerRadius(6)*layout.scale);
    draw->AddLine(Point(layout,32,92),Point(layout,1248,92),WithAlpha(theme.border,layout.alpha*.55f),layout.scale);
    DrawText(draw,Point(layout,54,39),WithAlpha(theme.text,layout.alpha),23*layout.scale,title);
    DrawTextFit(draw,Point(layout,54,69),WithAlpha(theme.muted,layout.alpha),10*layout.scale,820*layout.scale,subtitle);
    draw->AddRectFilled(Point(layout,1080,39),Point(layout,1227,77),WithAlpha(theme.selectedSurface,layout.alpha*.72f),CornerRadius(4)*layout.scale);
    DrawTextCentered(draw,Point(layout,1153,58),WithAlpha(theme.accent,layout.alpha),11*layout.scale,"B  BACK TO HUB");
    if(ClickedRect(layout,Point(layout,1080,39),Point(layout,1227,77)))
    { s_menu.page=RiftPage::Collection; s_menu.focus=FocusArea::Library; PulseHaptics(UiSound::Back); }
}

void DrawHubDetails(ImDrawList* draw, const RiftLayout& layout)
{
	HubPageHeading(draw,layout,"Figure details","Overview, raw figure data and safe backups live together here.");
	const auto theme=CurrentTheme();
	const auto* definition=s_menu.catalog.Find(s_menu.details.id,s_menu.details.variant);
	auto art=GetArtworkTexture(s_menu.details.imagePath);
	if(definition) art.accent=ElementColor(definition->element,theme.accent);
	const std::array<std::string_view,3> tabs{"OVERVIEW","FIGURE DATA","BACKUP"};
	for(int tab=0;tab<3;++tab)
	{
		const float x=405+tab*205.0f;
		const bool selected=s_menu.detailsTab==tab;
		draw->AddRectFilled(Point(layout,x,112),Point(layout,x+190,153),
			WithAlpha(selected?theme.selectedSurface:theme.surface,layout.alpha*(selected?.82f:.42f)),CornerRadius(4)*layout.scale);
		if(selected) draw->AddLine(Point(layout,x+12,153),Point(layout,x+178,153),WithAlpha(art.accent,layout.alpha),2*layout.scale);
		DrawTextCentered(draw,Point(layout,x+95,132),WithAlpha(selected?theme.text:theme.muted,layout.alpha),11*layout.scale,tabs[tab]);
		if(ClickedRect(layout,Point(layout,x,112),Point(layout,x+190,153))) {s_menu.detailsTab=tab;PulseHaptics();}
	}
	HubFrame(draw,layout,45,175,330,420,art.accent,0.4f);
	DrawHubCard(draw,art,Point(layout,210,350),310*layout.scale,1,layout.alpha);
	DrawTextFit(draw,Point(layout,68,526),WithAlpha(theme.text,layout.alpha),20*layout.scale,284*layout.scale,s_menu.details.name);
	DrawText(draw,Point(layout,68,560),WithAlpha(art.accent,layout.alpha),10*layout.scale,
		definition?ElementName(definition->element):"UNKNOWN ELEMENT");
	const auto snapshot=ReadDetailSnapshot();
	const std::string source=snapshot?(snapshot->hasBackingFile?"LOCAL .SKY FILE":"LIVE PORTAL CACHE"):
		(s_menu.details.fromLibrary?"LOCAL FILE UNREADABLE":"PORTAL DATA UNAVAILABLE");
	draw->AddRectFilled(Point(layout,405,175),Point(layout,1218,595),WithAlpha(theme.surface,layout.alpha*.48f),CornerRadius(6)*layout.scale);
	if(s_menu.detailsTab==0)
	{
		DrawText(draw,Point(layout,434,204),WithAlpha(art.accent,layout.alpha),11*layout.scale,"CURRENT STATE");
		DrawTextFit(draw,Point(layout,432,239),WithAlpha(theme.text,layout.alpha),28*layout.scale,740*layout.scale,s_menu.details.name);
		const std::array<std::pair<std::string,std::string>,3> facts{{
			{"SOURCE",source},
			{"PORTAL",s_menu.details.actualSlot>=0?fmt::format("Active in slot {}",s_menu.details.actualSlot+1):"Not currently placed"},
			{"SAVE IMAGE",snapshot?"1 KB image readable":"No readable figure image"}}};
		for(int i=0;i<3;++i)
		{
			const float y=312+i*66.0f;
			DrawText(draw,Point(layout,434,y),WithAlpha(theme.muted,layout.alpha),10*layout.scale,facts[i].first);
			DrawTextFit(draw,Point(layout,610,y-3),WithAlpha(theme.text,layout.alpha),14*layout.scale,565*layout.scale,facts[i].second);
			draw->AddLine(Point(layout,434,y+32),Point(layout,1188,y+32),WithAlpha(theme.border,layout.alpha*.28f),layout.scale);
		}
	}
	else if(s_menu.detailsTab==1)
	{
		DrawText(draw,Point(layout,434,204),WithAlpha(art.accent,layout.alpha),11*layout.scale,"IDENTITY & GAMEPLAY DATA");
		const std::array<std::pair<std::string,std::string>,4> facts{{
			{"FIGURE ID",fmt::format("{} / 0x{:04X}",s_menu.details.id,s_menu.details.id)},
			{"VARIANT",fmt::format("0x{:04X}",s_menu.details.variant)},
			{"FIGURE UID",snapshot?FigureUid(*snapshot):"—"},
			{"RAW IMAGE",snapshot?"Captured and ready for backup":"Unavailable"}}};
		for(int i=0;i<4;++i)
		{
			const float y=248+i*49.0f;
			DrawText(draw,Point(layout,434,y),WithAlpha(theme.muted,layout.alpha),10*layout.scale,facts[i].first);
			DrawTextFit(draw,Point(layout,615,y-3),WithAlpha(theme.text,layout.alpha),14*layout.scale,560*layout.scale,facts[i].second);
			draw->AddLine(Point(layout,434,y+28),Point(layout,1188,y+28),WithAlpha(theme.border,layout.alpha*.28f),layout.scale);
		}
		draw->AddRectFilled(Point(layout,432,468),Point(layout,1190,565),WithAlpha(theme.selectedSurface,layout.alpha*.46f),CornerRadius(5)*layout.scale);
		DrawText(draw,Point(layout,452,488),WithAlpha(theme.accent,layout.alpha),10*layout.scale,"GAMEPLAY STATS — SAFE MODE");
		DrawText(draw,Point(layout,452,518),WithAlpha(theme.text,layout.alpha),13*layout.scale,"XP, currency, upgrades and nickname editing are locked.");
		DrawTextFit(draw,Point(layout,452,544),WithAlpha(theme.muted,layout.alpha),10*layout.scale,710*layout.scale,
			"They will unlock only after the checksums and per-game data layouts are decoded and verified.");
	}
	else
	{
		DrawText(draw,Point(layout,434,204),WithAlpha(art.accent,layout.alpha),11*layout.scale,"SAFE FIGURE BACKUP");
		DrawText(draw,Point(layout,434,247),WithAlpha(theme.text,layout.alpha),18*layout.scale,
			snapshot?"This figure is ready to back up.":"No readable figure data is available yet.");
		DrawText(draw,Point(layout,434,292),WithAlpha(theme.muted,layout.alpha),10*layout.scale,"SOURCE");
		DrawTextFit(draw,Point(layout,434,317),WithAlpha(theme.text,layout.alpha),14*layout.scale,735*layout.scale,source);
		DrawText(draw,Point(layout,434,365),WithAlpha(theme.muted,layout.alpha),10*layout.scale,"DESTINATION");
		const auto configured=GetConfig().emulated_usb_devices.skylander_collection_path.GetValue();
		DrawTextFit(draw,Point(layout,434,390),WithAlpha(theme.text,layout.alpha),13*layout.scale,735*layout.scale,
			configured.empty()?"Choose a collection folder in Cemu settings":configured+" / Backups");
		draw->AddRectFilled(Point(layout,434,470),Point(layout,1188,548),
			WithAlpha(snapshot?art.accent:theme.border,layout.alpha*(snapshot?.18f:.12f)),CornerRadius(5)*layout.scale);
		DrawTextCentered(draw,Point(layout,811,509),WithAlpha(snapshot?theme.text:theme.muted,layout.alpha),13*layout.scale,
			snapshot?"Y  CREATE TIMESTAMPED BACKUP":"WAITING FOR READABLE FIGURE DATA");
		if(snapshot && ClickedRect(layout,Point(layout,434,470),Point(layout,1188,548))) BackupDetailFigure();
	}
}

void DrawHubForge(ImDrawList* draw, const RiftLayout& layout)
{
    HubPageHeading(draw,layout,"Create a Skylander","Browse the verified catalog. Creating a figure leaves existing files intact.");
    const auto theme=CurrentTheme();
    const auto& figures=s_menu.catalog.GetCreatableDefinitions();
    if(figures.empty())
    { DrawText(draw,Point(layout,350,240),WithAlpha(theme.text,layout.alpha),18*layout.scale,"No valid artwork-backed figures found."); return; }
    s_menu.selectedDefinition=std::clamp(s_menu.selectedDefinition,0,static_cast<int>(figures.size())-1);
    const auto& selected=figures[s_menu.selectedDefinition];
    DrawTextFit(draw,Point(layout,50,147),WithAlpha(theme.text,layout.alpha),26*layout.scale,1160*layout.scale,selected.name);
    DrawText(draw,Point(layout,50,192),WithAlpha(ElementColor(selected.element,theme.accent),layout.alpha),12*layout.scale,
        fmt::format("{}    |    ID {}    /    VARIANT {:04X}",ElementName(selected.element),selected.id,selected.variant));
    static float position=0;
    position=MotionEnabled()?SmoothTowards(position,static_cast<float>(s_menu.selectedDefinition),s_hubWorkspace.response):static_cast<float>(s_menu.selectedDefinition);
    struct Entry{int index;float distance;}; std::vector<Entry> entries;
    const int center=static_cast<int>(std::round(position));
    for(int i=std::max(0,center-4);i<=std::min(static_cast<int>(figures.size())-1,center+4);++i) entries.push_back({i,i-position});
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return std::abs(a.distance)>std::abs(b.distance);});
    draw->PushClipRect(Point(layout,38,236),Point(layout,1245,567),true);
    int clicked=-1;
    for(const auto& entry:entries)
    {
        const float d=std::abs(entry.distance),focus=Clamp01(1-d);
        const float x=640+entry.distance*181,h=258-std::min(d,3.0f)*30,y=389+std::min(d,3.0f)*19;
        auto art=GetArtworkTexture(figures[entry.index].imagePath); art.accent=ElementColor(figures[entry.index].element,theme.accent);
        DrawHubCard(draw,art,Point(layout,x,y),h*layout.scale,focus,layout.alpha*Clamp01(4-d));
        if(ClickedRect(layout,Point(layout,x-h*0.30f,y-h*0.5f),Point(layout,x+h*0.30f,y+h*0.5f))) clicked=entry.index;
    }
    draw->PopClipRect();
    if(clicked>=0 && clicked!=s_menu.selectedDefinition) {s_menu.selectedDefinition=clicked;PulseHaptics();}
    if(ImGui::IsMouseHoveringRect(Point(layout,38,236),Point(layout,1245,567)) && ImGui::GetIO().MouseWheel!=0)
    {
        const int old=s_menu.selectedDefinition;
        s_menu.selectedDefinition=std::clamp(old+(ImGui::GetIO().MouseWheel<0?1:-1),0,static_cast<int>(figures.size())-1);
        if(old!=s_menu.selectedDefinition)PulseHaptics();
    }
    DrawTextCentered(draw,Point(layout,640,594),WithAlpha(theme.muted,layout.alpha),12*layout.scale,
        fmt::format("{} / {}    |    SCROLL OR USE THE D-PAD",s_menu.selectedDefinition+1,figures.size()));
}

void HubSettingRow(ImDrawList* draw,const RiftLayout& layout,float x,float y,float width,
    std::string_view name,std::string_view value,float& focus,bool selected)
{
    const auto theme=CurrentTheme();
    focus=AnimateFocus(focus,selected?1:0);
    draw->AddRectFilled(Point(layout,x-2,y+4+focus*3),Point(layout,x+width+2,y+48+focus*3),
        WithAlpha(IM_COL32_BLACK,layout.alpha*(0.10f+focus*0.13f)),CornerRadius(5)*layout.scale);
    draw->AddRectFilled(Point(layout,x,y),Point(layout,x+width,y+43),
        WithAlpha(BlendColor(theme.surface,theme.selectedSurface,focus),layout.alpha*(0.35f+0.5f*focus)),CornerRadius(4)*layout.scale);
    draw->AddLine(Point(layout,x,y+43),Point(layout,x+width,y+43),WithAlpha(BlendColor(theme.border,theme.accent,focus),layout.alpha*(0.25f+focus*0.6f)),layout.scale);
    DrawTextFit(draw,Point(layout,x+12,y+15),WithAlpha(theme.text,layout.alpha),11*layout.scale,(width-157)*layout.scale,name);
    DrawTextFit(draw,Point(layout,x+width-132,y+15),WithAlpha(BlendColor(theme.muted,theme.accent,focus),layout.alpha),11*layout.scale,120*layout.scale,value);
}

void HubCompactSettingRow(ImDrawList* draw,const RiftLayout& layout,float x,float y,float width,
    std::string_view name,std::string_view value,float& focus,bool selected)
{
    const auto theme=CurrentTheme();
    focus=AnimateFocus(focus,selected?1:0);
    draw->AddRectFilled(Point(layout,x,y),Point(layout,x+width,y+25),
        WithAlpha(BlendColor(theme.surface,theme.selectedSurface,focus),layout.alpha*(.30f+.55f*focus)),CornerRadius(3)*layout.scale);
    if(selected) draw->AddRectFilled(Point(layout,x,y),Point(layout,x+4,y+25),WithAlpha(theme.accent,layout.alpha),CornerRadius(2)*layout.scale);
    DrawTextFit(draw,Point(layout,x+14,y+7),WithAlpha(theme.text,layout.alpha),10*layout.scale,(width-260)*layout.scale,name);
    DrawTextFit(draw,Point(layout,x+width-220,y+7),WithAlpha(BlendColor(theme.muted,theme.accent,focus),layout.alpha),10*layout.scale,194*layout.scale,value);
}

void DrawHubOptions(ImDrawList* draw,const RiftLayout& layout)
{
    HubPageHeading(draw,layout,"Rift settings","Choose a preset first, then change only the few controls that matter.");
    const auto theme=CurrentTheme();
    DrawText(draw,Point(layout,196,126),WithAlpha(theme.accent,layout.alpha),11*layout.scale,"CONTROLLER-FRIENDLY PRESETS & FEEDBACK");
    const std::array<std::string_view,kOptionCount> names{"Experience preset","Motion","Portal particles","UI sounds",
        "Controller feedback","Feedback strength","Cards on screen","Color scheme","Experimental hybrid mode","Support Rift on Ko-fi"};
    for(int option=0;option<kOptionCount;++option)
    {
        const float x=196,y=140+option*45.0f;
        HubSettingRow(draw,layout,x,y,888,names[option],OptionValue(option),s_menu.optionFocusAnimations[option],option==s_menu.selectedOption);
        if(ClickedRect(layout,Point(layout,x,y),Point(layout,x+888,y+43)))
        { if(s_menu.selectedOption==option)AdjustOption(option,1);else{s_menu.selectedOption=option;PulseHaptics();} }
    }
    DrawTextCentered(draw,Point(layout,640,613),WithAlpha(theme.muted,layout.alpha),9*layout.scale,
        "UNOFFICIAL DEVELOPMENT BUILD | NOT AFFILIATED WITH CEMU, ACTIVISION, TOYS FOR BOB, OR NINTENDO");
}

void DrawDeleteConfirm(ImDrawList* draw,const RiftLayout& layout)
{
    HubPageHeading(draw,layout,"Delete collection file","This action requires confirmation and never targets files outside your collection.");
    const auto theme=CurrentTheme();
    draw->AddRectFilled(Point(layout,226,176),Point(layout,1054,555),WithAlpha(theme.surface,layout.alpha*.72f),CornerRadius(7)*layout.scale);
    DrawTextCentered(draw,Point(layout,640,235),WithAlpha(theme.accent,layout.alpha),13*layout.scale,"CONFIRM FILE DELETION");
    DrawTextFit(draw,Point(layout,300,285),WithAlpha(theme.text,layout.alpha),27*layout.scale,680*layout.scale,s_menu.details.name);
    DrawTextCentered(draw,Point(layout,640,355),WithAlpha(theme.text,layout.alpha),14*layout.scale,
        "The selected .sky file will leave your active collection.");
    DrawTextCentered(draw,Point(layout,640,391),WithAlpha(theme.muted,layout.alpha),12*layout.scale,
        "Rift moves it to the collection's .rift-trash folder so it can be recovered.");
    DrawTextCentered(draw,Point(layout,640,466),WithAlpha(theme.accent,layout.alpha),14*layout.scale,
        "A  CONFIRM DELETE        B  CANCEL");
}

void DrawHubOrder(ImDrawList* draw,const RiftLayout& layout)
{
    HubPageHeading(draw,layout,"Arrange your collection","Sort, pin and filter without changing any of your .sky files.");
    DrawHubHero(draw,layout);
    const std::array<std::string_view,5> names{"Sort by","Pin first","Element first","Show figure family","Element filter"};
    for(int i=0;i<5;++i)
    {
        const float y=173+i*78.0f;
        HubSettingRow(draw,layout,350,y,880,names[i],LibraryOptionValue(i),s_menu.libraryOptionFocusAnimations[i],i==s_menu.selectedLibraryOption);
        if(ClickedRect(layout,Point(layout,350,y),Point(layout,1230,y+43)))
        { if(i==s_menu.selectedLibraryOption)AdjustLibraryOption(i,1);else{s_menu.selectedLibraryOption=i;PulseHaptics();} }
    }
    DrawText(draw,Point(layout,360,583),WithAlpha(CurrentTheme().muted,layout.alpha),13*layout.scale,
        fmt::format("{} figures match your choices",s_menu.library.size()));
}
