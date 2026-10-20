/*
 * Copyright © Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "plane_capabilities.h"

#include <mir/graphics/kms/drm_mode_resources.h>
#include <mir/log.h>

#include <boost/throw_exception.hpp>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mga = mir::graphics::atomic;
namespace mgk = mir::graphics::kms;

namespace
{
using BlobUPtr = std::unique_ptr<drmModePropertyBlobRes, void (*)(drmModePropertyBlobPtr)>;

auto is_range(drmModePropertyRes const& prop) -> bool
{
    return prop.count_values >= 2 &&
        (prop.flags & DRM_MODE_PROP_RANGE) != 0 &&
        prop.values[0] <= prop.values[1];
}

auto enum_value(drmModePropertyRes const& prop, std::string_view name) -> std::optional<mga::PropertyValue>
{
    std::span const enums{prop.enums, static_cast<size_t>(prop.count_enums)};
    auto const it = std::ranges::find(
        enums, name, [](drm_mode_property_enum const& entry) { return std::string_view{entry.name}; });
    if (it == enums.end())
        return std::nullopt;
    return it->value;
}

auto crtc_index_of(int drm_fd, uint32_t crtc_id) -> uint32_t
{
    mgk::DRMModeResources const resources{drm_fd};
    uint32_t index{0};
    for (auto const& crtc : resources.crtcs())
    {
        if (crtc && crtc->crtc_id == crtc_id)
            return index;
        ++index;
    }
    BOOST_THROW_EXCEPTION(std::runtime_error{"CRTC " + std::to_string(crtc_id) + " not found"});
}

auto decode_zpos(drmModePropertyRes const& prop, uint64_t current) -> std::optional<mga::ZPos>
{
    if (!is_range(prop))
        return std::nullopt;

    return mga::ZPos{
        .min = static_cast<int64_t>(prop.values[0]),
        .max = static_cast<int64_t>(prop.values[1]),
        .current = static_cast<int64_t>(current),
        .immutable = (prop.flags & DRM_MODE_PROP_IMMUTABLE) != 0,
    };
}

auto zpos_order_key(int64_t zpos, uint32_t id) -> std::pair<int64_t, uint32_t>
{
    return {zpos, id};
}

auto decode_bt709_limited(mgk::ObjectProperties const& props) -> std::optional<mga::Bt709Limited>
{
    auto const encoding_prop = props.metadata("COLOR_ENCODING");
    auto const range_prop = props.metadata("COLOR_RANGE");
    if (!encoding_prop || !range_prop)
        return std::nullopt;

    auto const encoding = enum_value(*encoding_prop, "ITU-R BT.709 YCbCr");
    auto const range = enum_value(*range_prop, "YCbCr limited range");
    if (!encoding || !range)
        return std::nullopt;
    return mga::Bt709Limited{*encoding, *range};
}

template<typename T>
auto read_at(std::span<std::byte const> data, size_t offset) -> T
{
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof value);
    return value;
}

auto within(size_t size, uint32_t offset, uint32_t count, size_t element_size) -> bool
{
    return uint64_t{offset} + uint64_t{count} * element_size <= size;
}

auto parse_in_formats(std::span<std::byte const> data) -> std::optional<std::vector<mga::FormatModifier>>
{
    if (data.size() < sizeof(drm_format_modifier_blob))
        return std::nullopt;

    auto const header = read_at<drm_format_modifier_blob>(data, 0);
    if (header.version != FORMAT_BLOB_CURRENT ||
        !within(data.size(), header.formats_offset, header.count_formats, sizeof(uint32_t)) ||
        !within(data.size(), header.modifiers_offset, header.count_modifiers, sizeof(drm_format_modifier)))
        return std::nullopt;

    std::vector<mga::FormatModifier> pairs;
    for (uint32_t i = 0; i != header.count_modifiers; ++i)
    {
        auto const entry = read_at<drm_format_modifier>(
            data, header.modifiers_offset + i * sizeof(drm_format_modifier));
        for (uint64_t bit = 0; bit < 64; ++bit)
        {
            if ((entry.formats & (uint64_t{1} << bit)) == 0)
                continue;

            auto const index = uint64_t{entry.offset} + bit;
            if (index >= header.count_formats)
                return std::nullopt;
            pairs.push_back({
                read_at<uint32_t>(data, header.formats_offset + index * sizeof(uint32_t)), entry.modifier});
        }
    }

    if (pairs.empty())
        return std::nullopt;
    return pairs;
}

auto decode_formats(int drm_fd, mgk::ObjectProperties const& props) -> std::optional<std::vector<mga::FormatModifier>>
{
    if (!props.has_property("IN_FORMATS"))
        return std::nullopt;

    auto const blob_id = props["IN_FORMATS"];
    if (blob_id == 0 || blob_id > std::numeric_limits<uint32_t>::max())
        return std::nullopt;

    errno = 0;
    BlobUPtr const blob{drmModeGetPropertyBlob(drm_fd, static_cast<uint32_t>(blob_id)), &drmModeFreePropertyBlob};
    if (!blob)
    {
        // drmModeGetPropertyBlob either sets errno, or has failed in malloc()
        auto const error = errno ? errno : ENOMEM;
        BOOST_THROW_EXCEPTION((std::system_error{
            error,
            std::system_category(),
            "Failed to read IN_FORMATS blob of plane " + std::to_string(props.parent_id())}));
    }

    return parse_in_formats({static_cast<std::byte const*>(blob->data), blob->length});
}

auto decode_plane(int drm_fd, mgk::ObjectProperties const& props) -> std::optional<mga::PlaneCapabilities>
{
    auto const zpos_prop = props.metadata("zpos");
    if (!zpos_prop)
        return std::nullopt;
    auto const zpos = decode_zpos(*zpos_prop, props["zpos"]);
    if (!zpos)
        return std::nullopt;

    auto formats = decode_formats(drm_fd, props);
    if (!formats)
        return std::nullopt;

    std::optional<mga::PropertyValue> alpha_max;
    if (auto const alpha = props.metadata("alpha"))
    {
        if (!is_range(*alpha))
            return std::nullopt;
        alpha_max = alpha->values[1];
    }

    auto const blend_prop = props.metadata("pixel blend mode");
    auto const blend_none = blend_prop ? enum_value(*blend_prop, "None") : std::nullopt;

    auto const fixed_opaque = !alpha_max && !blend_prop;

    return mga::PlaneCapabilities{
        .id = props.parent_id(),
        .zpos = *zpos,
        .formats = std::move(*formats),
        .alpha_max = alpha_max,
        .blend_none = blend_none,
        .fixed_opaque = fixed_opaque,
        .bt709_limited = decode_bt709_limited(props),
        .can_be_above_primary = false,
        .can_be_below_primary = false,
    };
}

}

auto mga::discover_plane_capabilities(int drm_fd, uint32_t crtc_id) -> CrtcPlaneCapabilities
{
    auto const crtc_index = crtc_index_of(drm_fd, crtc_id);
    auto const crtc_mask = uint32_t{1} << crtc_index;

    std::optional<PlaneCapabilities> primary;
    bool primary_seen{false};
    std::vector<PlaneCapabilities> overlays;

    mgk::PlaneResources const plane_resources{drm_fd};
    for (auto const& plane : plane_resources.planes())
    {
        if ((plane->possible_crtcs & crtc_mask) == 0)
            continue;

        mgk::ObjectProperties const props{drm_fd, plane};
        auto const type = props.has_property("type") ? props["type"] : std::numeric_limits<uint64_t>::max();
        switch (type)
        {
        case DRM_PLANE_TYPE_PRIMARY:
            if (!primary_seen)
            {
                primary_seen = true;
                primary = decode_plane(drm_fd, props);
            }
            break;
        case DRM_PLANE_TYPE_OVERLAY:
            if (auto overlay = decode_plane(drm_fd, props))
                overlays.push_back(std::move(*overlay));
            break;
        case DRM_PLANE_TYPE_CURSOR:
            break;
        default:
            mir::log_warning("Ignoring plane %u with missing or unknown type", props.parent_id());
            break;
        }
    }

    if (!primary)
        BOOST_THROW_EXCEPTION(std::runtime_error{"No usable primary plane for CRTC " + std::to_string(crtc_id)});

    for (auto& overlay : overlays)
    {
        // drm_atomic_normalize_zpos() orders planes by (zpos, plane id), with the
        // higher id on top when zpos ties, so compare both bounds the same way.
        overlay.can_be_above_primary =
            zpos_order_key(overlay.zpos.max, overlay.id) >
            zpos_order_key(primary->zpos.current, primary->id);
        overlay.can_be_below_primary =
            zpos_order_key(overlay.zpos.min, overlay.id) <
            zpos_order_key(primary->zpos.current, primary->id);
    }
    std::ranges::sort(
        overlays,
        {},
        [](PlaneCapabilities const& overlay) { return zpos_order_key(overlay.zpos.current, overlay.id); });

    std::string fixed_opaque_ids = primary->fixed_opaque ? std::to_string(primary->id) : "";
    for (auto const& overlay : overlays)
    {
        if (overlay.fixed_opaque)
            fixed_opaque_ids += (fixed_opaque_ids.empty() ? "" : ", ") + std::to_string(overlay.id);
    }
    if (!fixed_opaque_ids.empty())
    {
        mir::log_info(
            "Planes treated as fixed-opaque on CRTC %u (no alpha or blend control): %s",
            crtc_id,
            fixed_opaque_ids.c_str());
    }

    return CrtcPlaneCapabilities{
        .crtc_id = crtc_id,
        .primary = std::move(*primary),
        .overlays = std::move(overlays),
    };
}

void mga::PlaneCapabilityState::rebuild(int drm_fd, uint32_t crtc_id)
{
    std::shared_ptr<CrtcPlaneCapabilities const> caps;
    try
    {
        caps = std::make_shared<CrtcPlaneCapabilities const>(discover_plane_capabilities(drm_fd, crtc_id));
    }
    catch (std::runtime_error const& e)
    {
        mir::log_warning("Failed to discover plane capabilities for CRTC %u: %s", crtc_id, e.what());
    }
    catch (...)
    {
        *state.lock() = nullptr;
        throw;
    }
    *state.lock() = std::move(caps);
}

auto mga::PlaneCapabilityState::current() const -> std::shared_ptr<CrtcPlaneCapabilities const>
{
    return *state.lock();
}
