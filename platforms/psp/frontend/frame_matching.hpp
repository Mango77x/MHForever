#pragma once

// Frame interpolation, second design (docs/05-runtime-hle.md §6): the game simulates 30 frames a second, and the
// pictures in between draw the older frame's draws again with their transforms blended toward the newer frame's. This
// part is independent of the backend: it recognises a draw of one frame in the next, works out how the camera moved,
// decides whether two frames may be blended at all, and blends matrices.
//
// Ported from Yakumo's frame interpolation (TeamGDB/Yakumo, profiles/mhp3rd/host/gpu/frame_interpolation.*, MIT;
// see THIRD_PARTY.md), which interpolates MHP3rd: the same kind of engine as MHFU, which also keeps its view matrix
// fixed and puts the camera into every world matrix. The camera's motion is found by pokke's vote instead of Yakumo's
// median (theguyser/pokke, profiles/mhp2g, MIT). Matrices are column-major 4x4, as there.

#include "backend.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mhf::psp::interpolation {

using Matrix = std::array<float, 16>; // column-major

// What matching and blending need to know about one draw.
struct DrawSummary {
    // Identity: the same object drawn in two frames reads the same vertices and indices from the same addresses with
    // the same vertex type and texture. Draws with the same identity (instances of one mesh) pair up in the order the
    // frame draws them, then by where they are (Matcher::pair_nearest_instances).
    std::uint32_t vertex_address{}, index_address{}, vertex_type{}, texture_address{}, count{};
    std::uint8_t primitive{};
    std::uint32_t target{}; // the framebuffer drawn into
    std::uint64_t key_hash{};
    std::array<float, 3> eye_translation{}; // the translation of view times world
    bool perspective{}; // transformed through a perspective projection, and not a clear
    // A perspective draw into the framebuffer the frame showed: only these are blended; 2D, interface, orthographic
    // draws, clears and render-to-texture passes are shown as the frame drew them.
    bool eligible{};
    bool skinned{};
    Matrix world{}, view{}, projection{};
};

DrawSummary summarize(const gpu::DrawInfo &draw);
// Marks the perspective draws into `displayed` eligible.
void mark_eligible(std::vector<DrawSummary> &draws, std::uint32_t displayed) noexcept;
[[nodiscard]] bool is_orthographic(const Matrix &projection) noexcept;

// How two consecutive frames' draws correspond.
struct Matching {
    // For each draw of the older frame: the index of the same draw in the newer frame, kNoPartner, or kFollowCamera
    // for a pair whose own motion is not believable (it is then moved with the camera only).
    std::vector<std::int32_t> newer_of;
    static constexpr std::int32_t kNoPartner = -1;
    static constexpr std::int32_t kFollowCamera = -2;
    std::uint32_t eligible_older{}, eligible_newer{}, matched{};
    std::uint32_t rejected{}, rejected_shared{}; // pairs given up (moved too far on their own), and of a shared mesh
    std::uint32_t repaired{};                    // instances given a nearer partner than drawing order's
    std::uint32_t too_many_instances{};          // meshes with more instances than the cap: left in drawing order
    float max_own_motion{}, max_rejected_motion{};
    // How the camera moved, as the motion of eye space: for scenery that stands still, newer eye = camera * older eye.
    bool camera_found{};
    float camera_angle_degrees{}, camera_distance{};
    Matrix camera{};
    const char *cut{}; // why the two frames must not be blended, or null
    bool continued{};  // blended only because the motion continues the previous pair's
};

// Thresholds for telling a camera cut or a scene change from motion.
struct CutThresholds {
    float min_matched_fraction{0.5f}; // of the newer frame's eligible draws
    float max_camera_angle_degrees{30.0f};
    float max_camera_distance{200.0f}; // world units the camera moves in one frame
    // Motion that continues the previous pair's is not a cut even past the limits above (a camera turned fast with
    // the stick): the limits become the previous pair's times `continuous_growth` plus a margin, up to the max.
    float continuous_growth{1.5f};
    float continuous_angle_margin_degrees{6.0f};
    float continuous_distance_margin{60.0f};
    float max_continuous_angle_degrees{75.0f};
    float max_continuous_distance{800.0f};
    // A pair that moved more than this in eye space beyond the camera's motion is two different objects. 0: off.
    float max_own_motion{120.0f};
    // Instances of one mesh paired by where they are, once the camera's motion is taken out (up to this many).
    bool nearest_instances{true};
    std::uint32_t max_nearest_instances{64u};
};

class Matcher {
public:
    // Pairs `older`'s eligible draws with `newer`'s and decides whether the frames may be blended. Each call is taken
    // to follow the previous one: a pair that was blended lets the next one continue its motion.
    const Matching &match(const std::vector<DrawSummary> &older, const std::vector<DrawSummary> &newer,
                          const CutThresholds &thresholds);
    void forget_motion() noexcept {
        previous_blended_ = false;
        has_previous_camera_ = false;
    }

    static std::uint64_t hash_of(const DrawSummary &draw) noexcept;
    static std::array<float, 3> eye_translation_of(const DrawSummary &draw) noexcept;

private:
    struct Key {
        std::uint32_t vertex_address, index_address, vertex_type, texture_address, count;
        std::uint8_t primitive;
        bool operator==(const Key &) const = default;
    };
    static Key key_of(const DrawSummary &draw) noexcept;
    // One key of the newer frame: its first and last draw, chained in drawing order, and the next one to hand out;
    // first < 0 marks an empty slot. Also the older frame's draws of the key.
    struct Slot {
        Key key{};
        std::int32_t first{-1}, last{-1}, cursor{-1};
        std::uint32_t newer_count{};
        std::uint32_t older_count{};
        std::int32_t older_first{-1}, older_last{-1};
    };
    void pair_nearest_instances(const std::vector<DrawSummary> &older, const std::vector<DrawSummary> &newer,
                                const CutThresholds &thresholds);

    std::vector<Slot> slots_;
    std::vector<std::int32_t> next_, older_slot_, older_next_;
    std::vector<std::uint8_t> shared_, done_slot_;
    struct Candidate {
        float distance;
        std::int32_t older, newer;
    };
    std::vector<Candidate> candidates_;
    std::vector<std::int32_t> instance_older_, instance_newer_, instance_partner_;
    std::vector<std::uint8_t> instance_taken_;
    Matching result_;
    bool previous_blended_{};
    float previous_angle_degrees_{}, previous_distance_{};
    bool has_previous_camera_{};
    Matrix previous_camera_{}; // the last pair's camera motion, which breaks a tie between candidates
};

// Blends two affine transforms: each basis vector keeps a length between the two while its direction turns, and the
// translation moves linearly. t = 0 gives a, 1 gives b.
[[nodiscard]] Matrix blend_affine(const Matrix &a, const Matrix &b, float t) noexcept;
[[nodiscard]] Matrix multiply(const Matrix &a, const Matrix &b) noexcept; // a after b
[[nodiscard]] Matrix blend_linear(const Matrix &a, const Matrix &b, float t) noexcept;
[[nodiscard]] float rotation_angle_degrees(const Matrix &a, const Matrix &b) noexcept;
[[nodiscard]] bool affine_inverse(const Matrix &m, Matrix &out) noexcept;
[[nodiscard]] Matrix identity() noexcept;
// A GE 4x3 matrix (column-major, translation last) as a 4x4.
[[nodiscard]] Matrix from_4x3(const std::array<float, 12> &m) noexcept;

// A texture offset between two frames. A scroll moves it a little each frame; a flipbook (fire, smoke) moves it to the
// next cell of an atlas at once, and blending that would sweep across the cells in between. Steps below `max_scroll`
// are scrolling and blended; bigger ones hold the older offset.
inline constexpr float kMaxScrollStep = 0.1f;
[[nodiscard]] inline bool scrolls(float from, float to, float max_scroll = kMaxScrollStep) noexcept {
    const float step = to > from ? to - from : from - to;
    return step < max_scroll;
}
[[nodiscard]] float blend_offset(float from, float to, float t, float max_scroll = kMaxScrollStep) noexcept;

// A rigid motion taken apart so that it can be followed part of the way: a turn about an axis through a centre, and a
// slide along that axis. Following a camera that orbits its target a fraction of the way keeps every point on its
// arc, where blending matrices would cut across the arc and pull distant scenery in.
struct RigidMotion {
    bool valid{};
    float angle{}; // radians
    std::array<float, 3> axis{0.0f, 1.0f, 0.0f};
    std::array<float, 3> centre{};
    std::array<float, 3> slide{};
    std::array<float, 3> translation{};
    Matrix inverse{};
};
[[nodiscard]] RigidMotion rigid_motion(const Matrix &m) noexcept;
[[nodiscard]] Matrix rigid_at(const RigidMotion &motion, float t) noexcept; // 0 the identity, 1 the motion

// Where a draw is between two frames, in eye space (view times world), when the camera moved by `camera`: its own
// motion is blended in the older frame's eye space and the camera's motion is followed along its arcs.
[[nodiscard]] Matrix blend_eye(const Matrix &older, const Matrix &newer, const RigidMotion &camera, float t) noexcept;

} // namespace mhf::psp::interpolation
