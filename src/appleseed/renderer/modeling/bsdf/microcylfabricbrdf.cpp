
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
#include "microcylfabricbrdf.h"

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
    // Microcylinder fabric BRDF.
    //
    // Reference:
    //
    //   Physically based shading at DreamWorks Animation: DWA Fabric Modelling
    //   http://blog.selfshadow.com/publications/s2017-shading-course/dreamworks/s2017_pbs_dreamworks_notes.pdf
    //

    const char* Model = "microcylfabric_brdf";

    class MicrocylFabricBRDFImpl
      : public BSDF
    {
      public:
        MicrocylFabricBRDFImpl(
            const char*                 name,
            const ParamArray&           params)
          : BSDF(name, Reflective, ScatteringMode::Glossy, params)
        {
            m_inputs.declare("reflectance", InputFormatSpectralReflectance);
            m_inputs.declare("reflectance_multiplier", InputFormatFloat, "1.0");
            m_inputs.declare("roughness" , InputFormatFloat, "0.1");
        }

        void release() override
        {
            delete this;
        }

        const char* get_model() const override
        {
            return Model;
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

            // Build exponent from roughness, Eq.7.
            const float inv_m = 1.0f - values->m_roughness;
            const float sqr_m = square(inv_m);
            values->m_exponent = fast_ceil(1.0f + 29 * sqr_m);
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

            // Sample phi
            const float phi = s[0] * Pi<float>();
            float cos_phi = cos(phi);
            float sin_phi = sin(phi);

            // Compute the BRDF value.
            const InputValues* values = static_cast<const InputValues*>(data);

            // Sample theta
            const float sin_theta = 1.0f - pow(s[1], 1.0f / (values->m_exponent + 1.0f));
            const float cos_theta = sqrt(max(0.0f, 1.0f - square(sin_theta)));

            // compute the halfway vector in world space
            Vector3f h =sample.m_shading_basis.transform_to_parent(
                    Vector3f::make_unit_vector(
                        cos_theta, sin_theta, cos_phi, sin_phi));

            Vector3f incoming = force_above_surface(
                        reflect(sample.m_outgoing.get_value(), h),
                        sample.m_geometric_normal);

            // Compute dot products
            const Vector3f& shading_normal =
                sample.m_shading_basis.get_normal();

            const float cos_hn = dot(h, shading_normal);
            const float sin_hn = sqrt(max(0.0f, 1.0f - square(cos_hn)));
            const float cos_ho = abs(dot(h, sample.m_outgoing.get_value()));
            const float denom = pow(1.0f - sin_hn, values->m_exponent);

            //sample.m_value.m_glossy = 1.0f;
            //sample.m_value.m_beauty = sample.m_value.m_glossy;

            // Compute the probability density of the sampled direction.
            sample.m_probability = (values->m_exponent + 1.0f) /
                    (RcpFourPi<float>() * cos_ho);

            sample.m_incoming = Dual3f(incoming);
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

            const float cos_in = abs(dot(incoming, n));
            const float cos_on = abs(dot(outgoing, n));
            const float cos_oh = abs(dot(outgoing, h));
            const float cos_hn = abs(dot(h, n));

            // Evaluate the fabric BRDF



            // Evaluate the PDF
            // Return the probability density of the sampled direction.
            return cos_in * RcpPi<float>();
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

            // get reflectance values

            // Compute the halfway vector in world space
            const Vector3f h = normalize(incoming + outgoing);

            float pdf_fabric = 0.0f;
            // Evaluate the PDF for the halfway vector

            const float cos_oh = abs(dot(outgoing, h));
            const float cos_hn = abs(dot(h, shading_basis.get_normal()));

            // Return the probability density of the sampled direction.
            return pdf_fabric;
        }

      private:
        typedef MicrocylFabricBRDFInputValues InputValues;

        static void fabric_brdf(
            const Spectrum&             reflectance,
            const float                 exponent,
            const Vector3f&             wi,
            const Vector3f&             wo,
            const Vector3f&             m,
            Spectrum&                   value)
        {
            ;
        }

        static float fabric_pdf(
            const float                 exponent,
            const Vector3f&             wo,
            const Vector3f&             m)
        {
            if (exponent == 0.0f)
                return 0.0f;

            const float cos_wom = dot(wo, m);
            if (cos_wom == 0.0f)
                return 0.0f;

            const float jacobian = 1.0f / (4.0f * abs(cos_wom));

            const float normalization = (exponent + 1.0f) *
                    RcpTwoPi<float>();

            return 1.0f;
        }
    };

    typedef BSDFWrapper<MicrocylFabricBRDFImpl> MicrocylFabricBRDF;
}


//
// MicrocylFabricBRDFFactory class implementation.
//

void MicrocylFabricBRDFFactory::release()
{
    delete this;
}

const char* MicrocylFabricBRDFFactory::get_model() const
{
    return Model;
}

Dictionary MicrocylFabricBRDFFactory::get_model_metadata() const
{
    return
        Dictionary()
            .insert("name", Model)
            .insert("label", "Microcylinder Fabric BRDF");
}

DictionaryArray MicrocylFabricBRDFFactory::get_input_metadata() const
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

    return metadata;
}

auto_release_ptr<BSDF> MicrocylFabricBRDFFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return auto_release_ptr<BSDF>(new MicrocylFabricBRDF(name, params));
}

}   // namespace renderer
