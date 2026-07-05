#include "DragDropPanel.hpp"
#include "Widgets/Label.hpp"
#include <slic3r/GUI/wxExtensions.hpp>
#include "GUI_App.hpp"

namespace Slic3r { namespace GUI {

// H2C: Event definition for drag-drop completion notification.
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – wxDEFINE_EVENT
wxDEFINE_EVENT(wxEVT_DRAG_DROP_COMPLETED, wxCommandEvent);

struct CustomData
{
    int filament_id;
    unsigned char r, g, b, a;
    char type[64];
};


wxColor Hex2Color(const std::string& str)
{
    if (str.empty() || (str.length() != 9 && str.length() != 7) || str[0] != '#')
        throw std::invalid_argument("Invalid hex color format");

    auto hexToByte = [](const std::string& hex)->unsigned char
        {
            unsigned int byte;
            std::istringstream(hex) >> std::hex >> byte;
            return static_cast<unsigned char>(byte);
        };
    auto r = hexToByte(str.substr(1, 2));
    auto g = hexToByte(str.substr(3, 2));
    auto b = hexToByte(str.substr(5, 2));
    unsigned char a = 255;
    if (str.size() == 9)
        a = hexToByte(str.substr(7, 2));
    return wxColor(r, g, b, a);
}

// Custom data object used to store information that needs to be backed up during drag and drop
class ColorDataObject : public wxCustomDataObject
{
public:
    ColorDataObject(const wxColour &color = *wxBLACK, int filament_id = 0, const std::string &type = "PLA")
        : wxCustomDataObject(wxDataFormat("application/customize_format"))
    {
        set_custom_data_filament_id(filament_id);
        set_custom_data_color(color);
        set_custom_data_type(type);
    }

    wxColour GetColor() const { return wxColor(m_data.r, m_data.g, m_data.b, m_data.a); }
    void     SetColor(const wxColour &color) { set_custom_data_color(color); }

    int      GetFilament() const { return m_data.filament_id; }
    void     SetFilament(int label) { set_custom_data_filament_id(label); }

    std::string     GetType() const { return m_data.type; }
    void     SetType(const std::string &type) { set_custom_data_type(type); }

    void set_custom_data_type(const std::string& type) {
        std::strncpy(m_data.type, type.c_str(), sizeof(m_data.type) - 1);
        m_data.type[sizeof(m_data.type) - 1] = '\0';
    }

    void set_custom_data_filament_id(int filament_id) {
        m_data.filament_id = filament_id;
    }

    void set_custom_data_color(const wxColor& color) {
        m_data.r           = color.Red();
        m_data.g           = color.Green();
        m_data.b           = color.Blue();
        m_data.a           = color.Alpha();
    }

    virtual size_t GetDataSize() const override { return sizeof(m_data); }
    virtual bool   GetDataHere(void *buf) const override
    {
        std::memcpy(buf, &m_data, sizeof(m_data));
        return true;
    }
    virtual bool SetData(size_t len, const void *buf) override
    {
        if (len == GetDataSize()) {
            std::memcpy(&m_data, buf, sizeof(m_data));
            return true;
        }
        return false;
    }
private:
    CustomData m_data;
};

///////////////   ColorPanel  start ////////////////////////
// The UI panel of drag item
ColorPanel::ColorPanel(DragDropPanel *parent, const wxColour &color, int filament_id, const std::string &type)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(32, 40), wxBORDER_NONE), m_parent(parent), m_color(color), m_filament_id(filament_id), m_type(type)
{
    // H2C: Inherit parent background for dark mode support.
    SetBackgroundColour(parent->GetBackgroundColour());
    Bind(wxEVT_LEFT_DOWN, &ColorPanel::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &ColorPanel::OnLeftUp, this);
    Bind(wxEVT_PAINT, &ColorPanel::OnPaint, this);
}

void ColorPanel::OnLeftDown(wxMouseEvent &event)
{
    m_parent->set_is_draging(true);
    m_parent->DoDragDrop(this, GetColor(), GetType(), GetFilamentId());
}

void ColorPanel::OnLeftUp(wxMouseEvent &event) { m_parent->set_is_draging(false); }

void ColorPanel::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    wxSize   size  = GetSize();
    int svg_size = size.GetWidth();
    int type_label_height = FromDIP(10);
    wxString type_label(m_type);
    int type_label_margin = FromDIP(3);

    std::string replace_color = m_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::string svg_name = "outlined_rect";
    if (replace_color == "#FFFFFF00") {
        svg_name = "outlined_rect_transparent";
    }
    static Slic3r::GUI::BitmapCache cache;
    wxBitmap* bmp = cache.load_svg(svg_name, 0, svg_size, false, false, replace_color, 0.f);
    dc.DrawBitmap(*bmp, wxPoint(0,0));

    wxString label = wxString::Format(wxT("%d"), m_filament_id);
    dc.SetTextForeground(m_color.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    dc.DrawLabel(label, wxRect(0, 0, svg_size, svg_size), wxALIGN_CENTER);

    if(m_parent)
        dc.SetTextForeground(this->m_parent->GetBackgroundColour().GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    else
        dc.SetTextForeground(*wxBLACK);
    if (type_label.length() > 4) {
        wxString first = type_label.Mid(0, 4);
        wxString rest = type_label.Mid(4);
        dc.DrawLabel(first, wxRect(0, svg_size + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
        dc.DrawLabel(rest, wxRect(0, svg_size + type_label_height + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
    }else {
        dc.DrawLabel(type_label, wxRect(0, svg_size + type_label_margin, svg_size, type_label_height), wxALIGN_CENTER);
    }
}
///////////////   ColorPanel  end ////////////////////////


// Save the source object information to m_data when dragging
class ColorDropSource : public wxDropSource
{
public:
    ColorDropSource(wxPanel *parent, wxPanel *color_block, const wxColour &color, const std::string& type, int filament_id) : wxDropSource(parent)
    {
        m_data.SetColor(color);
        m_data.SetFilament(filament_id);
        m_data.SetType(type);
        SetData(m_data);
    }

private:
    ColorDataObject m_data;
};

///////////////   ColorDropTarget  start ////////////////////////
class ColorDropTarget : public wxDropTarget
{
public:
    ColorDropTarget(DragDropPanel *panel) : wxDropTarget(), m_panel(panel)
    {
        m_data = new ColorDataObject();
        SetDataObject(m_data);
    }

    virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override;
    virtual bool         OnDrop(wxCoord x, wxCoord y) override {
        return true;
    }

private:
    DragDropPanel * m_panel;
    ColorDataObject* m_data;
};

wxDragResult ColorDropTarget::OnData(wxCoord x, wxCoord y, wxDragResult def)
{
    if (!GetData())
        return wxDragNone;

    m_panel->AddColorBlock(m_data->GetColor(), m_data->GetType(), m_data->GetFilament());

    return wxDragCopy;
}
///////////////   ColorDropTarget  end ////////////////////////


// H2C: Updated DragDropPanel constructor with has_title and is_sub support.
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – DragDropPanel ctor
DragDropPanel::DragDropPanel(wxWindow *parent, const wxString &label, bool is_auto, bool has_title, bool is_sub)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_is_auto(is_auto)
{
    SetBackgroundColour(0xF8F8F8);

    m_sizer    = new wxBoxSizer(wxVERTICAL);

    if (has_title) {
        auto title_panel = new wxPanel(this);
        // H2C: Sub-panels use lighter background and smaller font.
        // Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – is_sub styling
        title_panel->SetBackgroundColour(is_sub ? 0xF8F8F8 : 0xEEEEEE);
        auto title_sizer = new wxBoxSizer(wxHORIZONTAL);
        title_panel->SetSizer(title_sizer);

        m_title_label = new Label(this, label);
        m_title_label->SetFont(is_sub ? Label::Body_12 : Label::Head_13);
        m_title_label->SetForegroundColour(is_sub ? wxColour(0x6B6B6B) : wxColour(0x000000));
        m_title_label->SetBackgroundColour(is_sub ? 0xF8F8F8 : 0xEEEEEE);

        title_sizer->Add(m_title_label, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

        m_sizer->Add(title_panel, 0, wxEXPAND);
        m_sizer->AddSpacer(10);
    }

    // H2C: Sub-panels use 3 columns with tighter spacing to fit narrower width.
    // Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – is_sub grid layout
    if (is_sub) {
        m_grid_item_sizer = new wxGridSizer(0, 3, FromDIP(6), FromDIP(6));
        m_sizer->Add(m_grid_item_sizer, 0, wxEXPAND);
    } else {
        m_grid_item_sizer = new wxGridSizer(0, 6, FromDIP(8), FromDIP(8));
        m_sizer->Add(m_grid_item_sizer, 1, wxEXPAND | wxALL, FromDIP(8));
    }

    auto drop_target = new ColorDropTarget(this);
    SetDropTarget(drop_target);

    SetSizer(m_sizer);
    Layout();
    Fit();
    wxGetApp().UpdateDarkUIWin(this);
}

void DragDropPanel::AddColorBlock(const wxColour &color, const std::string &type, int filament_id, bool update_ui)
{
    ColorPanel *panel = new ColorPanel(this, color, filament_id, type);
    panel->SetMinSize(wxSize(FromDIP(30), FromDIP(60)));
    m_grid_item_sizer->Add(panel, 0);
    m_filament_blocks.push_back(panel);
    if (update_ui) {
        m_filament_blocks.front()->Refresh();
        GetParent()->GetParent()->Layout();
        GetParent()->GetParent()->Fit();
    }
    NotifyDragDropCompleted();
}

void DragDropPanel::RemoveColorBlock(ColorPanel *panel, bool update_ui)
{
    m_sizer->Detach(panel);
    panel->Destroy();
    m_filament_blocks.erase(std::remove(m_filament_blocks.begin(), m_filament_blocks.end(), panel), m_filament_blocks.end());
    if (update_ui) {
        GetParent()->GetParent()->Layout();
        GetParent()->GetParent()->Fit();
    }
}

void DragDropPanel::DoDragDrop(ColorPanel *panel, const wxColour &color, const std::string &type, int filament_id)
{
    if (m_is_auto)
        return;

    ColorDropSource source(this, panel, color, type, filament_id);
    if (source.DoDragDrop(wxDrag_CopyOnly) == wxDragResult::wxDragCopy) {
        this->RemoveColorBlock(panel);
    }
}

// H2C: Update the title label text.
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – UpdateLabel
void DragDropPanel::UpdateLabel(const wxString &label)
{
    if (m_title_label) {
        m_title_label->SetLabel(label);
        m_title_label->Refresh();
        Layout();
    }
}

std::vector<int> DragDropPanel::GetAllFilaments() const
{
    std::vector<int>          filaments;
    for (size_t i = 0; i < m_grid_item_sizer->GetItemCount(); ++i) {
        wxSizerItem *item = m_grid_item_sizer->GetItem(i);
        if (item != nullptr) {
            wxWindow *  window     = item->GetWindow();
            ColorPanel *colorPanel = dynamic_cast<ColorPanel *>(window);
            if (colorPanel != nullptr) {
                filaments.push_back(colorPanel->GetFilamentId());
            }
        }
    }

    return filaments;
}

// H2C: Notify parent that a drag-drop completed (filament assignment changed).
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – NotifyDragDropCompleted
void DragDropPanel::NotifyDragDropCompleted()
{
    wxCommandEvent event(wxEVT_DRAG_DROP_COMPLETED);
    event.SetEventObject(this);
    wxPostEvent(GetParent(), event);
}


///////////////   SeparatedDragDropPanel  start ////////////////////////
// H2C: Drop target for SeparatedDragDropPanel — routes drops to HF or Standard sub-panel.
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – SeparatedColorDropTarget
class SeparatedColorDropTarget : public wxDropTarget
{
public:
    SeparatedColorDropTarget(SeparatedDragDropPanel *panel) : wxDropTarget(), m_panel(panel)
    {
        m_data = new ColorDataObject();
        SetDataObject(m_data);
    }

    virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override;
    virtual bool         OnDrop(wxCoord x, wxCoord y) override { return true; }

private:
    SeparatedDragDropPanel *m_panel;
    ColorDataObject        *m_data;
};

wxDragResult SeparatedColorDropTarget::OnData(wxCoord x, wxCoord y, wxDragResult def)
{
    if (!GetData()) return wxDragNone;

    m_panel->AddColorBlock(m_data->GetColor(), m_data->GetType(), m_data->GetFilament(), false);
    return wxDragCopy;
}

// H2C: SeparatedDragDropPanel implementation — Hybrid HF/Standard split.
// Reference to BBS: BambuStudio/src/slic3r/GUI/DragDropPanel.cpp – SeparatedDragDropPanel
SeparatedDragDropPanel::SeparatedDragDropPanel(wxWindow *parent, const wxString &label, bool use_separation)
    : wxPanel(parent), m_use_separation(use_separation)
{
    SetBackgroundColour(0xF8F8F8);

    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    auto title_panel = new wxPanel(this);
    title_panel->SetBackgroundColour(0xEEEEEE);
    auto title_sizer = new wxBoxSizer(wxHORIZONTAL);
    title_panel->SetSizer(title_sizer);

    m_label = new Label(title_panel, label);
    m_label->SetFont(Label::Head_13);
    m_label->SetBackgroundColour(0xEEEEEE);

    title_sizer->Add(m_label, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    m_main_sizer->Add(title_panel, 0, wxEXPAND);
    m_main_sizer->AddSpacer(10);

    m_content_panel = new wxPanel(this);
    m_content_panel->SetBackgroundColour(0xF8F8F8);
    m_content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_content_panel->SetSizer(m_content_sizer);

    m_high_flow_panel = new DragDropPanel(m_content_panel, _L("High Flow"), false, true, true);
    m_standard_panel  = new DragDropPanel(m_content_panel, _L("Standard"), false, true, true);
    m_unified_panel   = new DragDropPanel(m_content_panel, wxEmptyString, false, false);

    m_high_flow_panel->SetBackgroundColour(0xF8F8F8);
    m_standard_panel->SetBackgroundColour(0xF8F8F8);
    m_unified_panel->SetBackgroundColour(0xF8F8F8);
    m_unified_panel->SetMinSize({FromDIP(260), -1});

    m_main_sizer->Add(m_content_panel, 1, wxEXPAND);

    UpdateLayout();

    auto drop_target = new SeparatedColorDropTarget(this);
    SetDropTarget(drop_target);

    SetSizer(m_main_sizer);
    Layout();
    wxGetApp().UpdateDarkUIWin(this);
}

void SeparatedDragDropPanel::UpdateLayout()
{
    m_content_sizer->Clear(false);

    if (m_use_separation) {
        m_unified_panel->Hide();
        m_high_flow_panel->Show();
        m_standard_panel->Show();

        wxSize content_size = m_content_panel->GetSize();
        int panel_width = (content_size.GetWidth() - FromDIP(1) - FromDIP(8)) / 2;
        if (panel_width > 0) {
            m_high_flow_panel->SetMinSize(wxSize(panel_width, -1));
            m_standard_panel->SetMinSize(wxSize(panel_width, -1));
        }

        m_content_sizer->Add(m_high_flow_panel, 1, wxEXPAND | wxLEFT, FromDIP(8));

        auto separator = new wxPanel(m_content_panel, wxID_ANY);
        separator->SetBackgroundColour(0xCCCCCC);
        separator->SetMinSize(wxSize(FromDIP(1), -1));
        m_content_sizer->Add(separator, 0, wxEXPAND | wxALL, FromDIP(4));

        m_content_sizer->Add(m_standard_panel, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    } else {
        m_high_flow_panel->Hide();
        m_standard_panel->Hide();
        m_unified_panel->Show();

        m_content_sizer->Add(m_unified_panel, 1, wxEXPAND);
    }
    m_content_sizer->Layout();
    Layout();
    Fit();
    if (GetParent()) {
        GetParent()->Layout();
        if (GetParent()->GetParent()) {
            GetParent()->GetParent()->Layout();
        }
    }
}

void SeparatedDragDropPanel::UpdateLabel(const wxString &label)
{
    if (m_label) {
        m_label->SetLabel(label);
        m_label->Refresh();
        Layout();
    }
}

void SeparatedDragDropPanel::SetUseSeparation(bool use_separation)
{
    if (m_use_separation != use_separation) {
        m_use_separation = use_separation;

        if (use_separation) {
            // Moving from unified → separated: all blocks go to Standard by default
            auto blocks = m_unified_panel->get_filament_blocks();
            for (auto &block : blocks) {
                m_standard_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
                m_unified_panel->RemoveColorBlock(block, false);
            }
        } else {
            // Moving from separated → unified: merge all blocks
            auto high_flow_blocks = m_high_flow_panel->get_filament_blocks();
            auto standard_blocks  = m_standard_panel->get_filament_blocks();

            for (auto &block : high_flow_blocks) {
                m_unified_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
                m_high_flow_panel->RemoveColorBlock(block, false);
            }

            for (auto &block : standard_blocks) {
                m_unified_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
                m_standard_panel->RemoveColorBlock(block, false);
            }
        }

        UpdateLayout();
    }
}

void SeparatedDragDropPanel::AddColorBlock(const wxColour &color, const std::string &type, int filament_id, bool is_high_flow, bool update_ui)
{
    if (m_use_separation) {
        if (is_high_flow) {
            m_high_flow_panel->AddColorBlock(color, type, filament_id, update_ui);
        } else {
            m_standard_panel->AddColorBlock(color, type, filament_id, update_ui);
        }

        if (update_ui) {
            CallAfter([this]() {
                Layout();
                GetParent()->Layout();
            });
        }
    } else {
        m_unified_panel->AddColorBlock(color, type, filament_id, update_ui);
    }
}

void SeparatedDragDropPanel::RemoveColorBlock(ColorPanel *panel, bool update_ui)
{
    auto high_flow_blocks = m_high_flow_panel->get_filament_blocks();
    auto standard_blocks  = m_standard_panel->get_filament_blocks();
    auto unified_blocks   = m_unified_panel->get_filament_blocks();

    if (std::find(high_flow_blocks.begin(), high_flow_blocks.end(), panel) != high_flow_blocks.end()) {
        m_high_flow_panel->RemoveColorBlock(panel, update_ui);
    } else if (std::find(standard_blocks.begin(), standard_blocks.end(), panel) != standard_blocks.end()) {
        m_standard_panel->RemoveColorBlock(panel, update_ui);
    } else if (std::find(unified_blocks.begin(), unified_blocks.end(), panel) != unified_blocks.end()) {
        m_unified_panel->RemoveColorBlock(panel, update_ui);
    }

    if (update_ui && m_use_separation) {
        CallAfter([this]() {
            Layout();
            GetParent()->Layout();
        });
    }
}

std::vector<int> SeparatedDragDropPanel::GetAllFilaments() const
{
    if (m_use_separation) {
        auto high_flow = m_high_flow_panel->GetAllFilaments();
        auto standard  = m_standard_panel->GetAllFilaments();

        std::vector<int> result;
        result.insert(result.end(), high_flow.begin(), high_flow.end());
        result.insert(result.end(), standard.begin(), standard.end());
        return result;
    } else {
        return m_unified_panel->GetAllFilaments();
    }
}

std::vector<int> SeparatedDragDropPanel::GetHighFlowFilaments() const
{
    if (m_use_separation) {
        return m_high_flow_panel->GetAllFilaments();
    }
    return {};
}

std::vector<int> SeparatedDragDropPanel::GetStandardFilaments() const
{
    if (m_use_separation) {
        return m_standard_panel->GetAllFilaments();
    }
    // When not separated, all filaments are treated as whatever the extruder volume type is
    return m_unified_panel->GetAllFilaments();
}

std::vector<ColorPanel *> SeparatedDragDropPanel::get_filament_blocks() const
{
    if (m_use_separation) {
        auto high_flow = m_high_flow_panel->get_filament_blocks();
        auto standard  = m_standard_panel->get_filament_blocks();

        std::vector<ColorPanel *> result;
        result.insert(result.end(), high_flow.begin(), high_flow.end());
        result.insert(result.end(), standard.begin(), standard.end());
        return result;
    } else {
        return m_unified_panel->get_filament_blocks();
    }
}

std::vector<ColorPanel *> SeparatedDragDropPanel::get_high_flow_blocks() const
{
    return m_high_flow_panel->get_filament_blocks();
}

std::vector<ColorPanel *> SeparatedDragDropPanel::get_standard_blocks() const
{
    return m_standard_panel->get_filament_blocks();
}

void SeparatedDragDropPanel::ClearAllBlocks()
{
    auto high_flow_blocks = m_high_flow_panel->get_filament_blocks();
    for (auto &block : high_flow_blocks) {
        m_high_flow_panel->RemoveColorBlock(block, false);
    }

    auto standard_blocks = m_standard_panel->get_filament_blocks();
    for (auto &block : standard_blocks) {
        m_standard_panel->RemoveColorBlock(block, false);
    }

    auto unified_blocks = m_unified_panel->get_filament_blocks();
    for (auto &block : unified_blocks) {
        m_unified_panel->RemoveColorBlock(block, false);
    }
}

///////////////   SeparatedDragDropPanel  end ////////////////////////

}} // namespace Slic3r::GUI
