
//
// This source file is part of appleseed.
// Visit https://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
// Copyright (c) 2014-2018 Francois Beaune, The appleseedhq Organization
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

#pragma once

// appleseed.renderer headers.
#include "renderer/global/globaltypes.h"
#include "renderer/modeling/bsdf/ibsdffactory.h"
#include "renderer/modeling/input/inputarray.h"

// appleseed.foundation headers.
#include "foundation/memory/autoreleaseptr.h"
#include "foundation/platform/compiler.h"

// appleseed.main headers.
#include "main/dllsymbol.h"

// Forward declarations.
namespace foundation    { class Dictionary; }
namespace foundation    { class DictionaryArray; }
namespace renderer      { class BSDF; }
namespace renderer      { class ParamArray; }

namespace renderer
{

//
// Specular BTDF input values.
//

APPLESEED_DECLARE_INPUT_VALUES(SpecularBTDFInputValues)
{
    Spectrum    m_reflectance;                  // specular reflectance
    float       m_reflectance_multiplier;       // specular reflectance multiplier
    Spectrum    m_transmittance;                // specular transmittance
    float       m_transmittance_multiplier;     // specular transmittance multiplier
    float       m_fresnel_multiplier;           // Fresnel multiplier
    float       m_ior;                          // index of refraction inside the medium
    float       m_volume_density;               // volume density
    float       m_volume_scale;                 // volume scaling factor

    struct Precomputed
    {
        float   m_eta;
    };

    Precomputed m_precomputed;
};


//
// Specular BTDF factory.
//

class APPLESEED_DLLSYMBOL SpecularBTDFFactory
  : public IBSDFFactory
{
  public:
    // Delete this instance.
    void release() override;

    // Return a string identifying this BSDF model.
    const char* get_model() const override;

    // Return metadata for this BSDF model.
    foundation::Dictionary get_model_metadata() const override;

    // Return metadata for the inputs of this BSDF model.
    foundation::DictionaryArray get_input_metadata() const override;

    // Create a new BSDF instance.
    foundation::auto_release_ptr<BSDF> create(
        const char*         name,
        const ParamArray&   params) const override;
};

}   // namespace renderer
