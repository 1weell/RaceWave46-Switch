//
// RT64
//

#include "rt64_raster_shader_cache.h"

#include "common/rt64_thread.h"
#include "common/rt64_wr64_stall_probe.h"

#define ENABLE_OPTIMIZED_SHADER_GENERATION

namespace RT64 {
    // RasterShaderCache::CompilationThread
    
    RasterShaderCache::CompilationThread::CompilationThread(RasterShaderCache *shaderCache) {
        assert(shaderCache != nullptr);

        this->shaderCache = shaderCache;

        thread = std::make_unique<std::thread>(&CompilationThread::loop, this);
    }

    RasterShaderCache::CompilationThread::~CompilationThread() {
        // Change the wait predicate under its mutex to avoid a lost wakeup.
        {
            const std::lock_guard<std::mutex> lock(shaderCache->descQueueMutex);
            threadRunning = false;
        }
        shaderCache->descQueueChanged.notify_all();
        thread->join();
        thread.reset(nullptr);
    }

    void RasterShaderCache::CompilationThread::loop() {
        Thread::setCurrentThreadName("RT64 Shader");
        wr64StallProbeNameThread("shader-compile");

        // The shader compilation thread should have idle priority by default as the application can use the ubershader in the meantime.
        Thread::setCurrentThreadPriority(Thread::Priority::Idle);

        while (true) {
            ShaderDescription shaderDesc;
            bool fromPriorityQueue = false;
            
            // Check the top of the queue or wait if it's empty.
            {
                std::unique_lock<std::mutex> queueLock(shaderCache->descQueueMutex);
                shaderCache->descQueueChanged.wait(queueLock, [this]() {
                    return !threadRunning || !shaderCache->descQueue.empty();
                });

                // Finish an in-flight compile, but do not start more work on shutdown.
                if (!threadRunning) {
                    break;
                }

                if (!shaderCache->descQueue.empty()) {
                    shaderDesc = shaderCache->descQueue.front();
                    shaderCache->descQueue.pop();
                    fromPriorityQueue = true;
                    shaderCache->descQueueActiveCount++;
                }
            }
            
            // Compile the shader at the top of the queue.
            if (fromPriorityQueue) {
                assert((shaderCache->shaderUber != nullptr) && "Ubershader should've been created by the time a new shader is submitted to the cache.");
                const RenderPipelineLayout *uberPipelineLayout = shaderCache->shaderUber->pipelineLayout.get();
                const RenderMultisampling multisampling = shaderCache->multisampling;
                std::unique_ptr<RasterShader> newShader = std::make_unique<RasterShader>(shaderCache->device, shaderDesc, uberPipelineLayout, shaderCache->shaderFormat, multisampling, shaderCache->shaderCompiler.get(), &shaderCache->optimizerCacheSPIRV);

                {
                    // The insert side of the same mutex. If shader-map-lock on the
                    // draw thread ever shows up in a dump, this is what it waited on.
                    // The size is read inside the lock, not as a constructor argument,
                    // because the draw thread is reading this map concurrently.
                    WR64StallProbe insertProbe("shader-map-insert");
                    const std::unique_lock<std::mutex> lock(shaderCache->GPUShadersMutex);
                    shaderCache->GPUShaders[shaderDesc.hash()] = std::move(newShader);
                    insertProbe.setDetail(uint64_t(shaderCache->GPUShaders.size()));
                }

                {
                    const std::lock_guard<std::mutex> lock(shaderCache->descQueueMutex);
                    shaderCache->descQueueActiveCount--;
                }
                shaderCache->compilationIdle.notify_all();
            }
        }
    }

    // RasterShaderCache

    RasterShaderCache::RasterShaderCache(uint32_t threadCount, uint32_t ubershaderThreadCount) {
        assert(threadCount > 0);

        this->threadCount = threadCount;
        this->ubershaderThreadCount = ubershaderThreadCount;

#ifdef ENABLE_OPTIMIZED_SHADER_GENERATION
#   ifdef _WIN32
        shaderCompiler = std::make_unique<ShaderCompiler>();
#   endif

        for (uint32_t t = 0; t < threadCount; t++) {
            compilationThreads.push_back(std::make_unique<CompilationThread>(this));
        }
#endif
    }

    RasterShaderCache::~RasterShaderCache() {
        // Stop the whole pool before joining any one worker. Otherwise the
        // remaining workers keep consuming queued work during teardown.
        {
            const std::lock_guard<std::mutex> lock(descQueueMutex);
            for (const auto &worker : compilationThreads) {
                worker->threadRunning = false;
            }
        }
        descQueueChanged.notify_all();
        compilationThreads.clear();
    }

    void RasterShaderCache::setup(RenderDevice *device, RenderShaderFormat shaderFormat, const ShaderLibrary *shaderLibrary, const RenderMultisampling &multisampling) {
        assert(device != nullptr);

        this->device = device;
        this->shaderFormat = shaderFormat;
        this->multisampling = multisampling;

        shaderUber = std::make_unique<RasterShaderUber>(device, shaderFormat, multisampling, shaderLibrary, ubershaderThreadCount);
        usesHDR = shaderLibrary->usesHDR;

        // Initialize the re-spirv optimizer cache.
        if (shaderFormat == RenderShaderFormat::SPIRV) {
            optimizerCacheSPIRV.initialize();
        }
    }

    void RasterShaderCache::submit(const ShaderDescription &desc) {
        {
            std::unique_lock<std::mutex> queueLock(submissionMutex);

            // Verify if an entry with the same hash was already submitted before.
            const uint64_t shaderHash = desc.hash();
            bool &found = shaderHashes[shaderHash];
            if (found) {
                return;
            }

            found = true;
        }

        // Push a new shader compilation to the queue.
        {
            const std::unique_lock<std::mutex> queueLock(descQueueMutex);
            descQueue.push(desc);
        }

        descQueueChanged.notify_one();
    }
    
    void RasterShaderCache::waitForAll() {
        // Preserve the reset contract: discard queued work and wait only for
        // in-flight compiles before the caller destroys/reconfigures shaders.
        std::unique_lock<std::mutex> queueLock(descQueueMutex);
        descQueue = std::queue<ShaderDescription>();
        compilationIdle.wait(queueLock, [this]() { return descQueueActiveCount == 0; });
    }

    void RasterShaderCache::destroyAll() {
        {
            std::unique_lock<std::mutex> lock(GPUShadersMutex);
            GPUShaders.clear();
        }

        {
            std::unique_lock<std::mutex> queueLock(submissionMutex);
            shaderHashes.clear();
        }
    }

    RasterShader *RasterShaderCache::getGPUShader(const ShaderDescription &desc) {
        const uint64_t shaderHash = desc.hash();

        // Draw path lookup against a mutex that idle priority compilation threads
        // also take to insert into the same unordered_map. An insert that rehashes
        // holds this for the whole rehash, and the compile threads run at Idle, so a
        // preempted one can hold it far longer than the work itself takes.
        WR64StallProbeHot shaderLockProbe("shader-map-lock");
        const std::unique_lock<std::mutex> lock(GPUShadersMutex);
        auto shaderIt = GPUShaders.find(shaderHash);
        if (shaderIt == GPUShaders.end()) {
            return nullptr;
        }

        return shaderIt->second.get();
    }

    RasterShaderUber *RasterShaderCache::getGPUShaderUber() const {
        return shaderUber.get();
    }

    uint32_t RasterShaderCache::shaderCount() {
        std::unique_lock<std::mutex> lock(GPUShadersMutex);
        return GPUShaders.size();
    }
};
