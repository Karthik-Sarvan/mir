/*
 * Copyright © Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "plane_capabilities.h"

#include <mir/logging/logger.h>
#include <mir/test/doubles/mock_drm.h>
#include <mir/test/doubles/null_logger.h>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace mga = mir::graphics::atomic;
namespace mtd = mir::test::doubles;

using namespace testing;

namespace
{
constexpr char const* drm_device{"/dev/dri/card0"};
constexpr uint32_t other_crtc_id{30};
constexpr uint32_t target_crtc_id{31};
constexpr uint32_t no_zpos_crtc_id{32};
constexpr uint32_t other_crtc_mask{0b01};
constexpr uint32_t target_crtc_mask{0b10};
constexpr uint32_t no_zpos_crtc_mask{0b100};
constexpr uint32_t tie_below_primary_id{39};
constexpr uint32_t primary_id{40};
constexpr uint32_t overlay_id{41};
constexpr uint32_t underlay_id{42};
constexpr uint32_t tie_above_primary_id{43};
constexpr uint32_t other_crtc_overlay_id{44};
constexpr uint32_t top_overlay_id{45};
constexpr uint32_t other_primary_id{46};
constexpr uint32_t no_zpos_id{47};
constexpr uint32_t invalid_zpos_id{48};
constexpr uint32_t no_formats_id{49};
constexpr uint32_t fixed_opaque_overlay_id{51};
constexpr uint32_t blend_without_none_with_alpha_id{52};
constexpr uint32_t cursor_id{53};
constexpr uint32_t untyped_id{54};
constexpr uint32_t invalid_alpha_id{55};
constexpr uint32_t second_primary_id{56};
constexpr uint32_t no_zpos_primary_id{57};
constexpr uint32_t inverted_zpos_id{58};
constexpr uint32_t inverted_alpha_id{59};
constexpr uint32_t immutable_range_flags{DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE};
using EnumValues = std::vector<std::pair<uint64_t, std::string>>;

struct InFormatsBlob
{
    drm_format_modifier_blob header{
        FORMAT_BLOB_CURRENT, 0, 2, offsetof(InFormatsBlob, formats), 2, offsetof(InFormatsBlob, modifiers)};
    uint32_t formats[2]{DRM_FORMAT_NV12, DRM_FORMAT_XRGB8888};
    drm_format_modifier modifiers[2]{{0b01, 0, 0, I915_FORMAT_MOD_Y_TILED}, {0b10, 0, 0, DRM_FORMAT_MOD_LINEAR}};

    auto bytes() const -> std::span<std::byte const>
    {
        return std::as_bytes(std::span{this, 1});
    }
};

auto ids_of(std::vector<mga::PlaneCapabilities> const& overlays) -> std::vector<uint32_t>
{
    return overlays | std::views::transform(&mga::PlaneCapabilities::id) | std::ranges::to<std::vector>();
}

auto find_overlay(mga::CrtcPlaneCapabilities const& caps, uint32_t id) -> mga::PlaneCapabilities const&
{
    auto const it = std::ranges::find(caps.overlays, id, &mga::PlaneCapabilities::id);
    if (it == caps.overlays.end())
        throw std::runtime_error("Plane " + std::to_string(id) + " not found");
    return *it;
}

class MockLogger : public mir::logging::Logger
{
public:
    MOCK_METHOD(void, log, (mir::logging::Severity severity, std::string const& message,
                            std::string const& component), (override));
};

class PlaneCapabilitiesTest : public Test
{
public:
    PlaneCapabilitiesTest()
    {
        mir::logging::set_logger(logger);
        mock_drm.reset(drm_device);
        for (auto const id : {other_crtc_id, target_crtc_id, no_zpos_crtc_id})
            mock_drm.add_crtc(drm_device, id, {});
        for (auto const id : {primary_id, underlay_id, no_zpos_id, invalid_zpos_id, no_formats_id,
                              fixed_opaque_overlay_id, blend_without_none_with_alpha_id, cursor_id, untyped_id,
                              invalid_alpha_id, second_primary_id, tie_below_primary_id, tie_above_primary_id,
                              inverted_zpos_id, inverted_alpha_id})
            mock_drm.add_plane(drm_device, id, target_crtc_mask);
        for (auto const id : {overlay_id, top_overlay_id})
            mock_drm.add_plane(drm_device, id, target_crtc_mask | other_crtc_mask);
        for (auto const id : {other_primary_id, other_crtc_overlay_id})
            mock_drm.add_plane(drm_device, id, other_crtc_mask);
        mock_drm.add_plane(drm_device, no_zpos_primary_id, no_zpos_crtc_mask);
        mock_drm.prepare(drm_device);

        for (auto const id : {primary_id, other_primary_id, second_primary_id})
            add_plane_properties(id, DRM_PLANE_TYPE_PRIMARY);
        for (auto const id : {overlay_id, underlay_id, top_overlay_id, other_crtc_overlay_id,
                              no_zpos_id, invalid_zpos_id, fixed_opaque_overlay_id,
                              blend_without_none_with_alpha_id, invalid_alpha_id,
                              tie_below_primary_id, tie_above_primary_id,
                              inverted_zpos_id, inverted_alpha_id})
            add_plane_properties(id, DRM_PLANE_TYPE_OVERLAY);
        add_plane_properties(cursor_id, DRM_PLANE_TYPE_CURSOR);
        add_plane_properties(no_zpos_primary_id, DRM_PLANE_TYPE_PRIMARY);
        add_property(no_formats_id, "type", DRM_PLANE_TYPE_OVERLAY);
        add_property(untyped_id, "IN_FORMATS", untyped_id);
        set_in_formats(untyped_id, InFormatsBlob{}.bytes());
        primary_zpos_id = add_range(primary_id, "zpos", 1, 1, 1, immutable_range_flags);
        for (auto const& [id, zpos] : {std::pair{overlay_id, 2}, {underlay_id, 0},
                                       {no_formats_id, 2}, {untyped_id, 2},
                                       {fixed_opaque_overlay_id, 2}, {blend_without_none_with_alpha_id, 2},
                                       {invalid_alpha_id, 2}, {other_primary_id, 0}, {second_primary_id, 0},
                                       {cursor_id, 5}, {tie_below_primary_id, 1}, {tie_above_primary_id, 1},
                                       {inverted_alpha_id, 2}})
            add_range(id, "zpos", zpos, zpos, zpos, immutable_range_flags);
        for (auto const& [id, zpos] : {std::pair{top_overlay_id, 3}, {other_crtc_overlay_id, 2}})
            add_range(id, "zpos", 0, 4, zpos);
        add_range(invalid_zpos_id, "zpos", 2, 2, 2, DRM_MODE_PROP_ENUM);
        add_range(inverted_zpos_id, "zpos", 4, 0, 2, immutable_range_flags);
        for (auto const id : {primary_id, other_primary_id, second_primary_id, overlay_id, other_crtc_overlay_id,
                              blend_without_none_with_alpha_id, tie_below_primary_id, tie_above_primary_id})
            add_range(id, "alpha", 0, 0xffff, 0xffff);
        add_range(invalid_alpha_id, "alpha", 0, 0xffff, 0xffff, DRM_MODE_PROP_ENUM);
        add_range(inverted_alpha_id, "alpha", 0xffff, 0, 0xffff);
        for (auto const id : {primary_id, other_primary_id, second_primary_id, overlay_id, top_overlay_id})
            add_enum(id, "pixel blend mode", {{6, "Pre-multiplied"}, {7, "None"}, {8, "Coverage"}});
        for (auto const id : {blend_without_none_with_alpha_id, invalid_alpha_id})
            add_enum(id, "pixel blend mode", {{6, "Pre-multiplied"}, {8, "Coverage"}});
        add_enum(overlay_id, "COLOR_ENCODING",
            {{10, "ITU-R BT.601 YCbCr"}, {11, "ITU-R BT.709 YCbCr"}, {12, "ITU-R BT.2020 YCbCr"}});
        add_enum(overlay_id, "COLOR_RANGE", {{13, "YCbCr limited range"}, {14, "YCbCr full range"}});

        // Discovery warns about the untyped plane; allow it in tests that assert on other log messages.
        EXPECT_CALL(*logger, log(_, _, _)).Times(AnyNumber());
    }

    void TearDown() override
    {
        mir::logging::set_logger(std::make_shared<mtd::NullLogger>());
        ASSERT_TRUE(logger.unique()) << "Test needs to destroy the MockLogger to verify assertions";
        logger.reset();
    }

    auto discover(uint32_t crtc_id = target_crtc_id) const -> mga::CrtcPlaneCapabilities
    {
        return mga::discover_plane_capabilities(drm_fd, crtc_id);
    }

    auto add_property(uint32_t plane, char const* name, uint64_t value = 0) -> uint32_t
    {
        auto const id = next_property_id++;
        mock_drm.add_property(drm_device, plane, DRM_MODE_OBJECT_PLANE, id, name, value);
        return id;
    }

    auto add_range(uint32_t plane, char const* name, uint64_t min, uint64_t max, uint64_t value,
                   uint32_t flags = DRM_MODE_PROP_RANGE) -> uint32_t
    {
        auto const id = add_property(plane, name, value);
        mock_drm.set_property_range(drm_device, id, flags, min, max);
        return id;
    }

    void add_enum(uint32_t plane, char const* name, EnumValues const& values)
    {
        mock_drm.set_property_enums(drm_device, add_property(plane, name), values);
    }

    void add_plane_properties(uint32_t plane, uint64_t type)
    {
        add_property(plane, "type", type);
        add_property(plane, "IN_FORMATS", plane);
        set_in_formats(plane, InFormatsBlob{}.bytes());
    }

    void set_in_formats(uint32_t plane, std::span<std::byte const> bytes)
    {
        mock_drm.add_property_blob(drm_device, plane, bytes);
    }

    std::shared_ptr<NiceMock<MockLogger>> logger{std::make_shared<NiceMock<MockLogger>>()};
    NiceMock<mtd::MockDRM> mock_drm;
    int const drm_fd{mock_drm.open(drm_device, 0)};
    uint32_t next_property_id{1000};
    uint32_t primary_zpos_id{};
};

struct MalformedInFormats
{
    std::string_view name;
    std::function<void(InFormatsBlob&)> modify;
    size_t length{sizeof(InFormatsBlob)};
};

class MalformedInFormatsTest : public PlaneCapabilitiesTest, public WithParamInterface<MalformedInFormats>
{
};
}

TEST_F(PlaneCapabilitiesTest, discovers_first_primary_and_routable_overlays_in_zpos_order)
{
    auto const caps = discover();

    EXPECT_THAT(caps.crtc_id, Eq(target_crtc_id));
    EXPECT_THAT(caps.primary.id, Eq(primary_id));
    EXPECT_THAT(caps.primary.zpos, Eq(mga::ZPos{1, 1, 1, true}));
    ASSERT_THAT(ids_of(caps.overlays), ElementsAre(
        underlay_id, tie_below_primary_id, tie_above_primary_id, overlay_id, fixed_opaque_overlay_id,
        blend_without_none_with_alpha_id, top_overlay_id))
        << "Rejected planes: missing zpos " << no_zpos_id << ", invalid zpos " << invalid_zpos_id
        << ", missing IN_FORMATS " << no_formats_id
        << ", cursor " << cursor_id << ", untyped " << untyped_id << ", invalid alpha " << invalid_alpha_id
        << ", later primary " << second_primary_id
        << ", inverted zpos " << inverted_zpos_id << ", inverted alpha " << inverted_alpha_id;

    auto const other_caps = discover(other_crtc_id);
    EXPECT_THAT(other_caps.crtc_id, Eq(other_crtc_id));
    EXPECT_THAT(other_caps.primary.id, Eq(other_primary_id));
    EXPECT_THAT(ids_of(other_caps.overlays), ElementsAre(overlay_id, other_crtc_overlay_id, top_overlay_id));
}

TEST_F(PlaneCapabilitiesTest, derives_stacking_relative_to_primary_zpos)
{
    auto const caps = discover();

    auto const stacking = caps.overlays | std::views::transform(
        [](auto const& overlay)
        {
            return std::tuple{overlay.id, overlay.can_be_above_primary, overlay.can_be_below_primary};
        }) | std::ranges::to<std::vector>();
    EXPECT_THAT(stacking, ElementsAre(
        std::tuple{underlay_id, false, true},
        std::tuple{tie_below_primary_id, false, true},
        std::tuple{tie_above_primary_id, true, false},
        std::tuple{overlay_id, true, false},
        std::tuple{fixed_opaque_overlay_id, true, false},
        std::tuple{blend_without_none_with_alpha_id, true, false},
        std::tuple{top_overlay_id, true, true}));

    EXPECT_THAT(find_overlay(caps, top_overlay_id).zpos, Eq(mga::ZPos{0, 4, 3, false}));
}

TEST_F(PlaneCapabilitiesTest, decodes_exact_formats_and_display_properties)
{
    auto const caps = discover();
    auto const& overlay = find_overlay(caps, overlay_id);
    EXPECT_THAT(overlay.formats, UnorderedElementsAre(
        mga::FormatModifier{DRM_FORMAT_NV12, I915_FORMAT_MOD_Y_TILED},
        mga::FormatModifier{DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR}));
    EXPECT_THAT(overlay.alpha_max, Optional(mga::PropertyValue{0xffff}));
    EXPECT_THAT(overlay.blend_none, Optional(mga::PropertyValue{7}));
    EXPECT_FALSE(overlay.fixed_opaque);
    EXPECT_THAT(overlay.bt709_limited, Optional(mga::Bt709Limited{11, 13}));

    auto const& blend_without_none = find_overlay(caps, blend_without_none_with_alpha_id);
    EXPECT_EQ(blend_without_none.blend_none, std::nullopt);
    EXPECT_THAT(blend_without_none.alpha_max, Optional(mga::PropertyValue{0xffff}));
    EXPECT_FALSE(blend_without_none.fixed_opaque);

    auto const& top = find_overlay(caps, top_overlay_id);
    EXPECT_EQ(top.alpha_max, std::nullopt);
    EXPECT_EQ(top.bt709_limited, std::nullopt);
}

TEST_F(PlaneCapabilitiesTest, nonzero_modifier_offset_selects_the_right_format)
{
    InFormatsBlob blob;
    blob.modifiers[1].offset = 1;
    blob.modifiers[1].formats = 1;
    set_in_formats(overlay_id, blob.bytes());

    auto const caps = discover();
    auto const& overlay = find_overlay(caps, overlay_id);
    EXPECT_THAT(overlay.formats, UnorderedElementsAre(
        mga::FormatModifier{DRM_FORMAT_NV12, I915_FORMAT_MOD_Y_TILED},
        mga::FormatModifier{DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR}));
}

TEST_P(MalformedInFormatsTest, overlay_with_malformed_or_empty_formats_is_not_usable)
{
    InFormatsBlob blob;
    GetParam().modify(blob);
    set_in_formats(overlay_id, blob.bytes().first(GetParam().length));

    EXPECT_THAT(ids_of(discover().overlays), Not(Contains(overlay_id)));
}

INSTANTIATE_TEST_SUITE_P(
    PlaneCapabilities,
    MalformedInFormatsTest,
    Values(
        MalformedInFormats{"ZeroFormatPairs", [](InFormatsBlob& blob) { blob.header.count_modifiers = 0; }},
        MalformedInFormats{"WrongVersion", [](InFormatsBlob& blob) { ++blob.header.version; }},
        MalformedInFormats{"FormatsPastEnd", [](InFormatsBlob& blob)
                           { blob.header.formats_offset = sizeof(InFormatsBlob) - sizeof(uint32_t); }},
        MalformedInFormats{"ModifiersPastEnd", [](InFormatsBlob&) {},
                           sizeof(InFormatsBlob) - sizeof(drm_format_modifier)},
        MalformedInFormats{"FormatBitPastEnd", [](InFormatsBlob& blob) { blob.modifiers[0].formats = 0b100; }}),
    [](TestParamInfo<MalformedInFormats> const& info) { return std::string{info.param.name}; });

TEST_F(PlaneCapabilitiesTest, primary_without_zpos_fails_discovery)
{
    EXPECT_THAT([this] { discover(no_zpos_crtc_id); },
        ThrowsMessage<std::runtime_error>(HasSubstr("No usable primary plane")));
}

TEST_F(PlaneCapabilitiesTest, primary_with_malformed_zpos_fails_discovery)
{
    mock_drm.set_property_range(drm_device, primary_zpos_id, DRM_MODE_PROP_ENUM, 1, 1);

    EXPECT_THAT([this] { discover(); }, ThrowsMessage<std::runtime_error>(HasSubstr("No usable primary plane")));
}

TEST_F(PlaneCapabilitiesTest, unusable_first_primary_fails_discovery_despite_later_usable_primary)
{
    set_in_formats(primary_id, {});

    EXPECT_THAT([this] { discover(); }, ThrowsMessage<std::runtime_error>(HasSubstr("No usable primary plane")));
}

TEST_F(PlaneCapabilitiesTest, fixed_opaque_planes_are_reported_once)
{
    EXPECT_CALL(*logger, log(_, AllOf(HasSubstr("fixed-opaque"), EndsWith(": 42, 51")), _)).Times(1);

    discover();
}

TEST_F(PlaneCapabilitiesTest, invalid_alpha_is_rejected)
{
    EXPECT_THAT(ids_of(discover().overlays), Not(Contains(invalid_alpha_id)));
}

TEST_F(PlaneCapabilitiesTest, inverted_range_bounds_are_rejected)
{
    auto const overlays = ids_of(discover().overlays);
    EXPECT_THAT(overlays, Not(Contains(inverted_zpos_id)));
    EXPECT_THAT(overlays, Not(Contains(inverted_alpha_id)));
}

TEST_F(PlaneCapabilitiesTest, primary_with_inverted_zpos_range_fails_discovery)
{
    mock_drm.set_property_range(drm_device, primary_zpos_id, DRM_MODE_PROP_RANGE, 4, 0);

    EXPECT_THAT([this] { discover(); }, ThrowsMessage<std::runtime_error>(HasSubstr("No usable primary plane")));
}

TEST_F(PlaneCapabilitiesTest, rebuild_publishes_new_snapshot_and_leaves_old_one_unchanged)
{
    mga::PlaneCapabilityState state;
    state.rebuild(drm_fd, target_crtc_id);
    auto const first = state.current();
    ASSERT_NE(first, nullptr);

    set_in_formats(overlay_id, {});
    state.rebuild(drm_fd, target_crtc_id);
    auto const second = state.current();

    ASSERT_NE(second, nullptr);
    EXPECT_THAT(ids_of(second->overlays), Not(Contains(overlay_id)));
    EXPECT_THAT(ids_of(first->overlays), Contains(overlay_id));
}

TEST_F(PlaneCapabilitiesTest, failed_rebuild_logs_and_leaves_no_capabilities)
{
    mga::PlaneCapabilityState state;
    state.rebuild(drm_fd, target_crtc_id);
    ASSERT_NE(state.current(), nullptr);

    set_in_formats(primary_id, {});
    EXPECT_CALL(*logger, log(_, HasSubstr("Failed to discover plane capabilities"), _)).Times(1);
    EXPECT_NO_THROW(state.rebuild(drm_fd, target_crtc_id));
    EXPECT_EQ(state.current(), nullptr);
}

TEST_F(PlaneCapabilitiesTest, rebuild_clears_capabilities_when_discovery_throws_non_runtime_error)
{
    mga::PlaneCapabilityState state;
    state.rebuild(drm_fd, target_crtc_id);
    ASSERT_NE(state.current(), nullptr);

    EXPECT_CALL(mock_drm, drmModeGetPropertyBlob(drm_fd, _)).WillOnce(Throw(std::bad_alloc{}));

    EXPECT_THROW(state.rebuild(drm_fd, target_crtc_id), std::bad_alloc);
    EXPECT_EQ(state.current(), nullptr);
}

TEST_F(PlaneCapabilitiesTest, unreadable_in_formats_blob_fails_discovery)
{
    EXPECT_CALL(mock_drm, drmModeGetPropertyBlob(drm_fd, _)).Times(AnyNumber());
    EXPECT_CALL(mock_drm, drmModeGetPropertyBlob(drm_fd, overlay_id)).WillOnce(Return(nullptr));

    EXPECT_THAT([this] { discover(); }, ThrowsMessage<std::system_error>(HasSubstr("IN_FORMATS")));
}

