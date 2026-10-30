// Frame interpolation's matching (frontend/frame_matching.*): draws of one frame found again in the next, the camera's
// motion, instances of one mesh paired by where they are, and cuts. Frames are made up the way MHFU draws: the view
// matrix stays put and the camera is inside every world matrix.

#include "frame_matching.hpp"

#include "check.hpp"

#include <cmath>
#include <vector>

namespace {

using namespace mhf::psp;
using namespace mhf::psp::interpolation;

constexpr float kPi = 3.14159265358979f;

// A camera at `eye`, turned `yaw` degrees about the vertical axis: what takes the world into eye space.
Matrix camera(float yaw, float x, float z) {
    const float a = yaw * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
    Matrix turn = identity();
    turn[0] = c;
    turn[2] = -s;
    turn[8] = s;
    turn[10] = c;
    Matrix move = identity();
    move[12] = -x;
    move[14] = -z;
    return multiply(turn, move);
}

Matrix at(float x, float y, float z) {
    Matrix m = identity();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

std::array<float, 12> to_4x3(const Matrix &m) {
    std::array<float, 12> out{};
    for (std::size_t column = 0; column < 4u; ++column) {
        for (std::size_t row = 0; row < 3u; ++row) out[column * 3u + row] = m[column * 4u + row];
    }
    return out;
}

// A draw of mesh `mesh` (its vertex address) placed at `place` in the world, seen by `cam`.
DrawSummary draw(std::uint32_t mesh, const Matrix &cam, const Matrix &place) {
    gpu::DrawInfo info;
    info.vertex_address = mesh;
    info.vertex_type = 0x11Cu;
    info.count = 36;
    info.primitive = 3;
    info.target = 0x88000u;
    info.world = to_4x3(multiply(cam, place)); // the camera inside the world matrix
    info.view = to_4x3(identity());
    info.projection = {1.5f, 0, 0, 0, 0, 2.6f, 0, 0, 0, 0, -1.0f, -1.0f, 0, 0, -20.0f, 0};
    return summarize(info);
}

struct Scene {
    std::vector<Matrix> props;  // drawn once each, from their own vertices
    std::vector<Matrix> tiles;  // one ground mesh drawn many times
};

std::vector<DrawSummary> frame(const Scene &scene, const Matrix &cam, const std::vector<std::size_t> &tile_order) {
    std::vector<DrawSummary> out;
    for (std::size_t i = 0; i < scene.props.size(); ++i) out.push_back(draw(0x08900000u + static_cast<std::uint32_t>(i) * 0x1000u, cam, scene.props[i]));
    for (const std::size_t t : tile_order) out.push_back(draw(0x08A00000u, cam, scene.tiles[t]));
    mark_eligible(out, 0x88000u);
    return out;
}

} // namespace

int main() {
    Scene scene;
    for (int i = 0; i < 12; ++i) scene.props.push_back(at(static_cast<float>(i * 300 - 1800), 0, -600.0f - static_cast<float>(i * 40)));
    for (int i = 0; i < 20; ++i) scene.tiles.push_back(at(static_cast<float>((i % 5) * 400 - 800), -50, static_cast<float>((i / 5) * -400 - 200)));

    // The camera turns 4 degrees and steps forward; the game draws other ground pieces in another order (one piece
    // leaves the view, one comes in): every piece is found again by where it is, not by drawing order.
    {
        Matcher matcher;
        const std::vector<std::size_t> before_order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
        const std::vector<std::size_t> after_order{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
        const auto older = frame(scene, camera(10.0f, 0, 0), before_order);
        const auto newer = frame(scene, camera(14.0f, 0, -30.0f), after_order);
        const Matching &m = matcher.match(older, newer, CutThresholds{});
        MHF_CHECK(m.cut == nullptr);
        MHF_CHECK(m.camera_found && std::fabs(m.camera_angle_degrees - 4.0f) < 0.05f);
        MHF_CHECK(m.repaired > 0);
        // Older piece k (scene tile before_order[k]) pairs with the newer draw of the same tile.
        const std::size_t first = scene.props.size();
        int right = 0;
        for (std::size_t k = 0; k < before_order.size(); ++k) {
            const std::int32_t partner = m.newer_of[first + k];
            if (partner < 0) continue;
            right += after_order[static_cast<std::size_t>(partner) - first] == before_order[k] ? 1 : 0;
        }
        MHF_CHECK(right == 18); // tile 0 left the view: no partner of its own
        MHF_CHECK(m.newer_of[first] < 0);
        for (std::size_t i = 0; i < first; ++i) MHF_CHECK(m.newer_of[i] == static_cast<std::int32_t>(i)); // props: by identity
    }

    // Standing still, drawn in the same order: paired in order, nothing repaired, no motion.
    {
        Matcher matcher;
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < scene.tiles.size(); ++i) order.push_back(i);
        const auto f = frame(scene, camera(0, 0, 0), order);
        const Matching &m = matcher.match(f, f, CutThresholds{});
        MHF_CHECK(m.cut == nullptr && m.repaired == 0 && m.camera_angle_degrees < 0.01f && m.camera_distance < 0.01f);
        MHF_CHECK(m.matched == f.size());
    }

    // A thing that moved far on its own in one frame is another thing: it only follows the camera.
    {
        Matcher matcher;
        const std::vector<std::size_t> order{0, 1, 2};
        auto older = frame(scene, camera(0, 0, 0), order);
        Scene moved = scene;
        moved.props[3] = at(5000.0f, 0, -600.0f);
        auto newer = frame(moved, camera(0, 0, 0), order);
        const Matching &m = matcher.match(older, newer, CutThresholds{});
        MHF_CHECK(m.newer_of[3] == Matching::kFollowCamera && m.rejected == 1);
    }

    // A cut: the camera turned 50 degrees in one frame from standing still.
    {
        Matcher matcher;
        const std::vector<std::size_t> order{0, 1, 2, 3};
        const auto still = frame(scene, camera(0, 0, 0), order);
        (void)matcher.match(still, still, CutThresholds{});
        const Matching &m = matcher.match(still, frame(scene, camera(50.0f, 0, 0), order), CutThresholds{});
        MHF_CHECK(m.cut != nullptr);
    }

    // A camera kept in the view matrix (the world matrices only place things): its turn is found all the same, from
    // the change of the view matrix if need be.
    {
        Matcher matcher;
        const auto in_view = [&](const Matrix &cam) {
            std::vector<DrawSummary> out;
            for (std::size_t t = 0; t < scene.tiles.size(); ++t) {
                gpu::DrawInfo info;
                info.vertex_address = 0x08A00000u;
                info.vertex_type = 0x11Cu;
                info.count = 36;
                info.primitive = 3;
                info.target = 0x88000u;
                info.world = to_4x3(scene.tiles[t]);
                info.view = to_4x3(cam);
                info.projection = {1.5f, 0, 0, 0, 0, 2.6f, 0, 0, 0, 0, -1.0f, -1.0f, 0, 0, -20.0f, 0};
                out.push_back(summarize(info));
            }
            mark_eligible(out, 0x88000u);
            return out;
        };
        const Matching &m = matcher.match(in_view(camera(0, 0, 0)), in_view(camera(6.0f, 0, -20.0f)), CutThresholds{});
        MHF_CHECK(m.cut == nullptr && m.camera_found && std::fabs(m.camera_angle_degrees - 6.0f) < 0.05f);
        MHF_CHECK(m.camera_votes == m.camera_voters);
    }

    // Following the camera's turn part of the way: half of a 10-degree turn is a 5-degree turn.
    {
        const RigidMotion motion = rigid_motion(camera(10.0f, 0, 0));
        MHF_CHECK(motion.valid && std::fabs(rotation_angle_degrees(identity(), rigid_at(motion, 0.5f)) - 5.0f) < 0.01f);
    }

    return mhf::test::finish("frame_matching");
}
