#pragma once
#include <cstdint>
namespace v3d {
// Current wire modes: 0 Auto, 1 Masked, 3 Translucent, 5 Opaque.
constexpr std::uint8_t normalize_material_mode(std::uint8_t mode)
{
    return (mode == 0 || mode == 1 || mode == 3 || mode == 5) ? mode : 0;
}
// Column: 0 Opaque, 1 Masked, 2 Translucent. glTF input sidedness is discarded at capture.
constexpr int material_alpha(std::uint8_t source_type, std::uint8_t override_mode)
{
    switch (normalize_material_mode(override_mode)) {
    case 1: return 1;
    case 3: return 2;
    case 5: return 0;
    default: break;
    }
    switch (source_type) {
    case 1: case 3: return 2;
    case 4: case 5: return 1;
    default: return 0;
    }
}
constexpr std::uint8_t single_sided_material_type(std::uint8_t source_type)
{
    return material_alpha(source_type, 0) == 1 ? 4 : material_alpha(source_type, 0) == 2 ? 1 : 0;
}
// Registry slot: Default static/dynamic models share alpha parents; Toon always uses slot 3.
constexpr int material_parent_slot(int family, int alpha)
{
    return family == 2 ? 3 : alpha;
}
// Explicit dynamic use (character, item, vehicle, dynamic actor) enables fade even when the
// source model was authored as Static.
constexpr bool material_is_dynamic(int dynamic_state, int request_family)
{
    return request_family == 1 || dynamic_state != 0;
}
constexpr float material_fade_distance(bool dynamic)
{
    return dynamic ? 50.0f : -1.0f;
}
}
