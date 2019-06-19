#ifndef slic3r_Camera_hpp_
#define slic3r_Camera_hpp_

#include "libslic3r/BoundingBox.hpp"
#include <array>

namespace Slic3r {
namespace GUI {

struct Camera
{
    static const double DefaultDistance;
    static double FrustrumMinZSize;
    static double FrustrumZMargin;

    enum EType : unsigned char
    {
        Unknown,
        Perspective,
        Ortho,
        Num_types
    };

    float zoom;
    float phi;
    bool requires_zoom_to_bed;
    bool inverted_phi;

private:
    EType m_type;
    Vec3d m_target;
    float m_theta;
    // Distance between camera position and camera target measured along the camera Z axis
    double m_distance;

    mutable std::array<int, 4> m_viewport;
    mutable Transform3d m_view_matrix;
    mutable Transform3d m_projection_matrix;
    mutable std::pair<double, double> m_frustrum_zs;

    BoundingBoxf3 m_scene_box;

public:
    Camera();

    EType get_type() const { return m_type; }
    std::string get_type_as_string() const;
    void set_type(EType type) { m_type = type; }
    void select_next_type();

    const Vec3d& get_target() const { return m_target; }
    void set_target(const Vec3d& target);

    float get_theta() const { return m_theta; }
    void set_theta(float theta, bool apply_limit);

    const BoundingBoxf3& get_scene_box() const { return m_scene_box; }
    void set_scene_box(const BoundingBoxf3& box){ m_scene_box = box; }

    bool select_view(const std::string& direction);

    const std::array<int, 4>& get_viewport() const { return m_viewport; }
    const Transform3d& get_view_matrix() const { return m_view_matrix; }
    const Transform3d& get_projection_matrix() const { return m_projection_matrix; }

    Vec3d get_dir_right() const { return m_view_matrix.matrix().block(0, 0, 3, 3).row(0); }
    Vec3d get_dir_up() const { return m_view_matrix.matrix().block(0, 0, 3, 3).row(1); }
    Vec3d get_dir_forward() const { return -m_view_matrix.matrix().block(0, 0, 3, 3).row(2); }

    Vec3d get_position() const { return m_view_matrix.matrix().inverse().block(0, 3, 3, 1); }

    double get_near_z() const { return m_frustrum_zs.first; }
    double get_far_z() const { return m_frustrum_zs.second; }

    void apply_viewport(int x, int y, unsigned int w, unsigned int h) const;
    void apply_view_matrix() const;
    void apply_projection(const BoundingBoxf3& box) const;

#if ENABLE_CAMERA_STATISTICS
    void debug_render() const;
#endif // ENABLE_CAMERA_STATISTICS

private:
    // returns tight values for nearZ and farZ plane around the given bounding box
    // the camera MUST be outside of the bounding box in eye coordinate of the given box
    std::pair<double, double> calc_tight_frustrum_zs_around(const BoundingBoxf3& box) const;
};

} // GUI
} // Slic3r

#endif // slic3r_Camera_hpp_

