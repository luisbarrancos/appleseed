
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
// Copyright (c) 2014-2017 Francois Beaune, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "texturestore.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"
#include "renderer/modeling/scene/assembly.h"
#include "renderer/modeling/scene/scene.h"
#include "renderer/modeling/texture/texture.h"
#include "renderer/utility/paramarray.h"

// appleseed.foundation headers.
#include "foundation/image/color.h"
#include "foundation/image/colorspace.h"
#include "foundation/image/tile.h"
#include "foundation/utility/api/apistring.h"
#include "foundation/utility/containers/dictionary.h"
#include "foundation/utility/foreach.h"
#include "foundation/utility/memory.h"
#include "foundation/utility/statistics.h"
#include "foundation/utility/string.h"

// Standard headers.
#include <algorithm>
#include <string>

using namespace foundation;
using namespace std;


// So it seems here we need to apply the EOTF, color space transformation and chromatic adaptation (if needed), the
// input device transforms.
// More below

namespace renderer
{

//
// TextureStore class implementation.
//

TextureStore::TextureStore(
    const Scene&        scene,
    const ParamArray&   params)
  : m_tile_swapper(scene, params)
  , m_tile_cache(m_tile_key_hasher, m_tile_swapper)
{
}

StatisticsVector TextureStore::get_statistics() const
{
    Statistics stats = make_single_stage_cache_stats(m_tile_cache);
    stats.insert_size("peak size", m_tile_swapper.get_peak_memory_size());

    return StatisticsVector::make("texture store statistics", stats);
}

Dictionary TextureStore::get_params_metadata()
{
    Dictionary metadata;

    const size_t DefaultTextureStoreSizeMB = 1024;
    metadata.dictionaries().insert(
        "max_size",
        Dictionary()
            .insert("type", "int")
            .insert("default", DefaultTextureStoreSizeMB * 1024 * 1024)
            .insert("label", "Texture Cache Size")
            .insert("help", "Texture cache size in bytes"));

    return metadata;
}


//
// TextureStore::TileSwapper class implementation.
//

// This is going to be more complex
//
// Ingested material exists in a encoding, color space, and white point definition
//
// The process is:
//
//  1. apply EOTF to get ingested material into linear gamma
//
//  2. convert from ingested material color space to rendering/working space
//     (i.e: a sRGB/Rec709 JPG texture, converted into Rec.2020 rendering/working space)
//
//      if the rendering/working space has a different white point: ACES 2065-1 AP0, or ACEScg - both with D60,
//      or DCI-P3 with the whitepoint outside the daylight curve, then you have 2 choices
//
//          a) use a precomputed chromatically adapted RGB to RGB matrix (see OSL chromatic adaptation, colorimetry)
//
//          b) do it on the fly for arbitrarily set whitepoints by converting the ingested material to XYZ intermediary space
//             and chromatically adapt for the rendering/working space whitepoint. Example, linear gamma sRGB/Rec709 D65, converted to
//             XYZ, and XYZ adapted to D60 whitepoint with a von Kries transform (and one of several chromatic adaptation transforms)
//
// Example 1:
//
//  Working space was set as Rec.2020, D65 whitepoint. Textures are sRGB JPEGs, D65.
//
//      1) apply sRTB EOTF
//      2) convert from sRGB/Rec709 chromaticies to Rec.2020 working space. The white point is the same, no CAT needed.
//
// Example 2:
//
//  Working space set to ACEScg AP1 D60 white point, textures are sRGB JPEGs, D65
//
//      1) apply sRGB EOTF
//      2) convert from sRGB/Rec709 chromaticities to ACEScg working space, but whitepoint differ, so:
//
//          a) use precomputed RGB->RGB matrices, see OSL colorimetry, and chromatic adaptation
//
//          b)  * convert from sRGB/Rec709 primaries to CIE XYZ
//              * apply von Kries transform to change from input D65 whitepoint, to working space D60 whitepoint
//              * convert from CIE XYZ to ACEScg primaries
//
// Example 3:
//
//  Working space set to ACES 2065-1 AP0, white point is D60. No ingested textures, but procedural sky models using
//  spectral data for instance.
//
//      1) no EOTF to apply, data is procedurally generated, linear, but color space is a consideration
//      2) convert from SPD to XYZ, but CMFs are with standard observer and D65 illuminant, so if the working/render space
//         differs, one would need to chromatically adapt the XYZ to the working/render space white point, from D65 to D60 here.
//         NOTE: alternatively, it would be interesting to see if we could use the CMFs with D60 illuminant.
//
// Example 4:
//
//  Working space set to Rec.709, white point D65, ingested textures are log encoded TIFFs, i.e. S-Log3
//
//      1) apply S-Log3 to linear EOTF
//      2) same chromaticities, and white point, no further change required
//

namespace
{
    // Convert a tile from the sRGB color space to the linear RGB color space.
    void convert_tile_srgb_to_linear_rgb(Tile& tile)
    {
        const size_t pixel_count = tile.get_pixel_count();
        const size_t channel_count = tile.get_channel_count();

        assert(channel_count == 3 || channel_count == 4);

        if (channel_count == 3)
        {
            for (size_t i = 0; i < pixel_count; ++i)
            {
                Color3f color;
                tile.get_pixel(i, color);
                tile.set_pixel(i, srgb_to_linear_rgb(color));
            }
        }
        else
        {
            for (size_t i = 0; i < pixel_count; ++i)
            {
                Color4f color;
                tile.get_pixel(i, color);
                color.rgb() = srgb_to_linear_rgb(color.rgb());
                tile.set_pixel(i, color);
            }
        }
    }

    // Convert a tile from the CIE XYZ color space to the linear RGB color space.

    // NOTE: Same as above: chromatic adaptation.
    // The advantage of precomputed matrices is saving computation time.
    // The disadvantage, we cannot use any arbitrary illuminant unless we precomputed all possible matrices, which is
    // a considerable number of combinations, but in practical terms, it's unlikely the working/rendering space will be different
    // than D60, D65, DCI-P3. So these combinations at least have to be done.
    //
    void convert_tile_ciexyz_to_linear_rgb(Tile& tile)
    {
        const size_t pixel_count = tile.get_pixel_count();
        const size_t channel_count = tile.get_channel_count();

        assert(channel_count == 3 || channel_count == 4);

        if (channel_count == 3)
        {
            for (size_t i = 0; i < pixel_count; ++i)
            {
                Color3f color;
                tile.get_pixel(i, color);
                tile.set_pixel(i, ciexyz_to_linear_rgb(color));
            }
        }
        else
        {
            for (size_t i = 0; i < pixel_count; ++i)
            {
                Color4f color;
                tile.get_pixel(i, color);
                color.rgb() = ciexyz_to_linear_rgb(color.rgb());
                tile.set_pixel(i, color);
            }
        }
    }
}

TextureStore::TileSwapper::TileSwapper(
    const Scene&        scene,
    const ParamArray&   params)
  : m_scene(scene)
  , m_params(params)
  , m_memory_size(0)
  , m_peak_memory_size(0)
{
    gather_assemblies(scene.assemblies());
}

void TextureStore::TileSwapper::load(const TileKey& key, TileRecord& record)
{
    // Fetch the texture container.
    const TextureContainer& textures =
        key.m_assembly_uid == UniqueID(~0)
            ? m_scene.textures()
            : m_assemblies[key.m_assembly_uid]->textures();

    // Fetch the texture.
    Texture* texture = textures.get_by_uid(key.m_texture_uid);

    if (m_params.m_track_tile_loading)
    {
        RENDERER_LOG_DEBUG(
            "loading tile (" FMT_SIZE_T ", " FMT_SIZE_T ") "
            "from texture \"%s\"...",
            key.get_tile_x(),
            key.get_tile_y(),
            texture->get_path().c_str());
    }

    // Load the tile.
    record.m_tile = texture->load_tile(key.get_tile_x(), key.get_tile_y());
    record.m_owners = 0;

    // Same as above

    // Convert the tile to the linear RGB color space.
    switch (texture->get_color_space())
    {
      case ColorSpaceLinearRGB:
        break;

      case ColorSpaceSRGB:
        convert_tile_srgb_to_linear_rgb(*record.m_tile);
        break;

      case ColorSpaceCIEXYZ:
        convert_tile_ciexyz_to_linear_rgb(*record.m_tile);
        break;

      assert_otherwise;
    }

    // Track the amount of memory used by the tile cache.
    m_memory_size += record.m_tile->get_memory_size();
    m_peak_memory_size = max(m_peak_memory_size, m_memory_size);

    if (m_params.m_track_store_size)
    {
        if (m_memory_size > m_params.m_memory_limit)
        {
            RENDERER_LOG_DEBUG(
                "texture store size is %s, exceeding capacity %s by %s",
                pretty_size(m_memory_size).c_str(),
                pretty_size(m_params.m_memory_limit).c_str(),
                pretty_size(m_memory_size - m_params.m_memory_limit).c_str());
        }
        else
        {
            RENDERER_LOG_DEBUG(
                "texture store size is %s, below capacity %s by %s",
                pretty_size(m_memory_size).c_str(),
                pretty_size(m_params.m_memory_limit).c_str(),
                pretty_size(m_params.m_memory_limit - m_memory_size).c_str());
        }
    }
}

bool TextureStore::TileSwapper::unload(const TileKey& key, TileRecord& record)
{
    // Cannot unload tiles that are still in use.
    if (atomic_read(&record.m_owners) > 0)
        return false;

    // Track the amount of memory used by the tile cache.
    const size_t tile_memory_size = record.m_tile->get_memory_size();
    assert(m_memory_size >= tile_memory_size);
    m_memory_size -= tile_memory_size;

    // Fetch the texture container.
    const TextureContainer& textures =
        key.m_assembly_uid == UniqueID(~0)
            ? m_scene.textures()
            : m_assemblies[key.m_assembly_uid]->textures();

    // Fetch the texture.
    Texture* texture = textures.get_by_uid(key.m_texture_uid);

    if (m_params.m_track_tile_unloading)
    {
        RENDERER_LOG_DEBUG(
            "unloading tile (" FMT_SIZE_T ", " FMT_SIZE_T ") "
            "from texture \"%s\"...",
            key.get_tile_x(),
            key.get_tile_y(),
            texture->get_path().c_str());
    }

    // Unload the tile.
    texture->unload_tile(key.get_tile_x(), key.get_tile_y(), record.m_tile);

    // Successfully unloaded the tile.
    return true;
}

void TextureStore::TileSwapper::gather_assemblies(const AssemblyContainer& assemblies)
{
    for (const_each<AssemblyContainer> i = assemblies; i; ++i)
    {
        m_assemblies[i->get_uid()] = &*i;
        gather_assemblies(i->assemblies());
    }
}


//
// TextureStore::TileSwapper::Parameters class implementation.
//

TextureStore::TileSwapper::Parameters::Parameters(const ParamArray& params)
  : m_memory_limit(params.get_optional<size_t>("max_size", 256 * 1024 * 1024))
  , m_track_tile_loading(params.get_optional<bool>("track_tile_loading", false))
  , m_track_tile_unloading(params.get_optional<bool>("track_tile_unloading", false))
  , m_track_store_size(params.get_optional<bool>("track_store_size", false))
{
    assert(m_memory_limit > 0);
}

}   // namespace renderer
