struct HubWorkspace
{
    int animation{};
    float response{10};
    float motionBlur{0.25f};
    float vignette{0.25f};
    std::optional<skylander_ui::CollectionFigure> hero;
    float heroVisibility{1};
} s_hubWorkspace;

void LoadHubWorkspace()
{
}

RiftLayout HubPanelLayout(const RiftLayout& parent, int)
{
    return parent;
}

void DrawHubCard(ImDrawList* draw, const ArtworkTexture& art, ImVec2 center, float height, float focus, float alpha, ImVec2 shutter = {})
{
    if (alpha <= 0.005f || height <= 0) return;
    const auto theme = CurrentTheme();
    const float width = height*(art.id && art.size.y>0 ? art.size.x/art.size.y : 0.64f);
    const ImVec2 lo{center.x-width*0.5f, center.y-height*0.5f}, hi{center.x+width*0.5f, center.y+height*0.5f};
    const float corner = std::min(width*0.04f, CornerRadius(6));
    const float lift = 3.0f + focus * 5.0f;
    draw->AddRectFilled({lo.x-5,lo.y+lift+5}, {hi.x+5,hi.y+lift+11},
        WithAlpha(IM_COL32_BLACK, alpha*(0.07f+focus*.05f)), corner+7);
    draw->AddRectFilled({lo.x-2,lo.y+lift+2}, {hi.x+2,hi.y+lift+6},
        WithAlpha(IM_COL32_BLACK, alpha*(0.16f+focus*.10f)), corner+3);
    if (GetConfig().emulated_usb_devices.skylander_card_effect.GetValue() > 0)
        DrawGlow(draw, center, height*0.7f, art.accent, alpha*focus*0.5f);
    if (art.id)
    {
        const float blur = MotionLevel() == 2 ? s_hubWorkspace.motionBlur : 0;
        shutter.x = std::clamp(shutter.x*blur,-height*0.06f,height*0.06f);
        shutter.y = std::clamp(shutter.y*blur,-height*0.06f,height*0.06f);
        if (std::abs(shutter.x)+std::abs(shutter.y)>0.5f)
            for (int sample=8;sample>0;--sample)
            {
                const float t=sample/8.0f;
                draw->AddImageRounded(art.id,{lo.x+shutter.x*t,lo.y+shutter.y*t},
                    {hi.x+shutter.x*t,hi.y+shutter.y*t},art.uvMinimum,art.uvMaximum,
                    WithAlpha(IM_COL32_WHITE,alpha*0.055f*(1-t*0.6f)),corner);
            }
        draw->AddImageRounded(art.id, lo, hi, art.uvMinimum, art.uvMaximum, WithAlpha(IM_COL32_WHITE, alpha), corner);
    }
    else
    {
        draw->AddRectFilled(lo, hi, WithAlpha(theme.surface, alpha), corner);
        DrawTextCentered(draw, {center.x,center.y-6}, WithAlpha(theme.muted, alpha), 11, art.state==ArtworkTexture::State::Loading ? "LOADING" : "NO ARTWORK");
    }
    const int border = GetConfig().emulated_usb_devices.skylander_card_border.GetValue();
    const ImU32 edge = border == 0 ? theme.border : art.accent;
    draw->AddRect({lo.x-2,lo.y-2}, {hi.x+2,hi.y+2}, WithAlpha(edge, alpha*(0.18f+focus*0.67f)), corner+2, 0, 1+focus);
}

void DrawHubAtmosphere(ImDrawList* draw, const RiftLayout& layout)
{
    const auto theme = CurrentTheme();
    const float edgeAlpha=layout.alpha*s_hubWorkspace.vignette*0.55f;
    draw->AddRectFilledMultiColor(Point(layout,0,0),Point(layout,190,720),WithAlpha(IM_COL32_BLACK,edgeAlpha),0,0,WithAlpha(IM_COL32_BLACK,edgeAlpha));
    draw->AddRectFilledMultiColor(Point(layout,1090,0),Point(layout,1280,720),0,WithAlpha(IM_COL32_BLACK,edgeAlpha),WithAlpha(IM_COL32_BLACK,edgeAlpha),0);
    const int level = std::clamp<int>(GetConfig().emulated_usb_devices.skylander_particle_level.GetValue(), 0, 3);
    const int count = level*16;
    const double time = MotionEnabled() ? ImGui::GetTime() : 0;
    for(int i=0; i<count; ++i)
    {
        const float phase = static_cast<float>(std::fmod(time*(0.025+(i%5)*0.004)+i*0.6180339, 1.0));
        const float envelope = std::sin(phase*kPi);
        const float x = 35 + (i*173%1200) + std::sin(phase*2*kPi+i)*12;
        const float y = 700-phase*660;
        const auto position = Point(layout,x,y);
        const auto color = i%3 ? theme.accent : theme.secondary;
        if (i%4==0) DrawGlow(draw,position,12*layout.scale,color,layout.alpha*envelope*0.25f);
        draw->AddCircleFilled(position,(0.7f+(i%3)*0.35f)*layout.scale,WithAlpha(color,layout.alpha*envelope*0.32f),8);
    }
    if (s_menu.backgroundCelebration > 0.0f && level > 0)
    {
        const float progress = 1.0f - s_menu.backgroundCelebration;
        const ImVec2 origin = Point(layout, 756, 570);
        const int burstCount = 20 + level * 10;
        for (int i = 0; i < burstCount; ++i)
        {
            const float angle = i * 2.399963f + (i % 5) * 0.13f;
            const float distance = (34.0f + progress * (260.0f + (i % 11) * 28.0f)) * layout.scale;
            const float arc = std::sin(progress * kPi) * (28.0f + (i % 6) * 9.0f) * layout.scale;
            const ImVec2 point{origin.x + std::cos(angle) * distance,
                origin.y + std::sin(angle) * distance * 0.48f - arc};
            const float fade = s_menu.backgroundCelebration * SmoothStep(std::min(1.0f, progress * 5.0f));
            const ImU32 color = i % 4 == 0 ? theme.secondary : s_menu.placementAccent;
            if (i % 6 == 0)
                DrawGlow(draw, point, (13.0f + (i % 3) * 5.0f) * layout.scale, color, layout.alpha * fade * 0.36f);
            draw->AddCircleFilled(point, (0.9f + (i % 4) * 0.38f) * layout.scale,
                WithAlpha(color, layout.alpha * fade * 0.72f), 10);
        }
    }
}
