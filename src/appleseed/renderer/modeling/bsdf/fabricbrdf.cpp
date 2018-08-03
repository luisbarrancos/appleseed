
//
// This source file is part of appleseed.
// Visit https://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2018 Luis B. Barrancos, The appleseedhq Organization
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
#include "fabricbrdf.h"

// appleseed.renderer headers.
#include "renderer/kernel/lighting/scatteringmode.h"
#include "renderer/kernel/shading/directshadingcomponents.h"
#include "renderer/kernel/shading/shadingpoint.h"
#include "renderer/modeling/bsdf/bsdf.h"
#include "renderer/modeling/bsdf/bsdfsample.h"
#include "renderer/modeling/bsdf/bsdfwrapper.h"

// appleseed.foundation headers.
#include "foundation/math/basis.h"
#include "foundation/math/sampling/mappings.h"
#include "foundation/math/scalar.h"
#include "foundation/math/vector.h"
#include "foundation/utility/api/specializedapiarrays.h"
#include "foundation/utility/containers/dictionary.h"

// Standard headers.
#include <algorithm>
#include <cmath>

// Forward declarations.
namespace foundation    { class IAbortSwitch; }
namespace renderer      { class Assembly; }
namespace renderer      { class Project; }

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // DWA fabric BRDF.
    //
    // Reference:
    //
    //   Physically based shading at DreamWorks Animation: DWA Fabric Modelling
    //   http://blog.selfshadow.com/publications/s2017-shading-course/dreamworks/s2017_pbs_dreamworks_notes.pdf
    //

    const char* Model = "fabric_brdf";

    class FabricBRDFImpl
      : public BSDF
    {
      public:
        FabricBRDFImpl(
            const char*                 name,
            const ParamArray&           params)
          : BSDF(name, Reflective, ScatteringMode::Glossy, params)
        {
            m_inputs.declare("reflectance", InputFormatSpectralReflectance);
            m_inputs.declare("reflectance_multiplier", InputFormatFloat, "1.0");
            m_inputs.declare("roughness" , InputFormatFloat, "0.1");
            m_inputs.declare("energy_compensation", InputFormatFloat, "0.1");
        }

        void release() override
        {
            delete this;
        }

        const char* get_model() const override
        {
            return Model;
        }

        size_t compute_input_data_size() const override
        {
            return sizeof(InputValues);
        }

        void prepare_inputs(
            Arena&                      arena,
            const ShadingPoint&         shading_point,
            void*                       data) const override
        {
            InputValues* values = static_cast<InputValues*>(data);

            // Apply multipliers to input values.
            values->m_reflectance *= values->m_reflectance_multiplier;

            values->m_roughness = max(values->m_roughness, shading_point.get_ray().m_max_roughness);

            new (&values->m_precomputed) InputValues::Precomputed();

            values->m_precomputed.m_exponent = compute_exponent(values->m_roughness);
            values->m_precomputed.m_energy_compensation_factor = 0.0f; // WIP
        }

        void sample(
            SamplingContext&            sampling_context,
            const void*                 data,
            const bool                  adjoint,
            const bool                  cosine_mult,
            const int                   modes,
            BSDFSample&                 sample) const override
        {
            if (!ScatteringMode::has_glossy(modes))
                return;

            sample.m_max_roughness = 1.0f;

            // Set the scattering mode
            sample.m_mode = ScatteringMode::Glossy;

            // Compute the incoming direction.
            sampling_context.split_in_place(2, 1);
            const Vector2f s = sampling_context.next2<Vector2f>();            
            const InputValues* values = static_cast<const InputValues*>(data);

            // Sample phi
            const float phi = s[0] * Pi<float>();
            float cos_phi = cos(phi);
            float sin_phi = sin(phi);

            // Sample theta
            const float sin_theta = 1.0f - pow(s[1], 1.0f / (values->m_precomputed.m_exponent + 1.0f));
            const float cos_theta = sqrt(max(0.0f, 1.0f - square(sin_theta)));

            // compute the halfway vector in world space
            Vector3f h =sample.m_shading_basis.transform_to_parent(
                    Vector3f::make_unit_vector(
                        cos_theta, sin_theta, cos_phi, sin_phi));

            Vector3f incoming = force_above_surface(
                        reflect(sample.m_outgoing.get_value(), h),
                        sample.m_geometric_normal);

            // Compute dot products
            const Vector3f& shading_normal = sample.m_shading_basis.get_normal();

            const float cos_hn = abs(dot(h, shading_normal));
            const float cos_ho = min(1.0f, abs(dot(h, sample.m_outgoing.get_value())));
            const float sin_hn = sqrt(max(0.0f, 1.0f - square(cos_hn)));

            // Evaluate the BRDF
            const float num = pow(1.0f - sin_hn, values->m_precomputed.m_exponent);
            const float den = cos_ho * FourPi<float>();
            // And PDF
            const float pdf = (values->m_precomputed.m_exponent + 1.0f) * num / den;
            assert(pdf >= 0.0f);

            sample.m_incoming = Dual3f(incoming);
            sample.m_value.m_glossy = values->m_reflectance * num;
            sample.m_value.m_beauty = sample.m_value.m_glossy;
            sample.m_probability = pdf;

            sample.compute_reflected_differentials();
        }

        float evaluate(
            const void*                 data,
            const bool                  adjoint,
            const bool                  cosine_mult,
            const Vector3f&             geometric_normal,
            const Basis3f&              shading_basis,
            const Vector3f&             outgoing,
            const Vector3f&             incoming,
            const int                   modes,
            DirectShadingComponents&    value) const override
        {
            if (!ScatteringMode::has_glossy(modes))
                return 0.0f;

            // Compute the halfway vector in world space
            const Vector3f h = normalize(incoming + outgoing);

            // Compute the BRDF value.
            const InputValues* values = static_cast<const InputValues*>(data);

            // Compute dot products
            const Vector3f& n = shading_basis.get_normal();

            const float cos_on = abs(dot(outgoing, n));
            const float cos_ho = abs(dot(outgoing, h));
            const float cos_hn = abs(dot(h, n));
            const float sin_hn = sqrt(max(0.0f, 1.0f - square(cos_hn)));

            // Evaluate the fabric BRDF
            const float num = pow(1.0f - sin_hn, values->m_precomputed.m_exponent);
            const float den = cos_ho * FourPi<float>();
            // And PDF
            const float pdf = (values->m_precomputed.m_exponent + 1.0f) * num / den;
            assert(pdf >= 0.0f);

            value.m_glossy = values->m_reflectance * num;
            value.m_beauty = value.m_glossy;

            // Return the probability density of the sampled direction.
            return pdf;
        }

        float evaluate_pdf(
            const void*                 data,
            const bool                  adjoint,
            const Vector3f&             geometric_normal,
            const Basis3f&              shading_basis,
            const Vector3f&             outgoing,
            const Vector3f&             incoming,
            const int                   modes) const override
        {
            if (!ScatteringMode::has_glossy(modes))
                return 0.0f;

            const InputValues* values = static_cast<const InputValues*>(data);

            // get reflectance values
            //if (values->m_reflectance < epsilon)
            //    return 0.0f;

            // Compute the halfway vector in world space
            const Vector3f h = normalize(incoming + outgoing);

            // Evaluate the PDF for the halfway vector
            const float cos_ho = abs(dot(outgoing, h));
            const float cos_hn = abs(dot(h, shading_basis.get_normal()));
            const float sin_hn = sqrt(max(0.0f, 1.0f - square(cos_hn)));

            const float num = pow(1.0f - sin_hn, values->m_precomputed.m_exponent);
            const float den = cos_ho * FourPi<float>();
            const float pdf = (values->m_precomputed.m_exponent + 1.0f) * num / den;
            assert(pdf >= 0.0f);

            // Return the probability density of the sampled direction.
            return pdf;
        }

      private:
        typedef FabricBRDFInputValues InputValues;

        static float compute_exponent(const float roughness)
        {
            // Build exponent from roughness, Eq.7.
            const float inv_m = 1.0f - roughness;
            const float sqr_m = square(inv_m);

            return fast_ceil(1.0f + 29 * sqr_m);
        }

        static void fabric_brdf(
            const Spectrum&             reflectance,
            const float                 exponent,
            const Vector3f&             wi,
            const Vector3f&             wo,
            const Vector3f&             m,
            Spectrum&                   value)
        {
            ; // move brdf here (TODO)
        }

        static float fabric_pdf(
            const float                 exponent,
            const Vector3f&             wo,
            const Vector3f&             m)
        {
            // WIP, move PDF here
            /*
            if (exponent == 0.0f)
                return 0.0f;

            const float cos_wom = dot(wo, m);
            if (cos_wom == 0.0f)
                return 0.0f;

            const float jacobian = 1.0f / (4.0f * abs(cos_wom));

            const float normalization = (exponent + 1.0f) *
                    RcpTwoPi<float>();

            return 1.0f;
            */
        }

        // Add EC term (WIP)
    };

    typedef BSDFWrapper<FabricBRDFImpl> FabricBRDF;
}


//
// FabricBRDFFactory class implementation.
//

void FabricBRDFFactory::release()
{
    delete this;
}

const char* FabricBRDFFactory::get_model() const
{
    return Model;
}

Dictionary FabricBRDFFactory::get_model_metadata() const
{
    return
        Dictionary()
            .insert("name", Model)
            .insert("label", "Fabric BRDF");
}

DictionaryArray FabricBRDFFactory::get_input_metadata() const
{
    DictionaryArray metadata;

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance")
            .insert("label", "Reflectance")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary()
                    .insert("color", "Colors")
                    .insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.5"));

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance_multiplier")
            .insert("label", "Reflectance Multiplier")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
            .insert("name", "roughness")
            .insert("label", "Roughness")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.1"));

    metadata.push_back(
        Dictionary()
            .insert("name", "energy_compensation")
            .insert("label", "Energy Compensation")
            .insert("type", "numeric")
            .insert("min",
                Dictionary()
                    .insert("value", "0.0")
                    .insert("type", "hard"))
            .insert("max",
                Dictionary()
                    .insert("value", "1.0")
                    .insert("type", "hard"))
            .insert("use", "optional")
            .insert("default", "0.0"));

    return metadata;
}

auto_release_ptr<BSDF> FabricBRDFFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return auto_release_ptr<BSDF>(new FabricBRDF(name, params));
}

}   // namespace renderer
