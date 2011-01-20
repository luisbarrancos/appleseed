
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2011 Francois Beaune
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
#include "samplegenerator.h"

// appleseed.renderer headers.
#include "renderer/kernel/rendering/progressive/progressiveframebuffer.h"
#include "renderer/kernel/rendering/progressive/sample.h"
#include "renderer/kernel/rendering/progressive/samplecounter.h"
#include "renderer/kernel/rendering/isamplerenderer.h"
#include "renderer/kernel/shading/shadingresult.h"
#include "renderer/modeling/frame/frame.h"

// appleseed.foundation headers.
#include "foundation/math/rng.h"
#include "foundation/utility/memory.h"

using namespace foundation;
using namespace std;

namespace renderer
{

//
// SampleGenerator class implementation.
//

#undef ENABLE_SAMPLE_GENERATION_DURING_CONTENTION

namespace
{
    const size_t SampleBatchSize = 67;
    const size_t AdditionalSampleCount = 4096;
    const size_t AdditionalSampleBatchSize = 64;
}

SampleGenerator::SampleGenerator(
    Frame&                      frame,
    ISampleRenderer*            sample_renderer,
    SampleCounter&              sample_counter,
    const size_t                generator_index,
    const size_t                generator_count,
    const bool                  enable_logging)
  : m_frame(frame)
  , m_sample_renderer(sample_renderer)
  , m_sample_counter(sample_counter)
  , m_lighting_conditions(frame.get_lighting_conditions())
  , m_enable_logging(enable_logging)
  , m_stride((generator_count - 1) * SampleBatchSize)
  , m_sequence_index(generator_index * SampleBatchSize)
  , m_current_batch_size(0)
  , m_sample_count(0)
  , m_pfb_lock_acquired_immediately(0)
  , m_pfb_lock_acquired_after_additional_work(0)
  , m_pfb_lock_acquired_after_blocking(0)
  , m_additional_sample_count(0)
{
}

SampleGenerator::~SampleGenerator()
{
    if (m_enable_logging)
    {
        const size_t total_acquisition_count = 
            m_pfb_lock_acquired_immediately +
            m_pfb_lock_acquired_after_additional_work +
            m_pfb_lock_acquired_after_blocking;

        RENDERER_LOG_DEBUG(
            "progressive framebuffer lock acquisition statistics:\n"
            "  acquired immediately            : %s\n"
            "  acquired after additional work  : %s\n"
            "  acquired after blocking         : %s\n"
            "  samples generated while waiting : %s\n",
            pretty_percent(m_pfb_lock_acquired_immediately, total_acquisition_count).c_str(),
            pretty_percent(m_pfb_lock_acquired_after_additional_work, total_acquisition_count).c_str(),
            pretty_percent(m_pfb_lock_acquired_after_blocking, total_acquisition_count).c_str(),
            pretty_uint(m_additional_sample_count).c_str());
    }
}

void SampleGenerator::generate_samples(
    const size_t                sample_count,
    ProgressiveFrameBuffer&     framebuffer)
{
    assert(sample_count > 0);

    ensure_size(m_samples, sample_count);
    m_sample_count = 0;

    generate_sample_vector(0, sample_count);
    store_samples(framebuffer);
}

void SampleGenerator::generate_sample_vector(const size_t index, const size_t count)
{
    Sample* RESTRICT sample_ptr = &m_samples[index];
    Sample* RESTRICT sample_end = sample_ptr + count;

    while (sample_ptr < sample_end)
    {
        generate_sample(*sample_ptr++);

        ++m_sequence_index;

        if (++m_current_batch_size == SampleBatchSize)
        {
            m_current_batch_size = 0;
            m_sequence_index += m_stride;
        }
    }

    m_sample_count += count;
}

void SampleGenerator::generate_sample(Sample& sample)
{
    // Compute the sample coordinates in [0,1)^2.
    const size_t Bases[2] = { 2, 3 };
    const Vector2d s = halton_sequence<double, 2>(Bases, m_sequence_index);

    // Compute the sample position, in NDC.
    const Vector2d sample_position = m_frame.get_sample_position(s.x, s.y);

    // Create a sampling context.
    SamplingContext sampling_context(
        m_rng,
        2,                      // number of dimensions
        0,                      // number of samples
        m_sequence_index);      // initial instance number

    // Render the sample.
    ShadingResult shading_result;
    m_sample_renderer->render_sample(
        sampling_context,
        sample_position,
        shading_result);

    // Transform the sample to the linear RGB color space.
    shading_result.transform_to_linear_rgb(m_lighting_conditions);

    // Return the sample.
    sample.m_position = sample_position;
    sample.m_color[0] = shading_result.m_color[0];
    sample.m_color[1] = shading_result.m_color[1];
    sample.m_color[2] = shading_result.m_color[2];
    sample.m_color[3] = shading_result.m_alpha[0];
}

void SampleGenerator::store_samples(ProgressiveFrameBuffer& framebuffer)
{
#ifdef ENABLE_SAMPLE_GENERATION_DURING_CONTENTION

    // Optimistically attempt to store the samples into the framebuffer.
    if (framebuffer.try_store_samples(m_sample_count, &m_samples[0]))
    {
        ++m_pfb_lock_acquired_immediately;
        return;
    }

    // That didn't work out. Make space for additional samples.
    const size_t max_sample_count = m_sample_count + AdditionalSampleCount;
    ensure_size(m_samples, max_sample_count);

    // Generate some more samples while the framebuffer is being used by another thread.
    while (m_sample_count < max_sample_count)
    {
        // Generate a bunch of additional samples.
        const size_t additional_sample_count =
            m_sample_counter.reserve(AdditionalSampleBatchSize);
        if (additional_sample_count == 0)
            break;
        generate_sample_vector(m_sample_count, additional_sample_count);
        m_additional_sample_count += additional_sample_count;

        // Attempt to store them into the framebuffer.
        if (framebuffer.try_store_samples(m_sample_count, &m_samples[0]))
        {
            ++m_pfb_lock_acquired_after_additional_work;
            return;
        }
    }

#endif

    // Give up: block until the framebuffer lock can be acquired.
    framebuffer.store_samples(m_sample_count, &m_samples[0]);
    ++m_pfb_lock_acquired_after_blocking;
}

}   // namespace renderer
