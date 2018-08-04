
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

            const InputValues* values = static_cast<const InputValues*>(data);

            sample.m_max_roughness = values->m_roughness;
            sample.m_mode = ScatteringMode::Glossy;

            // Compute the incoming direction.
            const Vector3f& outgoing = sample.m_outgoing.get_value();
            const Vector3f wo = sample.m_shading_basis.transform_to_local(outgoing);

            // get 2 RNG numbers.
            sampling_context.split_in_place(2, 1);
            const Vector2f s = sampling_context.next2<Vector2f>();

            // Sample phi and theta.
            const float phi = s[0] * Pi<float>();
            const float cos_phi = cos(phi);
            const float sin_phi = sin(phi);

            const float sin_theta = 1.0f - pow(s[1], 1.0f / (values->m_precomputed.m_exponent + 1.0f));
            const float cos_theta = sqrt(max(0.0f, 1.0f - square(sin_theta)));

            // Generate h and reflected vector.
            const Vector3f h = Vector3f::make_unit_vector(cos_theta, sin_theta, cos_phi, sin_phi);
            const Vector3f wi = improve_normalization(reflect(wo, h));

            if (wi.y < 0.0f)
                return;

            evaluate_fabric_brdf(
                        values->m_reflectance,
                        values->m_precomputed.m_exponent,
                        wi,
                        wo,
                        h,
                        sample.m_value.m_glossy);

            sample.m_probability = fabric_pdf(
                        values->m_precomputed.m_exponent,
                        wo,
                        h);

            sample.m_value.m_beauty = sample.m_value.m_glossy;
            sample.m_incoming = Dual3f(sample.m_shading_basis.transform_to_parent(wi));
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

            const InputValues* values = static_cast<const InputValues*>(data);

            const Vector3f wo = shading_basis.transform_to_local(outgoing);
            const Vector3f wi = shading_basis.transform_to_local(incoming);
            const Vector3f h = normalize(wi + wo);

            evaluate_fabric_brdf(
                        values->m_reflectance,
                        values->m_precomputed.m_exponent,
                        wi,
                        wo,
                        h,
                        value.m_glossy);

            value.m_beauty = value.m_glossy;

            return fabric_pdf(
                        values->m_precomputed.m_exponent,
                        wo,
                        wi);
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

            const Vector3f wo = shading_basis.transform_to_local(outgoing);
            const Vector3f wi = shading_basis.transform_to_local(incoming);
            const Vector3f h = normalize(wi + wo);

            return fabric_pdf(
                        values->m_precomputed.m_exponent,
                        wo,
                        h);
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

        static void evaluate_fabric_brdf(
            const Spectrum&             reflectance,
            const float                 exponent,
            const Vector3f&             wi,
            const Vector3f&             wo,
            const Vector3f&             h,
            Spectrum&                   value)
        {
            const float cos_theta = h.y;

            if (cos_theta == 0.0f)
            {
                value.set(0.0f);
                return;
            }

            const float denom = abs(4.0f * wo.y * wi.y);

            if (denom == 0.0f)
            {
                value.set(0.0f);
                return;
            }

            // Distribution for fiber, eq.4.
            const float sin_theta = sqrt(max(0.0f, 1.0f - square(cos_theta)));
            const float brdf = pow(1.0f - sin_theta, exponent);

            value = reflectance * brdf / denom;
        }

        static float fabric_pdf(
                const float         exponent,
                const Vector3f&     wo,
                const Vector3f&     h)
        {
            const float cos_theta = h.y;

            if (cos_theta == 0.0f)
                return 0.0f;

            const float cos_ho = dot(h, wo);

            if (cos_ho == 0.0f)
                return 0.0f;

            // PDF transformed from theta_h to theta_i domain, eq.11.
            const float jacobian = 1.0f / (4.0f * abs(cos_ho));

            const float sin_theta = sqrt(max(0.0f, 1.0f - square(cos_theta)));
            const float pdf = pow(1.0f - sin_theta, exponent);

            return pdf * ((exponent + 1) / (4.0f * Pi<float>() * abs(cos_ho)));
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
