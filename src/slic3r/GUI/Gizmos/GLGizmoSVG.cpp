#include "GLGizmoSVG.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_ObjectManipulation.hpp"
#include "slic3r/GUI/MainFrame.hpp" // to update title when add text
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Point.hpp"      
#include "libslic3r/SVG.hpp"      // debug store
#include "libslic3r/Geometry.hpp" // covex hull 2d
#include "libslic3r/Timer.hpp" // covex hull 2d
#include "libslic3r/Emboss.hpp" // heal_shape

#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp" // union_ex

#include "imgui/imgui_stdlib.h" // using std::string for inputs
#include "nanosvg/nanosvg.h"    // load SVG file

#include <wx/display.h> // detection of change DPI
#include <boost/log/trivial.hpp>

#include <GL/glew.h>
#include <chrono> // measure enumeration of fonts

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

GLGizmoSVG::GLGizmoSVG(GLCanvas3D &parent)
    : GLGizmoBase(parent, M_ICON_FILENAME, -3)
    , m_gui_cfg(nullptr)
    , m_rotate_gizmo(parent, GLGizmoRotate::Axis::Z) // grab id = 2 (Z axis)
{
    m_rotate_gizmo.set_group_id(0);
    m_rotate_gizmo.set_force_local_coordinate(true);
}

// Private functions to create emboss volume
namespace{

// Variable keep limits for variables
const struct Limits
{
    MinMax<float> emboss{0.01f, 1e4f}; // in mm
    // distance text object from surface
    MinMax<float> angle{-180.f, 180.f}; // in degrees
} limits;

/// <summary>
/// Open file dialog with svg files
/// </summary>
/// <returns>File path to svg</returns>
std::string choose_svg_file();

constexpr double get_tesselation_tolerance(double scale){ 
    constexpr double tesselation_tolerance_in_mm = .1; //8e-2;
    constexpr double tesselation_tolerance_scaled = (tesselation_tolerance_in_mm*tesselation_tolerance_in_mm) / SCALING_FACTOR / SCALING_FACTOR;
    return tesselation_tolerance_scaled / scale / scale;
}
ExPolygonsWithIds create_shape_with_ids(const NSVGimage &image, double tesselation_tolerance);

/// <summary>
/// Let user to choose file with (S)calable (V)ector (G)raphics - SVG.
/// Than let select contour
/// </summary>
/// <param name="filepath">SVG file path, when empty promt user to select one</param>
/// <returns>EmbossShape to create</returns>
EmbossShape select_shape(std::string_view filepath = "", double tesselation_tolerance_in_mm = get_tesselation_tolerance(1.));

/// <summary>
/// Create new embos data
/// </summary>
/// <param name="cancel">Cancel for previous job</param>
/// <param name="volume_type">To distiquish whether it is outside of model</param>
/// <param name="filepath">SVG file path</param>
/// <returns>Base data for emboss SVG</returns>
DataBasePtr create_emboss_data_base(std::shared_ptr<std::atomic<bool>> &cancel, ModelVolumeType volume_type, std::string_view filepath = "");

/// <summary>
/// Separate file name from file path.
/// String after last delimiter and before last point 
/// </summary>
/// <param name="file_path">path return by file dialog</param>
/// <returns>File name without directory path</returns>
std::string get_file_name(const std::string &file_path);

/// <summary>
/// Create volume name from shape information
/// </summary>
/// <param name="shape">File path</param>
/// <returns>Name for volume</returns>
std::string volume_name(const EmbossShape& shape);

/// <summary>
/// Create input for volume creation
/// </summary>
/// <param name="canvas">parent of gizmo</param>
/// <param name="raycaster">Keep scene</param>
/// <param name="volume_type">Type of volume to be created</param>
/// <returns>Params</returns>
CreateVolumeParams create_input(GLCanvas3D &canvas, RaycastManager &raycaster, ModelVolumeType volume_type);

enum class IconType : unsigned {
    reset_value,
    reset_value_hover,
    refresh,
    refresh_hover,
    change_file,
    change_file_hover,
    bake,
    bake_hover,
    save,
    save_hover,
    lock,
    lock_hover,
    unlock,
    unlock_hover,
    reflection_x,
    reflection_x_hover,
    reflection_y,
    reflection_y_hover,
    // automatic calc of icon's count
    _count
};
// Do not forgot add loading of file in funtion:
// IconManager::Icons init_icons(

const IconManager::Icon &get_icon(const IconManager::Icons &icons, IconType type) { 
    return *icons[static_cast<unsigned>(type)]; }

// This configs holds GUI layout size given by translated texts.
// etc. When language changes, GUI is recreated and this class constructed again,
// so the change takes effect. (info by GLGizmoFdmSupports.hpp)
struct GuiCfg
{
    // Detect invalid config values when change monitor DPI
    double screen_scale;
    float  main_toolbar_height;

    // Define bigger size(width or height)
    unsigned texture_max_size_px = 256;

    // Zero means it is calculated in init function
    ImVec2 minimal_window_size = ImVec2(0, 0);

    float input_width  = 0.f;
    float input_offset = 0.f;

    float icon_width   = 0.f;

    // offset for checbox for lock up vector
    float lock_offset = 0.f;
    // Only translations needed for calc GUI size
    struct Translations
    {
        std::string depth;
        std::string size;
        std::string use_surface;
        std::string rotation;
        std::string distance; // from surface
        std::string reflection;
    };
    Translations translations;
};
GuiCfg create_gui_configuration();

} // namespace 

// use private definition
struct GLGizmoSVG::GuiCfg: public ::GuiCfg{};

namespace Slic3r {
BoundingBox get_extents(const ExPolygonsWithIds &expoly_ids)
{
    BoundingBox result;
    for (const ExPolygonsWithId &expoly_id : expoly_ids)
        result.merge(get_extents(expoly_id.expoly));
    return result;
}
} // namespace Slic3r

bool GLGizmoSVG::create_volume(ModelVolumeType volume_type, const Vec2d &mouse_pos)
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel, volume_type);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume(input, std::move(base), mouse_pos);
}

bool GLGizmoSVG::create_volume(ModelVolumeType volume_type) 
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel,volume_type);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume_without_position(input, std::move(base));
}

bool GLGizmoSVG::create_volume(std::string_view svg_file, ModelVolumeType volume_type, const Vec2d &mouse_pos)
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel, volume_type, svg_file);
    if (!base) return false; // Uninterpretable svg
    // is not a number || is infinity
    if (mouse_pos.x() != mouse_pos.x() ||
        mouse_pos.y() != mouse_pos.y())
        return start_create_volume_without_position(input, std::move(base));
    return start_create_volume(input, std::move(base), mouse_pos);
}

bool GLGizmoSVG::is_svg(const ModelVolume &volume) {
    return volume.emboss_shape.has_value();
}

bool GLGizmoSVG::is_svg_object(const ModelVolume &volume) {
    if (!volume.emboss_shape.has_value()) return false;
    if (volume.type() != ModelVolumeType::MODEL_PART) return false;
    for (const ModelVolume *v : volume.get_object()->volumes) {
        if (v->id() == volume.id()) continue;
        if (v->type() == ModelVolumeType::MODEL_PART) return false;
    }
    return true;
}

namespace {
TransformationType get_transformation_type(const Selection &selection)
{
    assert(selection.is_single_full_object() || selection.is_single_volume());
    return selection.is_single_volume() ? 
        TransformationType::Local_Relative_Joint :
        TransformationType::Instance_Relative_Joint; // object
}
} // namespace

bool GLGizmoSVG::on_mouse_for_rotation(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving()) return false;

    bool used = use_grabbers(mouse_event);
    if (!m_dragging) return used;

    if (mouse_event.Dragging()) {
        if (!m_rotate_start_angle.has_value())
            m_rotate_start_angle = m_angle.value_or(0.f);
        double angle = m_rotate_gizmo.get_angle();
        angle -= PI / 2; // Grabber is upward

        // temporary rotation
        Selection &selection = m_parent.get_selection();
        selection.rotate(Vec3d(0., 0., angle), get_transformation_type(selection));

        angle += *m_rotate_start_angle;
        // move to range <-M_PI, M_PI>
        Geometry::to_range_pi_pi(angle);
        // propagate angle into property
        m_angle = static_cast<float>(angle);

        // do not store zero
        if (is_approx(*m_angle, 0.f))
            m_angle.reset();
    }
    return used;
}

bool GLGizmoSVG::on_mouse_for_translate(const wxMouseEvent &mouse_event)
{
    // exist selected volume?
    if (m_volume == nullptr)
        return false;

    std::optional<double> up_limit;
    if (m_keep_up)
        up_limit = Slic3r::GUI::up_limit;
    const Camera &camera = wxGetApp().plater()->get_camera();

    bool was_dragging = m_surface_drag.has_value();
    bool res = on_mouse_surface_drag(mouse_event, camera, m_surface_drag, m_parent, m_raycast_manager, up_limit);
    bool is_dragging  = m_surface_drag.has_value();

    // End with surface dragging?
    if (was_dragging && !is_dragging) {
        // Update surface by new position
        if (m_volume->emboss_shape->projection.use_surface)
            process();

        // TODO: Remove it when it will be stable
        // Distance should not change during dragging
        const GLVolume *gl_volume = m_parent.get_selection().get_first_volume();
        m_distance = calc_distance(*gl_volume, m_raycast_manager, m_parent);

        // Show correct value of height & depth inside of inputs
        calculate_scale();
    }

    // Start with dragging
    else if (!was_dragging && is_dragging) {
        // Cancel job to prevent interuption of dragging (duplicit result)
        if (m_job_cancel != nullptr)
            m_job_cancel->store(true);
    }

    // during drag
    else if (was_dragging && is_dragging) {
        // update scale of selected volume --> should be approx the same
        calculate_scale();

        // Recalculate angle for GUI
        if (!m_keep_up) {
            const GLVolume *gl_volume = get_selected_gl_volume(m_parent.get_selection());
            assert(gl_volume != nullptr);
            m_angle = calc_up(gl_volume->world_matrix(), Slic3r::GUI::up_limit);
        }
    }
    return res;
}

bool GLGizmoSVG::on_mouse(const wxMouseEvent &mouse_event)
{
    // not selected volume
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, m_parent.get_selection().get_model()->objects) == nullptr ||
        !m_volume->emboss_shape.has_value()) return false;

    if (on_mouse_for_rotation(mouse_event)) return true;
    if (on_mouse_for_translate(mouse_event)) return true;

    return false;
}

bool GLGizmoSVG::wants_enter_leave_snapshots() const { return true; }
std::string GLGizmoSVG::get_gizmo_entering_text() const { return _u8L("Enter SVG gizmo"); }
std::string GLGizmoSVG::get_gizmo_leaving_text() const { return _u8L("Leave SVG gizmo"); }
std::string GLGizmoSVG::get_action_snapshot_name() const { return _u8L("SVG actions"); }

bool GLGizmoSVG::on_init()
{
    m_rotate_gizmo.init();
    ColorRGBA gray_color(.6f, .6f, .6f, .3f);
    m_rotate_gizmo.set_highlight_color(gray_color);
    // Set rotation gizmo upwardrotate
    m_rotate_gizmo.set_angle(PI / 2);
    return true;
}

std::string GLGizmoSVG::on_get_name() const { return _u8L("SVG"); }

void GLGizmoSVG::on_render() {
    // no volume selected
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, m_parent.get_selection().get_model()->objects) == nullptr)
        return;

    Selection &selection = m_parent.get_selection();
    if (selection.is_empty()) return;

    // prevent get local coordinate system on multi volumes
    if (!selection.is_single_volume_or_modifier() && 
        !selection.is_single_volume_instance()) return;
    bool is_surface_dragging = m_surface_drag.has_value();
    bool is_parent_dragging = m_parent.is_mouse_dragging();
    // Do NOT render rotation grabbers when dragging object
    bool is_rotate_by_grabbers = m_dragging;
    if (is_rotate_by_grabbers || 
        (!is_surface_dragging && !is_parent_dragging)) {
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        m_rotate_gizmo.render();
    }
}

void GLGizmoSVG::on_register_raycasters_for_picking(){
    m_rotate_gizmo.register_raycasters_for_picking();
}
void GLGizmoSVG::on_unregister_raycasters_for_picking(){
    m_rotate_gizmo.unregister_raycasters_for_picking();
}

namespace{
IconManager::Icons init_icons(IconManager &mng, const GuiCfg &cfg)
{ 
    mng.release();
    
    ImVec2 size(cfg.icon_width, cfg.icon_width);
    // icon order has to match the enum IconType
    IconManager::InitTypes init_types{
        {"undo.svg",         size, IconManager::RasterType::white_only_data}, // undo           
        {"undo.svg",         size, IconManager::RasterType::color},           // undo_hovered
        {"refresh.svg",      size, IconManager::RasterType::white_only_data}, // refresh           
        {"refresh.svg",      size, IconManager::RasterType::color},           // refresh_hovered
        {"open.svg",         size, IconManager::RasterType::white_only_data}, // changhe_file
        {"open.svg",         size, IconManager::RasterType::color},           // changhe_file_hovered
        {"burn.svg",         size, IconManager::RasterType::white_only_data}, // bake_file
        {"burn.svg",         size, IconManager::RasterType::color},           // bake_hovered
        {"save.svg",         size, IconManager::RasterType::white_only_data}, // save
        {"save.svg",         size, IconManager::RasterType::color},           // save_hovered
        {"lock_closed.svg",  size, IconManager::RasterType::white_only_data}, // lock
        {"lock_open_f.svg",  size, IconManager::RasterType::white_only_data}, // lock_hovered
        {"lock_open.svg",    size, IconManager::RasterType::white_only_data}, // unlock
        {"lock_closed_f.svg",size, IconManager::RasterType::white_only_data}, // unlock_hovered
        {"reflection_x.svg", size, IconManager::RasterType::white_only_data}, // reflection_x
        {"reflection_x.svg", size, IconManager::RasterType::color},           // reflection_x_hovered
        {"reflection_y.svg", size, IconManager::RasterType::white_only_data}, // reflection_y
        {"reflection_y.svg", size, IconManager::RasterType::color},           // reflection_y_hovered
    };

    assert(init_types.size() == static_cast<size_t>(IconType::_count));
    std::string path = resources_dir() + "/icons/";
    for (IconManager::InitType &init_type : init_types)
        init_type.filepath = path + init_type.filepath;

    return mng.init(init_types);

    //IconManager::VIcons vicons = mng.init(init_types);
    //
    //// flatten icons
    //IconManager::Icons  icons;
    //icons.reserve(vicons.size());
    //for (IconManager::Icons &i : vicons)
    //    icons.push_back(i.front());
    //return icons;
}

bool reset_button(const IconManager::Icons &icons)
{
    float reset_offset = ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(reset_offset);

    // from GLGizmoCut
    //std::string label_id = "neco";
    //std::string btn_label;
    //btn_label += ImGui::RevertButton;
    //return ImGui::Button((btn_label + "##" + label_id).c_str());

    return clickable(get_icon(icons, IconType::reset_value), get_icon(icons, IconType::reset_value_hover));
}

} // namespace 

void GLGizmoSVG::on_render_input_window(float x, float y, float bottom_limit)
{
    set_volume_by_selection();

    // Configuration creation
    double screen_scale = wxDisplay(wxGetApp().plater()).GetScaleFactor();
    float  main_toolbar_height = m_parent.get_main_toolbar_height();
    if (m_gui_cfg == nullptr || // Exist configuration - first run
        m_gui_cfg->screen_scale != screen_scale || // change of DPI
        m_gui_cfg->main_toolbar_height != main_toolbar_height // change size of view port
        ) {
        // Create cache for gui offsets
        ::GuiCfg cfg = create_gui_configuration();
        cfg.screen_scale = screen_scale;
        cfg.main_toolbar_height = main_toolbar_height;

        GuiCfg gui_cfg{std::move(cfg)};
        m_gui_cfg = std::make_unique<const GuiCfg>(std::move(gui_cfg));

        // set position near toolbar
        m_set_window_offset = ImVec2(-1.f, -1.f);

        m_icons = init_icons(m_icon_manager, *m_gui_cfg); // need regeneration when change resolution(move between monitors)
    }

    const ImVec2 &min_window_size = m_gui_cfg->minimal_window_size;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, min_window_size);

    // Draw origin position of text during dragging
    if (m_surface_drag.has_value()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        ImVec2 center(
            mouse_pos.x + m_surface_drag->mouse_offset.x(),
            mouse_pos.y + m_surface_drag->mouse_offset.y());
        ImU32 color = ImGui::GetColorU32(
            m_surface_drag->exist_hit ? 
                ImVec4(1.f, 1.f, 1.f, .75f) : // transparent white
                ImVec4(1.f, .3f, .3f, .75f)
        ); // Warning color
        const float radius = 16.f;
        ImGuiWrapper::draw_cross_hair(center, radius, color);
    }

    // check if is set window offset
    if (m_set_window_offset.has_value()) {
        if (m_set_window_offset->y < 0)
            // position near toolbar
            m_set_window_offset = ImVec2(x, std::min(y, bottom_limit - min_window_size.y));
        
        ImGui::SetNextWindowPos(*m_set_window_offset, ImGuiCond_Always);
        m_set_window_offset.reset();
    } 

    bool is_opened = true;
    ImGuiWindowFlags flag = ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(on_get_name().c_str(), &is_opened, flag)) {
        // Need to pop var before draw window
        ImGui::PopStyleVar(); // WindowMinSize
        draw_window();
    } else {
        ImGui::PopStyleVar(); // WindowMinSize
    }

    ImGui::End();
    //if (!is_opened)
    //    close();
}

void GLGizmoSVG::on_set_state()
{
    // enable / disable bed from picking
    // Rotation gizmo must work through bed
    m_parent.set_raycaster_gizmos_on_top(GLGizmoBase::m_state == GLGizmoBase::On);

    m_rotate_gizmo.set_state(GLGizmoBase::m_state);

    // Closing gizmo. e.g. selecting another one
    if (GLGizmoBase::m_state == GLGizmoBase::Off) {
        reset_volume();
    } else if (GLGizmoBase::m_state == GLGizmoBase::On) {
        // Try(when exist) set text configuration by volume 
        set_volume_by_selection();
           
        m_set_window_offset = (m_gui_cfg != nullptr) ?
            ImGuiWrapper::change_window_position(on_get_name().c_str(), false) : ImVec2(-1, -1);
    }
}

void GLGizmoSVG::data_changed(bool is_serializing) { 
    set_volume_by_selection();
    if (!is_serializing && m_volume == nullptr)
        close();
}

void GLGizmoSVG::on_start_dragging() { m_rotate_gizmo.start_dragging(); }
void GLGizmoSVG::on_stop_dragging()
{
    m_rotate_gizmo.stop_dragging();

    // TODO: when start second rotatiton previous rotation rotate draggers
    // This is fast fix for second try to rotate
    // When fixing, move grabber above text (not on side)
    m_rotate_gizmo.set_angle(PI/2);

    // apply rotation
    m_parent.do_rotate(L("Text-Rotate"));

    m_rotate_start_angle.reset();

    // recalculate for surface cut
    if (m_volume != nullptr && 
        m_volume->emboss_shape.has_value() &&
        m_volume->emboss_shape->projection.use_surface)
        process();
}
void GLGizmoSVG::on_dragging(const UpdateData &data) { m_rotate_gizmo.dragging(data); }

#include "slic3r/GUI/BitmapCache.hpp"
#include "nanosvg/nanosvgrast.h"
#include "libslic3r/AABBTreeLines.hpp" // aabb lines for draw filled expolygon

namespace{
NSVGimage* init_image(EmbossShape::SvgFile &svg_file) {
    // is already initialized?
    if (svg_file.image.get() != nullptr)
        return svg_file.image.get();


    if (svg_file.file_data == nullptr){
        // chech if path is known
        if (svg_file.path.empty())
            return nullptr;
        svg_file.file_data = read_from_disk(svg_file.path);
        if (svg_file.file_data == nullptr)
            return nullptr;
    }

    // init svg image
    svg_file.image = nsvgParse(svg_file.file_data);
    if (svg_file.image.get() == NULL)
        return nullptr;

    // Disable stroke
    for (NSVGshape *shape = svg_file.image->shapes; shape != NULL; shape = shape->next)
        shape->stroke.type = 0;

    return svg_file.image.get();
}

// inspired by Xiaolin Wu's line algorithm - https://en.wikipedia.org/wiki/Xiaolin_Wu's_line_algorithm
// Draw inner part of polygon CCW line as full brightness(edge of expolygon)
void wu_draw_line_side(Linef line,
                const std::function<void(int x, int y, float brightess)>& plot) {
    auto ipart = [](float x) -> int {return static_cast<int>(std::floor(x));};
    auto round = [](float x) -> float {return std::round(x);};
    auto fpart = [](float x) -> float {return x - std::floor(x);};
    auto rfpart = [=](float x) -> float {return 1 - fpart(x);};
    
    Vec2d d = line.b - line.a;
    const bool steep = abs(d.y()) > abs(d.x());
    bool is_full; // identify full brightness pixel
    if (steep) {
        is_full = d.y() >= 0;
        std::swap(line.a.x(), line.a.y());
        std::swap(line.b.x(), line.b.y());
        std::swap(d.x(), d.y());
    }else
        is_full = d.x() < 0; // opposit direction of y

    if (line.a.x() > line.b.x()) {
        std::swap(line.a.x(), line.b.x());
        std::swap(line.a.y(), line.b.y());
        d *= -1;
    }
    const float gradient = (d.x() == 0) ? 1. : d.y() / d.x();
        
    int xpx11;
    float intery;
    {
        const float xend = round(line.a.x());
        const float yend = line.a.y() + gradient * (xend - line.a.x());
        const float xgap = rfpart(line.a.x() + 0.5f);
        xpx11 = int(xend);
        const int ypx11 = ipart(yend);
        if (steep) {
            plot(ypx11,     xpx11,  is_full? 1.f : (rfpart(yend) * xgap));
            plot(ypx11 + 1, xpx11, !is_full? 1.f : ( fpart(yend) * xgap));
        } else {
            plot(xpx11, ypx11,     is_full? 1.f : (rfpart(yend) * xgap));
            plot(xpx11, ypx11 + 1,!is_full? 1.f : ( fpart(yend) * xgap));
        }
        intery = yend + gradient;
    }
    
    int xpx12;
    {
        const float xend = round(line.b.x());
        const float yend = line.b.y() + gradient * (xend - line.b.x());
        const float xgap = rfpart(line.b.x() + 0.5);
        xpx12 = int(xend);
        const int ypx12 = ipart(yend);
        if (steep) {
            plot(ypx12,     xpx12,  is_full? 1.f : (rfpart(yend) * xgap));
            plot(ypx12 + 1, xpx12, !is_full? 1.f : ( fpart(yend) * xgap));
        } else {
            plot(xpx12, ypx12,      is_full? 1.f : (rfpart(yend) * xgap));
            plot(xpx12, ypx12 + 1, !is_full? 1.f : ( fpart(yend) * xgap));
        }
    }
        
    if (steep) {
        if (is_full){
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(ipart(intery),     x, 1.f);
                plot(ipart(intery) + 1, x, fpart(intery));
                intery += gradient;
            }
        } else {
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(ipart(intery),     x, rfpart(intery));
                plot(ipart(intery) + 1, x, 1.f );
                intery += gradient;
            }
        }
    } else {
        if (is_full){
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(x, ipart(intery),     1.f);
                plot(x, ipart(intery) + 1, fpart(intery));
                intery += gradient;
            }
        } else {
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(x, ipart(intery),     rfpart(intery));
                plot(x, ipart(intery) + 1, 1.f);
                intery += gradient;
            }
        }
    }
}

// Wu's line algorithm - https://en.wikipedia.org/wiki/Xiaolin_Wu's_line_algorithm
void wu_draw_line(Linef line,
                const std::function<void(int x, int y, float brightess)>& plot) {
    auto ipart = [](float x) -> int {return int(std::floor(x));};
    auto round = [](float x) -> float {return std::round(x);};
    auto fpart = [](float x) -> float {return x - std::floor(x);};
    auto rfpart = [=](float x) -> float {return 1 - fpart(x);};
    
    Vec2d d = line.b - line.a;
    const bool steep = abs(d.y()) > abs(d.x());
    if (steep) {
        std::swap(line.a.x(), line.a.y());
        std::swap(line.b.x(), line.b.y());
    }
    if (line.a.x() > line.b.x()) {
        std::swap(line.a.x(), line.b.x());
        std::swap(line.a.y(), line.b.y());
    }
    d = line.b - line.a;
    const float gradient = (d.x() == 0) ? 1 : d.y() / d.x();
        
    int xpx11;
    float intery;
    {
        const float xend = round(line.a.x());
        const float yend = line.a.y() + gradient * (xend - line.a.x());
        const float xgap = rfpart(line.a.x() + 0.5);
        xpx11 = int(xend);
        const int ypx11 = ipart(yend);
        if (steep) {
            plot(ypx11,     xpx11,  rfpart(yend) * xgap);
            plot(ypx11 + 1, xpx11,   fpart(yend) * xgap);
        } else {
            plot(xpx11, ypx11,    rfpart(yend) * xgap);
            plot(xpx11, ypx11 + 1, fpart(yend) * xgap);
        }
        intery = yend + gradient;
    }
    
    int xpx12;
    {
        const float xend = round(line.b.x());
        const float yend = line.b.y() + gradient * (xend - line.b.x());
        const float xgap = rfpart(line.b.x() + 0.5);
        xpx12 = int(xend);
        const int ypx12 = ipart(yend);
        if (steep) {
            plot(ypx12,     xpx12, rfpart(yend) * xgap);
            plot(ypx12 + 1, xpx12,  fpart(yend) * xgap);
        } else {
            plot(xpx12, ypx12,     rfpart(yend) * xgap);
            plot(xpx12, ypx12 + 1,  fpart(yend) * xgap);
        }
    }
        
    if (steep) {
        for (int x = xpx11 + 1; x < xpx12; x++) {
            plot(ipart(intery),     x, rfpart(intery));
            plot(ipart(intery) + 1, x,  fpart(intery));
            intery += gradient;
        }
    } else {
        for (int x = xpx11 + 1; x < xpx12; x++) {
            plot(x, ipart(intery),     rfpart(intery));
            plot(x, ipart(intery) + 1,  fpart(intery));
            intery += gradient;
        }
    }
}

/// <summary>
/// Draw filled ExPolygon into data 
/// line by line inspired by: http://alienryderflex.com/polygon_fill/
/// </summary>
/// <typeparam name="N">Count channels for one pixel(RGBA = 4)</typeparam>
/// <param name="shape">Shape to draw</param>
/// <param name="color">Color of shape</param>
/// <param name="data">Image(2d) stored in 1d array</param>
/// <param name="data_width">Count of pixel on one line(size in data = N x data_width)</param>
/// <param name="scale">Shape scale for conversion to pixels</param>
template<unsigned int N>
void draw_filled(const ExPolygons &shape, const std::array<unsigned char, N>& color, std::vector<unsigned char> &data, size_t data_width, double scale = 1.){
    assert(data.size() % N == 0);
    assert(data.size() % data_width == 0);
    assert((data.size() % (N*data_width)) == 0);

    BoundingBox bb_unscaled = get_extents(shape);
    
    Linesf lines = to_linesf(shape);
    BoundingBoxf bb(
        bb_unscaled.min.cast<double>(), 
        bb_unscaled.max.cast<double>());

    // scale lines to pixels
    if (!is_approx(scale, 1.)) {
        for (Linef &line : lines) {
            line.a *= scale;
            line.b *= scale;
        }
        bb.min *= scale;
        bb.max *= scale;
    }
    auto tree = Slic3r::AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);

    int count_lines = data.size() / (N * data_width);
    size_t data_line = N * data_width;
    auto get_offset = [count_lines, data_line](int x, int y) {
        // NOTE: y has opposit direction in texture
        return (count_lines - y - 1) * data_line + x * N;
    };
    auto set_color = [&data, &color, get_offset](int x, int y) {
        size_t offset = get_offset(x, y);
        if (data[offset + N - 1] != 0)
            return; // already setted by line
        for (size_t i = 0; i < N; ++i)
            data[offset + i] = color[i];
    };

    // anti aliased drawing of lines
    auto draw = [&data, data_width, count_lines, get_offset, &color](int x, int y, float brightess) {
        if (x < 0 || y < 0 || x >= data_width || y >= count_lines)
            return; // out of image
        size_t offset = get_offset(x, y);
        unsigned char &alpha = data[offset + N - 1];
        if (alpha == 0){
            alpha = static_cast<unsigned char>(std::round(brightess * 255));
            for (size_t i = 0; i < N-1; ++i)
                data[offset + i] = color[i];
        } else if (alpha != 255){
            alpha = static_cast<unsigned char>(std::min(255, int(alpha) + static_cast<int>(std::round(brightess * 255))));
        }
    };
    for (const Linef& line: lines) wu_draw_line_side(line, draw);
    

    // range for intersection line
    double x1 = bb.min.x() - 1.f;
    double x2 = bb.max.x() + 1.f;

    int max_y = std::min(count_lines, static_cast<int>(std::round(bb.max.y())));
    for (int y = std::max(0, static_cast<int>(std::round(bb.min.y()))); y < max_y; ++y){
        double y_f = y + .5; // 0.5 ... intersection in center of pixel of pixel
        Linef line(Vec2d(x1, y_f), Vec2d(x2, y_f));
        using Intersection = std::pair<Vec2d, size_t>;
        using Intersections = std::vector<Intersection>;
        // sorted .. false
        // <false, Vec2d, Linef, decltype(tree)>
        Intersections intersections = Slic3r::AABBTreeLines::get_intersections_with_line<false, Vec2d, Linef>(lines, tree, line);
        if (intersections.empty())
            continue;

        assert((intersections.size() % 2) == 0);

        // sort intersections by x
        std::sort(intersections.begin(), intersections.end(), 
            [](const Intersection &i1, const Intersection &i2) { return i1.first.x() < i2.first.x(); });

        // draw lines
        for (size_t i = 0; i < intersections.size(); i+=2) {
            const Vec2d& p2 = intersections[i+1].first;
            if (p2.x() < 0)
                continue; // out of data

            const Vec2d& p1 = intersections[i].first;
            if (p1.x() > data_width)
                break; // out of data

            // clamp to data
            int max_x = std::min(static_cast<int>(data_width-1), static_cast<int>(std::round(p2.x())));
            for (int x = std::max(0, static_cast<int>(std::round(p1.x()))); x <= max_x; ++x)
                set_color(x, y);
        }
    }  
}

// init texture by draw expolygons into texture
bool init_texture(Texture &texture, const ExPolygonsWithIds& shapes_with_ids, unsigned max_size_px){
    BoundingBox bb = get_extents(shapes_with_ids);
    Point  bb_size   = bb.size();
    double bb_width  = bb_size.x(); // [in mm]
    double bb_height = bb_size.y(); // [in mm]

    bool is_widder = bb_size.x() > bb_size.y();
    double scale = 0.f;
    if (is_widder) {
        scale          = max_size_px / bb_width;
        texture.width  = max_size_px;
        texture.height = static_cast<unsigned>(std::ceil(bb_height * scale));
    } else {
        scale          = max_size_px / bb_height;
        texture.width  = static_cast<unsigned>(std::ceil(bb_width * scale));
        texture.height = max_size_px;
    }
    const int n_pixels = texture.width * texture.height;
    if (n_pixels <= 0)
        return false;

    constexpr int channels_count = 4;
    std::vector<unsigned char> data(n_pixels * channels_count, {0});

    // Union All shapes
    ExPolygons shape;
    for (const ExPolygonsWithId &shapes_with_id : shapes_with_ids)
        expolygons_append(shape, shapes_with_id.expoly);
    shape = union_ex(shape);

    // align to texture
    for (ExPolygon& expolygon: shape)
        expolygon.translate(-bb.min);

    unsigned char alpha = 255; // without transparency
    std::array<unsigned char, 4> color{201, 201, 201, alpha};
    draw_filled(shape, color, data, texture.width, scale);

    // sends data to gpu 
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    if (texture.id != 0)
        glsafe(::glDeleteTextures(1, &texture.id));
    glsafe(::glGenTextures(1, &texture.id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture.id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) texture.width, (GLsizei) texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          (const void *) data.data()));

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    GLuint NO_TEXTURE_ID = 0;
    glsafe(::glBindTexture(GL_TEXTURE_2D, NO_TEXTURE_ID));
    return true;
}
} 

void GLGizmoSVG::set_volume_by_selection()
{
    const Selection &selection = m_parent.get_selection();
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    if (gl_volume == nullptr)
        return reset_volume();

    const ModelObjectPtrs &objects = selection.get_model()->objects;
    ModelVolume *volume =get_model_volume(*gl_volume, objects);
    if (volume == nullptr)
        return reset_volume();

    // is same volume as actual selected?
    if (volume->id() == m_volume_id)
        return;

    // Do not use focused input value when switch volume(it must swith value)
    if (m_volume != nullptr && 
        m_volume != volume) // when update volume it changed id BUT not pointer
        ImGuiWrapper::left_inputs();

    // is valid svg volume?
    if (!is_svg(*volume)) 
        return reset_volume();

    // cancel previous job
    if (m_job_cancel != nullptr) {
        m_job_cancel->store(true);
        m_job_cancel = nullptr;
    }
        
    // calculate scale for height and depth inside of scaled object instance
    calculate_scale(); // must be before calculation of tesselation

    EmbossShape &es = *volume->emboss_shape;
    ExPolygonsWithIds &shape_ids = es.shapes_with_ids;
    if (shape_ids.empty()) {
        // volume loaded from .3mf need to create shapes from svg
        NSVGimage* image = init_image(es.svg_file);
        assert(image != nullptr);
        if (image != nullptr){
            double tes_tol = get_tesselation_tolerance(std::max(m_scale_width.value_or(1.f), m_scale_height.value_or(1.f)));
            shape_ids = create_shape_with_ids(*image, tes_tol);
        }        
    }

    reset_volume(); // clear cached data

    m_volume = volume;
    m_volume_id = volume->id();
    m_volume_shape = *volume->emboss_shape; // copy

    // Calculate current angle of up vector
    m_angle = calc_up(gl_volume->world_matrix(), Slic3r::GUI::up_limit);
    m_distance = calc_distance(*gl_volume, m_raycast_manager, m_parent);
    
    m_shape_bb = get_extents(m_volume_shape.shapes_with_ids);
}
namespace {
void delete_texture(Texture& texture){
    if (texture.id != 0) {
        glsafe(::glDeleteTextures(1, &texture.id));
        texture.id = 0;
    }
}
}
void GLGizmoSVG::reset_volume()
{
    if (m_volume == nullptr)
        return; // already reseted

    m_volume = nullptr;
    m_volume_id.id = 0;
    m_volume_shape.shapes_with_ids.clear();
    m_filename_preview.clear();
    delete_texture(m_texture);
}

void GLGizmoSVG::calculate_scale() {
    Transform3d to_world = m_parent.get_selection().get_first_volume()->world_matrix();
    auto to_world_linear = to_world.linear();
    auto calc = [&to_world_linear](const Vec3d &axe, std::optional<float>& scale)->bool {
        Vec3d  axe_world = to_world_linear * axe;
        double norm_sq   = axe_world.squaredNorm();
        if (is_approx(norm_sq, 1.)) {
            if (scale.has_value())
                scale.reset();
            else
                return false;
        } else {
            scale = sqrt(norm_sq);
        }
        return true;
    };

    calc(Vec3d::UnitX(), m_scale_width);
    calc(Vec3d::UnitY(), m_scale_height);
    calc(Vec3d::UnitZ(), m_scale_depth);
}

bool GLGizmoSVG::process()
{
    // no volume is selected -> selection from right panel
    assert(m_volume != nullptr);
    if (m_volume == nullptr) 
        return false;
    
    assert(m_volume->emboss_shape.has_value());
    if (!m_volume->emboss_shape.has_value())
        return false;

    // Cancel previous Job, when it is in process
    // worker.cancel(); --> Use less in this case I want cancel only previous EmbossJob no other jobs
    // Cancel only EmbossUpdateJob no others
    if (m_job_cancel != nullptr)
        m_job_cancel->store(true);
    // create new shared ptr to cancel new job
    m_job_cancel = std::make_shared<std::atomic<bool>>(false);

    EmbossShape shape = m_volume_shape; // copy
    auto base = std::make_unique<DataBase>(m_volume->name, m_job_cancel, std::move(shape));
    base->is_outside = m_volume->type() == ModelVolumeType::MODEL_PART;
    DataUpdate data{std::move(base), m_volume_id};
    return start_update_volume(std::move(data), *m_volume, m_parent.get_selection(), m_raycast_manager);    
}

void GLGizmoSVG::close()
{
    reset_volume();
    // close gizmo == open it again
    auto& mng = m_parent.get_gizmos_manager();
    if (mng.get_current_type() == GLGizmosManager::Svg)
        mng.open_gizmo(GLGizmosManager::Svg);
}

void GLGizmoSVG::draw_window()
{
    assert(m_volume != nullptr);
    assert(m_volume_id.valid());
    if (m_volume == nullptr ||
        m_volume_id.invalid()) {
        ImGui::Text("Not valid state please report reproduction steps on github");
        return;
    }
    if(!draw_preview())
        return;

    ImGui::Separator();

    ImGui::Indent(m_gui_cfg->icon_width);
    draw_depth();
    draw_size();
    draw_use_surface();

    draw_distance();
    draw_rotation();
    draw_reflection();

    if (ImGui::Button(_u8L("Face the camera").c_str())) {
        const Camera &cam = wxGetApp().plater()->get_camera();
        if (face_selected_volume_to_camera(cam, m_parent) && 
            m_volume->emboss_shape->projection.use_surface)
            process();
    }

    ImGui::Unindent(m_gui_cfg->icon_width);  

    if (!m_volume->is_the_only_one_part()) {
        ImGui::Separator();
        draw_model_type();
    }
}
namespace {
size_t count(const NSVGshape &shape){
    size_t res = 0;
    for (const NSVGshape *shape_ptr = &shape; shape_ptr != NULL; shape_ptr = shape_ptr->next)
        ++res;
    return res;
}

void draw(const ExPolygonsWithIds& shapes_with_ids, unsigned max_size)
{
    ImVec2 actual_pos = ImGui::GetCursorPos();
    // draw shapes
    BoundingBox bb;
    for (const ExPolygonsWithId &shape : shapes_with_ids)
        bb.merge(get_extents(shape.expoly));

    Point  bb_size    = bb.size();
    double scale      = max_size / (double) std::max(bb_size.x(), bb_size.y());
    ImVec2 win_offset = ImGui::GetWindowPos();
    Point  offset(win_offset.x + actual_pos.x, win_offset.y + actual_pos.y);
    offset += bb_size / 2 * scale;
    auto draw_polygon = [&scale, offset](Slic3r::Polygon p) {
        p.scale(scale, -scale); // Y mirror
        p.translate(offset);
        ImGuiWrapper::draw(p);
    };

    for (const ExPolygonsWithId &shape : shapes_with_ids) {
        for (const ExPolygon &expoly : shape.expoly) {
            draw_polygon(expoly.contour);
            for (const Slic3r::Polygon &hole : expoly.holes)
                draw_polygon(hole);
        }
    }
}
}
bool GLGizmoSVG::draw_preview(){
    assert(m_volume->emboss_shape.has_value());
    if (!m_volume->emboss_shape.has_value()) {
        ImGui::Text("No embossed file");
        return false;
    }

    const EmbossShape &es = *m_volume->emboss_shape;
    // init texture when not initialized yet.
    // drag&drop is out of rendering scope so texture must be created on this place
    if (m_texture.id == 0)
        init_texture(m_texture, es.shapes_with_ids, m_gui_cfg->texture_max_size_px);

    //::draw(m_volume_shape.shapes_with_ids, m_gui_cfg->texture_max_size_px);

    if (m_texture.id != 0) {
        ImTextureID id = (void *) static_cast<intptr_t>(m_texture.id);
        ImVec2      s(m_texture.width, m_texture.height);
        ImGui::Image(id, s);
        if(ImGui::IsItemHovered()){
            size_t count_shapes = ::count(*es.svg_file.image->shapes);
            ImGui::SetTooltip("%d count shapes", count_shapes);
        }
    }

    if (m_filename_preview.empty()){
        // create filename preview
        m_filename_preview = get_file_name(es.svg_file.path);
        m_filename_preview = ImGuiWrapper::trunc(m_filename_preview, m_gui_cfg->input_width);
    }

    // Remove space between filename and gray suffix ".svg"
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    ImGui::Text("%s", m_filename_preview.c_str());
    bool is_hovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, ".svg");
    ImGui::PopStyleVar(); // ImGuiStyleVar_ItemSpacing 

    is_hovered |= ImGui::IsItemHovered();
    if (is_hovered) {
        std::string tooltip = GUI::format(_L("SVG file path is \"%1%\" "), es.svg_file.path);
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    bool file_changed = false;

    // Re-Load button
    bool can_reload = !m_volume_shape.svg_file.path.empty();
    if (can_reload) {
        ImGui::SameLine();
        if (clickable(get_icon(m_icons, IconType::refresh), get_icon(m_icons, IconType::refresh_hover))) {
            if (!boost::filesystem::exists(m_volume_shape.svg_file.path)) {
                m_volume_shape.svg_file.path.clear();
            } else {
                file_changed = true;
            }
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Re-load SVG file from disk.").c_str());
    }

    ImGui::SameLine();
    if (clickable(get_icon(m_icons, IconType::change_file), get_icon(m_icons, IconType::change_file_hover))) {
        std::string new_path = choose_svg_file();
        if (!new_path.empty()) {
            file_changed = true;
            m_volume_shape.svg_file.path = new_path;
        }
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _u8L("Change to another .svg file").c_str());
    }

    if (file_changed) {
        double tes_tol = get_tesselation_tolerance(std::max(m_scale_width.value_or(1.f), m_scale_height.value_or(1.f)));
        EmbossShape es_ = select_shape(m_volume_shape.svg_file.path, tes_tol);
        m_volume_shape.svg_file.image = std::move(es_.svg_file.image);
        m_volume_shape.shapes_with_ids = std::move(es_.shapes_with_ids);
        init_texture(m_texture, m_volume_shape.shapes_with_ids, m_gui_cfg->texture_max_size_px);
        process();
    }

    ImGui::SameLine();
    if (clickable(get_icon(m_icons, IconType::bake), get_icon(m_icons, IconType::bake_hover))) {
        m_volume->emboss_shape.reset();
        close();
        return false;
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _u8L("Bake to uneditable part and save copyright of svg").c_str());
    }

    ImGui::SameLine();
    if (clickable(get_icon(m_icons, IconType::save), get_icon(m_icons, IconType::save_hover))) {
        GUI::FileType file_type  = FT_SVG;
        wxString wildcard = file_wildcards(file_type);
        wxString dlg_title = _L("Export SVG file:");
        wxString dlg_dir = from_u8(wxGetApp().app_config->get_last_dir());
        const EmbossShape::SvgFile& svg = m_volume_shape.svg_file;
        wxString dlg_file = from_u8(get_file_name(((!svg.path.empty()) ? svg.path : svg.path_in_3mf))) + ".svg";
        wxFileDialog dlg(nullptr, dlg_title, dlg_dir, dlg_file, wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK ){
            wxString out_path = dlg.GetPath();        
            std::string path{out_path.c_str()};
            //Slic3r::save(*m_volume_shape.svg_file.image, path);

            std::ofstream stream(path);
            if (stream.is_open()){
                stream << svg.file_data.get();
            } else {
                BOOST_LOG_TRIVIAL(error) << "Opening file: \"" << path << "\" Failed";
            }
        }
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _u8L("Save as '.svg' file").c_str());
    }
    return true;
}

void GLGizmoSVG::draw_depth()
{
    ImGuiWrapper::text(m_gui_cfg->translations.depth);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    double &value = m_volume_shape.projection.depth;
    if (use_inch) {
        const char *size_format = "%.2f in";
        double value_inch = value * ObjectManipulation::mm_to_in * m_scale_depth.value_or(1.f);
        if (ImGui::InputDouble("##depth", &value_inch, 1., 10., size_format)) {
            value = value_inch * ObjectManipulation::in_to_mm / m_scale_depth.value_or(1.f);
            process();
        }
    } else if (m_scale_depth.has_value()) {
        const char *size_format = "%.1f mm";
        double value_mm = value * (*m_scale_depth);
        if (ImGui::InputDouble("##depth", &value_mm, 1., 10., size_format)) {
            value = value_mm / (*m_scale_depth);
            process();
        }
    } else {
        const char *size_format = "%.1f mm";
        if (ImGui::InputDouble("##depth", &value, 1., 10., size_format))
            process();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Size in emboss direction.").c_str());
}

void GLGizmoSVG::draw_size() 
{
    ImGuiWrapper::text(m_gui_cfg->translations.size);

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    
    Point size = m_shape_bb.size();
    double width = size.x() * m_volume_shape.scale * m_scale_width.value_or(1.f);
    if (use_inch) width *= ObjectManipulation::mm_to_in;    
    double height = size.y() * m_volume_shape.scale * m_scale_height.value_or(1.f);
    if (use_inch) height *= ObjectManipulation::mm_to_in;

    std::optional<Vec3d> new_relative_scale;
    const double minimal_scale_ratio_change = 1e-4;

    if (m_keep_ratio) {
        std::stringstream ss;
        ss << std::setprecision(2) << std::fixed << width << " x " << height << " " << (use_inch ? "in" : "mm");

        ImGui::SameLine(m_gui_cfg->input_offset);
        ImGui::SetNextItemWidth(m_gui_cfg->input_width);

        // convert to float for slider
        float width_f = width;
        if (m_imgui->slider_float("##width_size_slider", &width_f, 5.f, 100.f, ss.str().c_str(), 1.f, false)) {
            double width_ratio = width_f / width;
            if (std::fabs(width_ratio - 1.) > minimal_scale_ratio_change) {
                m_scale_width      = m_scale_width.value_or(1.f) * width_ratio;
                m_scale_height     = m_scale_height.value_or(1.f) * width_ratio;
                new_relative_scale = Vec3d(width_ratio, width_ratio, 1.);
            }
        }
    } else {
        ImGuiInputTextFlags flags = 0;

        float space         = m_gui_cfg->icon_width / 2;
        float input_width   = m_gui_cfg->input_width / 2 - space / 2;
        float second_offset = m_gui_cfg->input_offset + input_width + space;

        const char *size_format = (use_inch) ? "%.2f in" : "%.1f mm";
        double step = -1.0;
        double fast_step = -1.0;

        ImGui::SameLine(m_gui_cfg->input_offset);
        ImGui::SetNextItemWidth(input_width);
        double prev_width = width;
        if (ImGui::InputDouble("##width", &width, step, fast_step, size_format, flags)) {
            double width_ratio = width / prev_width;
            if (std::fabs(width_ratio - 1.) > minimal_scale_ratio_change) {
                m_scale_width = m_scale_width.value_or(1.f) * width_ratio;
                new_relative_scale = Vec3d(width_ratio, 1., 1.);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Width of SVG.");

        ImGui::SameLine(second_offset);
        ImGui::SetNextItemWidth(input_width);
        double prev_height = height;
        if (ImGui::InputDouble("##height", &height, step, fast_step, size_format, flags)) {
            double height_ratio = height / prev_height;
            if (std::fabs(height_ratio - 1.) > minimal_scale_ratio_change) {
                m_scale_height = m_scale_height.value_or(1.f) * height_ratio;
                new_relative_scale  = Vec3d(1., height_ratio, 1.);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Height of SVG.");    
    }

    // Lock on ratio m_keep_ratio
    ImGui::SameLine(m_gui_cfg->lock_offset);
    const IconManager::Icon &icon       = get_icon(m_icons, m_keep_ratio ? IconType::lock : IconType::unlock);
    const IconManager::Icon &icon_hover = get_icon(m_icons, m_keep_ratio ? IconType::lock_hover : IconType::unlock_hover);
    if (button(icon, icon_hover, icon))
        m_keep_ratio = !m_keep_ratio;    
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (m_keep_ratio ?
            _u8L("Free set of width and height value."):
            _u8L("Keep same ratio of width to height.")
        ).c_str());
    

    // reset button
    bool can_reset = m_scale_width.has_value() || m_scale_height.has_value() || m_scale_depth.has_value();
    if (can_reset) {
        if (reset_button(m_icons)) {
            new_relative_scale = Vec3d(1./m_scale_width.value_or(1.f), 1./m_scale_height.value_or(1.f), 1.);
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reset scale to loaded one from the SVG").c_str());
    }

    if (new_relative_scale.has_value()){
        Selection &selection = m_parent.get_selection();
        selection.setup_cache();
        TransformationType type = m_volume->is_the_only_one_part() ? 
            TransformationType::Instance_Relative_Independent :
            TransformationType::Local_Relative_Independent;
        selection.scale(*new_relative_scale, type);
        m_parent.do_scale(L("Resize"));
        wxGetApp().obj_manipul()->set_dirty();
        // should be the almost same
        calculate_scale();
                
        NSVGimage *img = m_volume_shape.svg_file.image.get();
        assert(img != NULL);
        if (img != NULL){
            double tes_tol = get_tesselation_tolerance(std::max(m_scale_width.value_or(1.f), m_scale_height.value_or(1.f)));
            m_volume_shape.shapes_with_ids = create_shape_with_ids(*img, tes_tol);
            process();        
        }
    }
}

void GLGizmoSVG::draw_use_surface() 
{
    bool can_use_surface = (m_volume->emboss_shape->projection.use_surface)? true : // already used surface must have option to uncheck
        !m_volume->is_the_only_one_part();
    m_imgui->disabled_begin(!can_use_surface);
    ScopeGuard sc([imgui = m_imgui]() { imgui->disabled_end(); });

    ImGuiWrapper::text(m_gui_cfg->translations.use_surface);
    ImGui::SameLine(m_gui_cfg->input_offset);

    if (ImGui::Checkbox("##useSurface", &m_volume_shape.projection.use_surface))
        process();
}

void GLGizmoSVG::draw_distance()
{
    const EmbossProjection& projection = m_volume->emboss_shape->projection;
    bool use_surface = projection.use_surface;
    bool allowe_surface_distance = !use_surface && !m_volume->is_the_only_one_part();

    float prev_distance = m_distance.value_or(.0f);
    float min_distance = static_cast<float>(-2 * projection.depth);
    float max_distance = static_cast<float>(2 * projection.depth);
 
    m_imgui->disabled_begin(!allowe_surface_distance);
    ScopeGuard sg([imgui = m_imgui]() { imgui->disabled_end(); });

    ImGuiWrapper::text(m_gui_cfg->translations.distance);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    const wxString move_tooltip = _L("Distance of the center of the text to the model surface.");
    bool is_moved = false;
    if (use_inch) {
        std::optional<float> distance_inch;
        if (m_distance.has_value()) distance_inch = (*m_distance * ObjectManipulation::mm_to_in);
        min_distance = static_cast<float>(min_distance * ObjectManipulation::mm_to_in);
        max_distance = static_cast<float>(max_distance * ObjectManipulation::mm_to_in);
        if (m_imgui->slider_optional_float("##distance", m_distance, min_distance, max_distance, "%.3f in", 1.f, false, move_tooltip)) {
            if (distance_inch.has_value()) {
                m_distance = *distance_inch * ObjectManipulation::in_to_mm;
            } else {
                m_distance.reset();
            }
            is_moved = true;
        }
    } else {
        if (m_imgui->slider_optional_float("##distance", m_distance, min_distance, max_distance, "%.2f mm", 1.f, false, move_tooltip)) 
            is_moved = true;
    }

    bool can_reset = m_distance.has_value();
    if (can_reset) {
        if (reset_button(m_icons)) {
            m_distance.reset();
            is_moved = true;
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reset distance to zero value").c_str());
    }

    if (is_moved)
        do_local_z_move(m_parent, m_distance.value_or(.0f) - prev_distance);
}

void GLGizmoSVG::draw_rotation()
{        
    ImGuiWrapper::text(m_gui_cfg->translations.rotation);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    // slider for Clock-wise angle in degress
    // stored angle is optional CCW and in radians
    // Convert stored value to degress
    // minus create clock-wise roation from CCW
    float angle = m_angle.value_or(0.f);
    float angle_deg = static_cast<float>(-angle * 180 / M_PI);
    if (m_imgui->slider_float("##angle", &angle_deg, limits.angle.min, limits.angle.max, u8"%.2f °", 1.f, false, _L("Rotate text Clock-wise."))){
        // convert back to radians and CCW
        double angle_rad = -angle_deg * M_PI / 180.0;
        Geometry::to_range_pi_pi(angle_rad);                

        double diff_angle = angle_rad - angle;
        do_local_z_rotate(m_parent, diff_angle);
        
        // calc angle after rotation
        const GLVolume *gl_volume = get_selected_gl_volume(m_parent.get_selection());
        m_angle = calc_up(gl_volume->world_matrix(), Slic3r::GUI::up_limit);
        
        // recalculate for surface cut
        if (m_volume->emboss_shape->projection.use_surface)
            process();
    }

    // Reset button
    if (m_angle.has_value()) {
        if (reset_button(m_icons)) {
            do_local_z_rotate(m_parent, -(*m_angle));
            m_angle.reset();

            // recalculate for surface cut
            if (m_volume->emboss_shape->projection.use_surface)
                process();
        } else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reset rotation to zero value").c_str());
    }

    // Keep up - lock button icon
    if (!m_volume->is_the_only_one_part()) {
        ImGui::SameLine(m_gui_cfg->lock_offset);
        const IconManager::Icon &icon = get_icon(m_icons,m_keep_up ? IconType::lock : IconType::unlock);
        const IconManager::Icon &icon_hover = get_icon(m_icons, m_keep_up ? IconType::lock_hover : IconType::unlock_hover);
        if (button(icon, icon_hover, icon))
            m_keep_up = !m_keep_up;    
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (m_keep_up?
                _u8L("Free angle when dragging above the object's surface."):
                _u8L("Keep same rotation angle when dragging above the object's surface.")
            ).c_str());
    }
}

void GLGizmoSVG::draw_reflection()
{
    ImGui::Text("%s", m_gui_cfg->translations.reflection.c_str());
    ImGui::SameLine(m_gui_cfg->input_offset);
    Axis axis = Axis::UNKNOWN_AXIS;
    if(clickable(get_icon(m_icons, IconType::reflection_x), get_icon(m_icons, IconType::reflection_x_hover))){
        axis = Axis::X;
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _u8L("Reflect by 2d Y axis").c_str());
    }

    ImGui::SameLine();
    if (clickable(get_icon(m_icons, IconType::reflection_y), get_icon(m_icons, IconType::reflection_y_hover))) {
        axis = Axis::Y;
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _u8L("Reflect by 2d X axis").c_str());
    }

    if (axis != Axis::UNKNOWN_AXIS){
        Selection &selection = m_parent.get_selection();
        selection.setup_cache();
        TransformationType type = m_volume->is_the_only_one_part()? TransformationType::Instance : TransformationType::Local;
        selection.mirror(axis, type);
        m_parent.do_mirror(L("Set Mirror"));
        wxGetApp().obj_manipul()->UpdateAndShow(true);

        if (m_volume_shape.projection.use_surface)
            process();
    }
}

void GLGizmoSVG::draw_model_type()
{
    bool is_last_solid_part = m_volume->is_the_only_one_part();
    std::string title = _u8L("Operation");
    if (is_last_solid_part) {
        ImVec4 color{.5f, .5f, .5f, 1.f};
        m_imgui->text_colored(color, title.c_str());
    } else {
        ImGui::Text("%s", title.c_str());
    }

    std::optional<ModelVolumeType> new_type;
    ModelVolumeType modifier = ModelVolumeType::PARAMETER_MODIFIER;
    ModelVolumeType negative = ModelVolumeType::NEGATIVE_VOLUME;
    ModelVolumeType part = ModelVolumeType::MODEL_PART;
    ModelVolumeType type = m_volume->type();

    //TRN EmbossOperation
    if (ImGui::RadioButton(_u8L("Join").c_str(), type == part))
        new_type = part;
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Click to change text into object part.").c_str());
    ImGui::SameLine();

    std::string last_solid_part_hint = _u8L("You can't change a type of the last solid part of the object.");
    if (ImGui::RadioButton(_CTX_utf8(L_CONTEXT("Cut", "EmbossOperation"), "EmbossOperation").c_str(), type == negative))
        new_type = negative;
    else if (ImGui::IsItemHovered()) {
        if (is_last_solid_part)
            ImGui::SetTooltip("%s", last_solid_part_hint.c_str());
        else if (type != negative)
            ImGui::SetTooltip("%s", _u8L("Click to change part type into negative volume.").c_str());
    }

    // In simple mode are not modifiers
    if (wxGetApp().plater()->printer_technology() != ptSLA && wxGetApp().get_mode() != ConfigOptionMode::comSimple) {
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Modifier").c_str(), type == modifier))
            new_type = modifier;
        else if (ImGui::IsItemHovered()) {
            if (is_last_solid_part)
                ImGui::SetTooltip("%s", last_solid_part_hint.c_str());
            else if (type != modifier)
                ImGui::SetTooltip("%s", _u8L("Click to change part type into modifier.").c_str());
        }
    }

    if (m_volume != nullptr && new_type.has_value() && !is_last_solid_part) {
        GUI_App &app    = wxGetApp();
        Plater * plater = app.plater();
        Plater::TakeSnapshot snapshot(plater, _L("Change SVG Type"), UndoRedo::SnapshotType::GizmoAction);
        m_volume->set_type(*new_type);

        bool is_volume_move_inside  = (type == part);
        bool is_volume_move_outside = (*new_type == part);
         // Update volume position when switch (from part) or (into part)
        if ((is_volume_move_inside || is_volume_move_outside))
            process();

        // inspiration in ObjectList::change_part_type()
        // how to view correct side panel with objects
        ObjectList *obj_list = app.obj_list();
        wxDataViewItemArray sel = obj_list->reorder_volumes_and_get_selection(
            obj_list->get_selected_obj_idx(),
            [volume = m_volume](const ModelVolume *vol) { return vol == volume; });
        if (!sel.IsEmpty()) obj_list->select_item(sel.front());       

        // NOTE: on linux, function reorder_volumes_and_get_selection call GLCanvas3D::reload_scene(refresh_immediately = false)
        // which discard m_volume pointer and set it to nullptr also selection is cleared so gizmo is automaticaly closed
        auto &mng = m_parent.get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::Svg)
            mng.open_gizmo(GLGizmosManager::Svg);
        // TODO: select volume back - Ask @Sasa
    }
}


/////////////
// private namespace implementation
///////////////
namespace {

std::string get_file_name(const std::string &file_path)
{
    if (file_path.empty())
        return file_path;

    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    if (pos_last_delimiter == std::string::npos) {
        // should not happend that in path is not delimiter
        assert(false);
        pos_last_delimiter = 0;
    }

    size_t pos_point = file_path.find_last_of('.');
    if (pos_point == std::string::npos || pos_point < pos_last_delimiter // last point is inside of directory path
    ) {
        // there is no extension
        assert(false);
        pos_point = file_path.size();
    }

    size_t offset = pos_last_delimiter + 1;             // result should not contain last delimiter ( +1 )
    size_t count  = pos_point - pos_last_delimiter - 1; // result should not contain extension point ( -1 )
    return file_path.substr(offset, count);
}

std::string volume_name(const EmbossShape &shape)
{
    std::string file_name = get_file_name(shape.svg_file.path);
    if (!file_name.empty())
        return file_name;
    return "SVG shape";
}

CreateVolumeParams create_input(GLCanvas3D &canvas, RaycastManager& raycaster, ModelVolumeType volume_type)
{
    auto gizmo = static_cast<unsigned char>(GLGizmosManager::Svg);
    const GLVolume *gl_volume = get_first_hovered_gl_volume(canvas);
    Plater *plater = wxGetApp().plater();
    return CreateVolumeParams{canvas, plater->get_camera(), plater->build_volume(),
        plater->get_ui_job_worker(), volume_type, raycaster, gizmo, gl_volume};
}

GuiCfg create_gui_configuration() {
    GuiCfg cfg; // initialize by default values;

    float line_height = ImGui::GetTextLineHeight();
    float line_height_with_spacing = ImGui::GetTextLineHeightWithSpacing();

    float space = line_height_with_spacing - line_height;

    cfg.icon_width = std::max(std::round(line_height/8)*8, 8.f);    

    GuiCfg::Translations &tr = cfg.translations;

    float lock_width = cfg.icon_width + 3 * space;
    tr.depth       = _u8L("Depth");
    tr.size        = _u8L("Size");
    tr.use_surface = _u8L("Use surface");
    tr.distance    = _u8L("From surface");
    tr.rotation    = _u8L("Rotation");
    tr.reflection  = _u8L("Reflection");
    float max_tr_width = std::max({
        ImGui::CalcTextSize(tr.depth.c_str()).x,
        ImGui::CalcTextSize(tr.size.c_str()).x + lock_width,
        ImGui::CalcTextSize(tr.use_surface.c_str()).x,
        ImGui::CalcTextSize(tr.distance.c_str()).x,
        ImGui::CalcTextSize(tr.rotation.c_str()).x + lock_width,
        ImGui::CalcTextSize(tr.reflection.c_str()).x,
    });

    const ImGuiStyle &style = ImGui::GetStyle();
    cfg.input_offset = style.WindowPadding.x + max_tr_width + space + cfg.icon_width;
    cfg.lock_offset = cfg.input_offset - (cfg.icon_width + 2 * space);

    ImVec2 letter_m_size = ImGui::CalcTextSize("M");
    const float count_letter_M_in_input = 12.f;
    cfg.input_width = letter_m_size.x * count_letter_M_in_input;
    cfg.texture_max_size_px = std::round((cfg.input_width + cfg.input_offset + cfg.icon_width +space)/8) * 8;
    return cfg;
}

std::string choose_svg_file()
{
    wxWindow    *parent       = nullptr;
    wxString     message      = _L("Choose SVG file for emboss:");
    wxString     defaultDir   = wxEmptyString;
    wxString     selectedFile = wxEmptyString;
    wxString     wildcard     = file_wildcards(FT_SVG);
    long         style        = wxFD_OPEN | wxFD_FILE_MUST_EXIST;
    wxFileDialog dialog(parent, message, defaultDir, selectedFile, wildcard, style);
    if (dialog.ShowModal() != wxID_OK) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file for emboss was NOT selected.";
        return {};
    }

    wxArrayString input_files;
    dialog.GetPaths(input_files);
    if (input_files.IsEmpty()) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog result is empty.";
        return {};
    }

    if (input_files.size() != 1)
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog result contain multiple files but only first is used.";

    auto &input_file = input_files.front();
    std::string path = std::string(input_file.c_str());

    if (!boost::filesystem::exists(path)) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog return invalid path.";
        return {};
    }

    if (!boost::algorithm::iends_with(path, ".svg")) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog return path without '.svg' tail";
        return {};    
    }

    return path;
}

void translate(ExPolygons &expolys, const Point &p) {
    for (ExPolygon &expoly : expolys)
        expoly.translate(p);
}

ExPolygonsWithIds create_shape_with_ids(const NSVGimage &image, double tesselation_tolerance)
{
    int max_level = 10;
    bool is_y_negative = true;
    ExPolygons expoly = to_expolygons(image, tesselation_tolerance, max_level, 1.0/SCALING_FACTOR, is_y_negative);
    if (expoly.empty())
        return {};

    expoly = union_ex(expoly);
    unsigned max_iteration = 10;
    if (!Slic3r::Emboss::heal_shape(expoly, max_iteration))
        return {};

    // SVG is used as centered
    // Do not disturb user by settings of pivot position
    BoundingBox bb = get_extents(expoly);
    translate(expoly, -bb.center());

    // Preparation for multi shape in svg
    unsigned id = 0;
    return {{id, expoly}};
}

EmbossShape select_shape(std::string_view filepath, double tesselation_tolerance)
{
    EmbossShape shape;
    shape.projection.depth       = 10.;
    shape.projection.use_surface = false;

    if (filepath.empty()) {
        // When empty open file dialog
        shape.svg_file.path = choose_svg_file();
        if (shape.svg_file.path.empty())            
            return {}; // file was not selected
    } else {
        shape.svg_file.path = filepath; // copy
    }
    
    if (!boost::filesystem::exists(shape.svg_file.path)) {
        show_error(nullptr, GUI::format(_u8L("File(%1%) does NOT exists."), shape.svg_file.path));
        return {};
    }

    if (!boost::algorithm::iends_with(shape.svg_file.path, ".svg")){
        show_error(nullptr, GUI::format(_u8L("File has to end with \".svg\" but you select: %1%"), shape.svg_file.path));
        return {};
    }

    init_image(shape.svg_file);
    if (shape.svg_file.image.get() == NULL) {
        show_error(nullptr, GUI::format(_u8L("Nano SVG parser can't load from file(%1%)."), shape.svg_file.path));
        return {};
    }

    // Set default and unchanging scale
    shape.shapes_with_ids = create_shape_with_ids(*shape.svg_file.image, tesselation_tolerance);

    // Must contain some shapes !!!
    if (shape.shapes_with_ids.empty()) {
        show_error(nullptr, GUI::format(_u8L("SVG file(%1%) do NOT contain path to be able embossed."), shape.svg_file.path));
        return {};
    }
    return shape;
}

DataBasePtr create_emboss_data_base(std::shared_ptr<std::atomic<bool>> &cancel, ModelVolumeType volume_type, std::string_view filepath)
{
    EmbossShape shape = select_shape(filepath);

    if (shape.shapes_with_ids.empty())
        // canceled selection of SVG file
        return nullptr;

    // Cancel previous Job, when it is in process
    // worker.cancel(); --> Use less in this case I want cancel only previous EmbossJob no other jobs
    // Cancel only EmbossUpdateJob no others
    if (cancel != nullptr)
        cancel->store(true);
    // create new shared ptr to cancel new job
    cancel = std::make_shared<std::atomic<bool>>(false);

    std::string name = volume_name(shape);

    auto result = std::make_unique<DataBase>(name, cancel /*copy*/, std::move(shape));
    result->is_outside = volume_type == ModelVolumeType::MODEL_PART;
    return result;
}
} // namespace

// any existing icon filename to not influence GUI
const std::string GLGizmoSVG::M_ICON_FILENAME = "cut.svg";
