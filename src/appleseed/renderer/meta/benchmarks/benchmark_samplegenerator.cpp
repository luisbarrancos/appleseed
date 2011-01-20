
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

// appleseed.renderer headers.
#include "renderer/global/global.h"
#include "renderer/kernel/rendering/debug/blanksamplerenderer.h"
#include "renderer/kernel/rendering/progressive/progressiveframebuffer.h"
#include "renderer/kernel/rendering/progressive/samplecounter.h"
#include "renderer/kernel/rendering/progressive/samplegenerator.h"
#include "renderer/modeling/frame/frame.h"

// appleseed.foundation headers.
#include "foundation/utility/benchmark.h"
#include "foundation/utility/job.h"

// Standard headers.
#include <vector>

using namespace foundation;
using namespace renderer;
using namespace std;

BENCHMARK_SUITE(Renderer_Kernel_Rendering_Progressive_SampleGenerator)
{
    const size_t ThreadCount = 16;
    const size_t BatchSize = 1;
    const size_t BatchCount = 16 * 512;
    const size_t MaxSampleCount = ThreadCount * BatchSize * BatchCount;

    struct Fixture
    {
        Frame                       m_frame;
        ProgressiveFrameBuffer      m_framebuffer;
        SampleCounter               m_sample_counter;
        Logger                      m_logger;
        JobQueue                    m_job_queue;
        JobManager                  m_job_manager;
        vector<ISampleRenderer*>    m_sample_renderers;
        vector<SampleGenerator*>    m_sample_generators;

        Fixture()
          : m_frame("frame", ParamArray().insert("resolution", "512 512"))
          , m_framebuffer(512, 512)
          , m_sample_counter(MaxSampleCount)
          , m_job_manager(m_logger, m_job_queue, ThreadCount)
        {
            m_job_manager.start();

            for (size_t i = 0; i < ThreadCount; ++i)
            {
                ISampleRenderer* sample_renderer = BlankSampleRendererFactory().create();
                m_sample_renderers.push_back(sample_renderer);

                m_sample_generators.push_back(
                    new SampleGenerator(
                        m_frame,
                        sample_renderer,
                        m_sample_counter,
                        i,
                        ThreadCount));
            }
        }

        ~Fixture()
        {
            for (size_t i = 0; i < ThreadCount; ++i)
            {
                delete m_sample_generators[i];
                delete m_sample_renderers[i];
            }
        }
    };

    struct SampleGeneratorJob
      : public IJob
    {
        Fixture&            m_fixture;
        const size_t        m_generator_index;

        SampleGeneratorJob(
            Fixture&        fixture,
            const size_t    generator_index)
          : m_fixture(fixture)
          , m_generator_index(generator_index)
        {
        }

        virtual void execute(const size_t thread_index)
        {
            while (true)
            {
                const size_t sample_count =
                    m_fixture.m_sample_counter.reserve(BatchSize);

                if (sample_count == 0)
                    break;

                SampleGenerator* sample_generator =
                    m_fixture.m_sample_generators[m_generator_index];

                sample_generator->generate_samples(
                    sample_count,
                    m_fixture.m_framebuffer);
            }
        }
    };

    BENCHMARK_CASE_WITH_FIXTURE(BenchmarkConcurrentSampleGeneration, Fixture)
    {
        for (size_t i = 0; i < ThreadCount; ++i)
            m_job_queue.schedule(new SampleGeneratorJob(*this, i));

        m_job_queue.wait_until_completion();

        assert(m_sample_counter.read() == MaxSampleCount);
    }
}
