/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <../TimeStats/TimeStats.h>
#include <gui/ISurfaceComposer.h>
#include <gui/JankInfo.h>
#include <perfetto/trace/android/frame_timeline_event.pbzero.h>
#include <perfetto/tracing.h>
#include <ui/FenceTime.h>
#include <utils/RefBase.h>
#include <utils/String16.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include <deque>
#include <mutex>

namespace android::frametimeline {

class FrameTimelineTest;

using namespace std::chrono_literals;

// Metadata indicating how the frame was presented w.r.t expected present time.
enum class FramePresentMetadata : int8_t {
    // Frame was presented on time
    OnTimePresent,
    // Frame was presented late
    LatePresent,
    // Frame was presented early
    EarlyPresent,
    // Unknown/initial state
    UnknownPresent,
};

// Metadata comparing the frame's actual finish time to the expected deadline.
enum class FrameReadyMetadata : int8_t {
    // App/SF finished on time. Early finish is treated as on time since the goal of any component
    // is to finish before the deadline.
    OnTimeFinish,
    // App/SF finished work later than expected
    LateFinish,
    // Unknown/initial state
    UnknownFinish,
};

// Metadata comparing the frame's actual start time to the expected start time.
enum class FrameStartMetadata : int8_t {
    // App/SF started on time
    OnTimeStart,
    // App/SF started later than expected
    LateStart,
    // App/SF started earlier than expected
    EarlyStart,
    // Unknown/initial state
    UnknownStart,
};

/*
 * Collection of timestamps that can be used for both predictions and actual times.
 */
struct TimelineItem {
    TimelineItem(const nsecs_t startTime = 0, const nsecs_t endTime = 0,
                 const nsecs_t presentTime = 0)
          : startTime(startTime), endTime(endTime), presentTime(presentTime) {}

    nsecs_t startTime;
    nsecs_t endTime;
    nsecs_t presentTime;

    bool operator==(const TimelineItem& other) const {
        return startTime == other.startTime && endTime == other.endTime &&
                presentTime == other.presentTime;
    }

    bool operator!=(const TimelineItem& other) const { return !(*this == other); }
};

struct TokenManagerPrediction {
    nsecs_t timestamp = 0;
    TimelineItem predictions;
};

struct JankClassificationThresholds {
    // The various thresholds for App and SF. If the actual timestamp falls within the threshold
    // compared to prediction, we treat it as on time.
    nsecs_t presentThreshold = std::chrono::duration_cast<std::chrono::nanoseconds>(2ms).count();
    nsecs_t deadlineThreshold = std::chrono::duration_cast<std::chrono::nanoseconds>(2ms).count();
    nsecs_t startThreshold = std::chrono::duration_cast<std::chrono::nanoseconds>(2ms).count();
};

/*
 * TokenManager generates a running number token for a set of predictions made by VsyncPredictor. It
 * saves these predictions for a short period of time and returns the predictions for a given token,
 * if it hasn't expired.
 */
class TokenManager {
public:
    virtual ~TokenManager() = default;

    // Generates a token for the given set of predictions. Stores the predictions for 120ms and
    // destroys it later.
    virtual int64_t generateTokenForPredictions(TimelineItem&& prediction) = 0;

    // Returns the stored predictions for a given token, if the predictions haven't expired.
    virtual std::optional<TimelineItem> getPredictionsForToken(int64_t token) const = 0;
};

enum class PredictionState {
    Valid,   // Predictions obtained successfully from the TokenManager
    Expired, // TokenManager no longer has the predictions
    None,    // Predictions are either not present or didn't come from TokenManager
};

class SurfaceFrame {
public:
    enum class PresentState {
        Presented, // Buffer was latched and presented by SurfaceFlinger
        Dropped,   // Buffer was dropped by SurfaceFlinger
        Unknown,   // Initial state, SurfaceFlinger hasn't seen this buffer yet
    };

    // Only FrameTimeline can construct a SurfaceFrame as it provides Predictions(through
    // TokenManager), Thresholds and TimeStats pointer.
    SurfaceFrame(int64_t token, pid_t ownerPid, uid_t ownerUid, std::string layerName,
                 std::string debugName, PredictionState predictionState, TimelineItem&& predictions,
                 std::shared_ptr<TimeStats> timeStats, JankClassificationThresholds thresholds);
    ~SurfaceFrame() = default;

    // Returns std::nullopt if the frame hasn't been classified yet.
    // Used by both SF and FrameTimeline.
    std::optional<int32_t> getJankType() const;

    // Functions called by SF
    int64_t getToken() const { return mToken; };
    TimelineItem getPredictions() const { return mPredictions; };
    // Actual timestamps of the app are set individually at different functions.
    // Start time (if the app provides) and Queue time are accessible after queueing the frame,
    // whereas Acquire Fence time is available only during latch.
    void setActualStartTime(nsecs_t actualStartTime);
    void setActualQueueTime(nsecs_t actualQueueTime);
    void setAcquireFenceTime(nsecs_t acquireFenceTime);
    void setPresentState(PresentState presentState, nsecs_t lastLatchTime = 0);

    // Functions called by FrameTimeline
    // BaseTime is the smallest timestamp in this SurfaceFrame.
    // Used for dumping all timestamps relative to the oldest, making it easy to read.
    nsecs_t getBaseTime() const;
    // Sets the actual present time, appropriate metadata and classifies the jank.
    void onPresent(nsecs_t presentTime, int32_t displayFrameJankType, nsecs_t vsyncPeriod);
    // All the timestamps are dumped relative to the baseTime
    void dump(std::string& result, const std::string& indent, nsecs_t baseTime) const;
    // Emits a packet for perfetto tracing. The function body will be executed only if tracing is
    // enabled. The displayFrameToken is needed to link the SurfaceFrame to the corresponding
    // DisplayFrame at the trace processor side.
    void trace(int64_t displayFrameToken);

    // Getter functions used only by FrameTimelineTests
    TimelineItem getActuals() const;
    pid_t getOwnerPid() const { return mOwnerPid; };
    PredictionState getPredictionState() const { return mPredictionState; };
    PresentState getPresentState() const;
    FrameReadyMetadata getFrameReadyMetadata() const;
    FramePresentMetadata getFramePresentMetadata() const;

private:
    const int64_t mToken;
    const pid_t mOwnerPid;
    const uid_t mOwnerUid;
    const std::string mLayerName;
    const std::string mDebugName;
    PresentState mPresentState GUARDED_BY(mMutex);
    const PredictionState mPredictionState;
    const TimelineItem mPredictions;
    TimelineItem mActuals GUARDED_BY(mMutex);
    std::shared_ptr<TimeStats> mTimeStats;
    const JankClassificationThresholds mJankClassificationThresholds;
    nsecs_t mActualQueueTime GUARDED_BY(mMutex) = 0;
    mutable std::mutex mMutex;
    // Bitmask for the type of jank
    int32_t mJankType GUARDED_BY(mMutex) = JankType::None;
    // Indicates if this frame was composited by the GPU or not
    bool mGpuComposition GUARDED_BY(mMutex) = false;
    // Enum for the type of present
    FramePresentMetadata mFramePresentMetadata GUARDED_BY(mMutex) =
            FramePresentMetadata::UnknownPresent;
    // Enum for the type of finish
    FrameReadyMetadata mFrameReadyMetadata GUARDED_BY(mMutex) = FrameReadyMetadata::UnknownFinish;
    // Time when the previous buffer from the same layer was latched by SF. This is used in checking
    // for BufferStuffing where the current buffer is expected to be ready but the previous buffer
    // was latched instead.
    nsecs_t mLastLatchTime GUARDED_BY(mMutex) = 0;
};

/*
 * Maintains a history of SurfaceFrames grouped together by the vsync time in which they were
 * presented
 */
class FrameTimeline {
public:
    virtual ~FrameTimeline() = default;
    virtual TokenManager* getTokenManager() = 0;

    // Initializes the Perfetto DataSource that emits DisplayFrame and SurfaceFrame events. Test
    // classes can avoid double registration by mocking this function.
    virtual void onBootFinished() = 0;

    // Create a new surface frame, set the predictions based on a token and return it to the caller.
    // Debug name is the human-readable debugging string for dumpsys.
    virtual std::shared_ptr<SurfaceFrame> createSurfaceFrameForToken(std::optional<int64_t> token,
                                                                     pid_t ownerPid, uid_t ownerUid,
                                                                     std::string layerName,
                                                                     std::string debugName) = 0;

    // Adds a new SurfaceFrame to the current DisplayFrame. Frames from multiple layers can be
    // composited into one display frame.
    virtual void addSurfaceFrame(std::shared_ptr<SurfaceFrame> surfaceFrame) = 0;

    // The first function called by SF for the current DisplayFrame. Fetches SF predictions based on
    // the token and sets the actualSfWakeTime for the current DisplayFrame.
    virtual void setSfWakeUp(int64_t token, nsecs_t wakeupTime, nsecs_t vsyncPeriod) = 0;

    // Sets the sfPresentTime and finalizes the current DisplayFrame. Tracks the given present fence
    // until it's signaled, and updates the present timestamps of all presented SurfaceFrames in
    // that vsync.
    virtual void setSfPresent(nsecs_t sfPresentTime,
                              const std::shared_ptr<FenceTime>& presentFence) = 0;

    // Args:
    // -jank : Dumps only the Display Frames that are either janky themselves
    //         or contain janky Surface Frames.
    // -all : Dumps the entire list of DisplayFrames and the SurfaceFrames contained within
    virtual void parseArgs(const Vector<String16>& args, std::string& result) = 0;

    // Sets the max number of display frames that can be stored. Called by SF backdoor.
    virtual void setMaxDisplayFrames(uint32_t size);

    // Restores the max number of display frames to default. Called by SF backdoor.
    virtual void reset() = 0;
};

namespace impl {

class TokenManager : public android::frametimeline::TokenManager {
public:
    TokenManager() : mCurrentToken(ISurfaceComposer::INVALID_VSYNC_ID + 1) {}
    ~TokenManager() = default;

    int64_t generateTokenForPredictions(TimelineItem&& predictions) override;
    std::optional<TimelineItem> getPredictionsForToken(int64_t token) const override;

private:
    // Friend class for testing
    friend class android::frametimeline::FrameTimelineTest;

    void flushTokens(nsecs_t flushTime) REQUIRES(mMutex);

    std::map<int64_t, TokenManagerPrediction> mPredictions GUARDED_BY(mMutex);
    int64_t mCurrentToken GUARDED_BY(mMutex);
    mutable std::mutex mMutex;
    static constexpr nsecs_t kMaxRetentionTime =
            std::chrono::duration_cast<std::chrono::nanoseconds>(120ms).count();
};

class FrameTimeline : public android::frametimeline::FrameTimeline {
public:
    class FrameTimelineDataSource : public perfetto::DataSource<FrameTimelineDataSource> {
        void OnSetup(const SetupArgs&) override{};
        void OnStart(const StartArgs&) override{};
        void OnStop(const StopArgs&) override{};
    };

    /*
     * DisplayFrame should be used only internally within FrameTimeline. All members and methods are
     * guarded by FrameTimeline's mMutex.
     */
    class DisplayFrame {
    public:
        DisplayFrame(std::shared_ptr<TimeStats> timeStats, JankClassificationThresholds thresholds);
        virtual ~DisplayFrame() = default;
        // Dumpsys interface - dumps only if the DisplayFrame itself is janky or is at least one
        // SurfaceFrame is janky.
        void dumpJank(std::string& result, nsecs_t baseTime, int displayFrameCount) const;
        // Dumpsys interface - dumps all data irrespective of jank
        void dumpAll(std::string& result, nsecs_t baseTime) const;
        // Emits a packet for perfetto tracing. The function body will be executed only if tracing
        // is enabled.
        void trace(pid_t surfaceFlingerPid) const;
        // Sets the token, vsyncPeriod, predictions and SF start time.
        void onSfWakeUp(int64_t token, nsecs_t vsyncPeriod, std::optional<TimelineItem> predictions,
                        nsecs_t wakeUpTime);
        // Sets the appropriate metadata, classifies the jank and returns the classified jankType.
        void onPresent(nsecs_t signalTime);
        // Adds the provided SurfaceFrame to the current display frame.
        void addSurfaceFrame(std::shared_ptr<SurfaceFrame> surfaceFrame);

        void setTokenAndVsyncPeriod(int64_t token, nsecs_t vsyncPeriod);
        void setPredictions(PredictionState predictionState, TimelineItem predictions);
        void setActualStartTime(nsecs_t actualStartTime);
        void setActualEndTime(nsecs_t actualEndTime);

        // BaseTime is the smallest timestamp in a DisplayFrame.
        // Used for dumping all timestamps relative to the oldest, making it easy to read.
        nsecs_t getBaseTime() const;

        // Functions to be used only in testing.
        TimelineItem getActuals() const { return mSurfaceFlingerActuals; };
        TimelineItem getPredictions() const { return mSurfaceFlingerPredictions; };
        FramePresentMetadata getFramePresentMetadata() const { return mFramePresentMetadata; };
        FrameReadyMetadata getFrameReadyMetadata() const { return mFrameReadyMetadata; };
        int32_t getJankType() const { return mJankType; }
        const std::vector<std::shared_ptr<SurfaceFrame>>& getSurfaceFrames() const {
            return mSurfaceFrames;
        }

    private:
        void dump(std::string& result, nsecs_t baseTime) const;

        int64_t mToken = ISurfaceComposer::INVALID_VSYNC_ID;

        /* Usage of TimelineItem w.r.t SurfaceFlinger
         * startTime    Time when SurfaceFlinger wakes up to handle transactions and buffer updates
         * endTime      Time when SurfaceFlinger sends a composited frame to Display
         * presentTime  Time when the composited frame was presented on screen
         */
        TimelineItem mSurfaceFlingerPredictions;
        TimelineItem mSurfaceFlingerActuals;
        std::shared_ptr<TimeStats> mTimeStats;
        const JankClassificationThresholds mJankClassificationThresholds;

        // Collection of predictions and actual values sent over by Layers
        std::vector<std::shared_ptr<SurfaceFrame>> mSurfaceFrames;

        PredictionState mPredictionState = PredictionState::None;
        // Bitmask for the type of jank
        int32_t mJankType = JankType::None;
        // Indicates if this frame was composited by the GPU or not
        bool mGpuComposition = false;
        // Enum for the type of present
        FramePresentMetadata mFramePresentMetadata = FramePresentMetadata::UnknownPresent;
        // Enum for the type of finish
        FrameReadyMetadata mFrameReadyMetadata = FrameReadyMetadata::UnknownFinish;
        // Enum for the type of start
        FrameStartMetadata mFrameStartMetadata = FrameStartMetadata::UnknownStart;
        // The refresh rate (vsync period) in nanoseconds as seen by SF during this DisplayFrame's
        // timeline
        nsecs_t mVsyncPeriod = 0;
    };

    FrameTimeline(std::shared_ptr<TimeStats> timeStats, pid_t surfaceFlingerPid,
                  JankClassificationThresholds thresholds = {});
    ~FrameTimeline() = default;

    frametimeline::TokenManager* getTokenManager() override { return &mTokenManager; }
    std::shared_ptr<SurfaceFrame> createSurfaceFrameForToken(std::optional<int64_t> token,
                                                             pid_t ownerPid, uid_t ownerUid,
                                                             std::string layerName,
                                                             std::string debugName) override;
    void addSurfaceFrame(std::shared_ptr<frametimeline::SurfaceFrame> surfaceFrame) override;
    void setSfWakeUp(int64_t token, nsecs_t wakeupTime, nsecs_t vsyncPeriod) override;
    void setSfPresent(nsecs_t sfPresentTime,
                      const std::shared_ptr<FenceTime>& presentFence) override;
    void parseArgs(const Vector<String16>& args, std::string& result) override;
    void setMaxDisplayFrames(uint32_t size) override;
    void reset() override;

    // Sets up the perfetto tracing backend and data source.
    void onBootFinished() override;
    // Registers the data source with the perfetto backend. Called as part of onBootFinished()
    // and should not be called manually outside of tests.
    void registerDataSource();

    static constexpr char kFrameTimelineDataSource[] = "android.surfaceflinger.frametimeline";

private:
    // Friend class for testing
    friend class android::frametimeline::FrameTimelineTest;

    void flushPendingPresentFences() REQUIRES(mMutex);
    void finalizeCurrentDisplayFrame() REQUIRES(mMutex);
    void dumpAll(std::string& result);
    void dumpJank(std::string& result);

    // Sliding window of display frames. TODO(b/168072834): compare perf with fixed size array
    std::deque<std::shared_ptr<DisplayFrame>> mDisplayFrames GUARDED_BY(mMutex);
    std::vector<std::pair<std::shared_ptr<FenceTime>, std::shared_ptr<DisplayFrame>>>
            mPendingPresentFences GUARDED_BY(mMutex);
    std::shared_ptr<DisplayFrame> mCurrentDisplayFrame GUARDED_BY(mMutex);
    TokenManager mTokenManager;
    mutable std::mutex mMutex;
    uint32_t mMaxDisplayFrames;
    std::shared_ptr<TimeStats> mTimeStats;
    const pid_t mSurfaceFlingerPid;
    const JankClassificationThresholds mJankClassificationThresholds;
    static constexpr uint32_t kDefaultMaxDisplayFrames = 64;
    // The initial container size for the vector<SurfaceFrames> inside display frame. Although
    // this number doesn't represent any bounds on the number of surface frames that can go in a
    // display frame, this is a good starting size for the vector so that we can avoid the
    // internal vector resizing that happens with push_back.
    static constexpr uint32_t kNumSurfaceFramesInitial = 10;
};

} // namespace impl
} // namespace android::frametimeline
