#include <cmath>
#include <cstdint>

#include "prx/libc/include/General.hpp"
#include "Ngs2Internal.hpp"

static constexpr float DEFAULT_SOUND_SPEED = 343.0f;

int Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param, Ngs2GeomListenerWork* outWork, std::uint32_t flags) {
    if (param == nullptr) APS5_INVALID_ARG_EX;
    if (outWork == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (flags != 0) throw std::runtime_error("NGS2: listener coordinate flags are unsupported");
    for (const Ngs2GeomVector* v : {&param->position, &param->orient_front, &param->orient_up, &param->velocity}) {
        if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z)) APS5_INVALID_ARG_EX;
    }
    if (!std::isfinite(param->sound_speed) || param->sound_speed <= 0.0f) APS5_INVALID_ARG_EX;
    // World-to-listener transform: rows are the listener's right, up and front axes, the last column moves
    // the listener to the origin. The default listener (front +z, up +y, at the origin) gives the identity.
    const auto dot = [](const Ngs2GeomVector& a, const Ngs2GeomVector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto cross = [](const Ngs2GeomVector& a, const Ngs2GeomVector& b) {
        return Ngs2GeomVector{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto normalized = [&](Ngs2GeomVector v) {
        const float length = std::sqrt(dot(v, v));
        if (!std::isfinite(length) || length <= 0.0f) return Ngs2GeomVector{};
        return Ngs2GeomVector{v.x / length, v.y / length, v.z / length};
    };
    // Titles pass zero or parallel orientation vectors (Dead Cells), which the hardware accepts: fall back to the
    // default front (+z) and up (+y) axes rather than rejecting the listener.
    Ngs2GeomVector front = normalized(param->orient_front);
    if (dot(front, front) == 0.0f) front = {0.0f, 0.0f, 1.0f};
    Ngs2GeomVector right = normalized(cross(param->orient_up, front));
    if (dot(right, right) == 0.0f) right = normalized(cross(Ngs2GeomVector{0.0f, 1.0f, 0.0f}, front));
    if (dot(right, right) == 0.0f) right = normalized(cross(Ngs2GeomVector{0.0f, 0.0f, 1.0f}, front));
    const Ngs2GeomVector up = cross(front, right);
    const Ngs2GeomVector axes[3] = {right, up, front};
    Ngs2GeomListenerWork work{};
    for (std::size_t i = 0; i < 3; i++) {
        work.matrix[i][0] = axes[i].x;
        work.matrix[i][1] = axes[i].y;
        work.matrix[i][2] = axes[i].z;
        work.matrix[i][3] = -dot(axes[i], param->position);
    }
    work.matrix[3][3] = 1.0f;
    work.velocity = param->velocity;
    work.sound_speed = param->sound_speed;
    *outWork = work;
    return SCE_NGS2_OK;
}

int Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
    if (out_listener_param == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *out_listener_param = {};
    out_listener_param->orient_front.z = 1.0f;
    out_listener_param->orient_up.y = 1.0f;
    out_listener_param->sound_speed = DEFAULT_SOUND_SPEED;
    return SCE_NGS2_OK;
}

int Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
    if (out_source_param == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *out_source_param = {};
    out_source_param->direction.z = 1.0f;
    out_source_param->cone.inner_level = 1.0f;
    out_source_param->cone.inner_angle = 360.0f;
    out_source_param->cone.outer_level = 1.0f;
    out_source_param->cone.outer_angle = 360.0f;
    out_source_param->rolloff.max_distance = 1000000.0f;
    out_source_param->rolloff.rolloff_factor = 1.0f;
    out_source_param->rolloff.reference_distance = 1.0f;
    out_source_param->doppler_factor = 1.0f;
    out_source_param->fbw_level = 1.0f;
    out_source_param->lfe_level = 1.0f;
    out_source_param->max_level = 1.0f;
    out_source_param->num_speakers = 2;
    out_source_param->matrix_format = 2;
    return SCE_NGS2_OK;
}
