// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/scheduler/scheduler.h"

#include <stddef.h>

#include <string>
#include <vector>

#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/numerics/safe_conversions.h"
#include "base/run_loop.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "cc/test/scheduler_test_common.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/test/begin_frame_args_test.h"
#include "components/viz/test/fake_delay_based_time_source.h"
#include "components/viz/test/fake_external_begin_frame_source.h"
#include "components/viz/test/ordered_simple_task_runner.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#define EXPECT_ACTIONS(...) \
  EXPECT_THAT(client_->Actions(), ::testing::ElementsAre(__VA_ARGS__))

#define EXPECT_NO_ACTION() EXPECT_THAT(client_->Actions(), ::testing::IsEmpty())

#define EXPECT_SCOPED(statements) \
  {                               \
    SCOPED_TRACE("");             \
    statements;                   \
  }

namespace cc {
namespace {

base::TimeDelta kSlowDuration = base::TimeDelta::FromSeconds(1);
base::TimeDelta kFastDuration = base::TimeDelta::FromMilliseconds(1);

class FakeSchedulerClient : public SchedulerClient,
                            public viz::FakeExternalBeginFrameSource::Client {
 public:
  FakeSchedulerClient()
      : inside_begin_impl_frame_(false),
        automatic_ack_(true),
        scheduler_(nullptr) {
    Reset();
  }

  void Reset() {
    actions_.clear();
    states_.clear();
    will_begin_impl_frame_causes_redraw_ = false;
    will_begin_impl_frame_requests_one_begin_impl_frame_ = false;
    draw_will_happen_ = true;
    swap_will_happen_if_draw_happens_ = true;
    num_draws_ = 0;
    last_begin_main_frame_args_ = viz::BeginFrameArgs();
    last_begin_frame_ack_ = viz::BeginFrameAck();
  }

  void set_scheduler(TestScheduler* scheduler) { scheduler_ = scheduler; }

  bool needs_begin_frames() { return scheduler_->begin_frames_expected(); }
  int num_draws() const { return num_draws_; }
  const std::vector<std::string> Actions() const {
    return std::vector<std::string>(actions_.begin(), actions_.end());
  }
  std::string StateForAction(int i) const { return states_[i]->ToString(); }
  base::TimeTicks posted_begin_impl_frame_deadline() const {
    return posted_begin_impl_frame_deadline_;
  }

  int ActionIndex(const char* action) const {
    for (size_t i = 0; i < actions_.size(); i++)
      if (!strcmp(actions_[i], action))
        return base::checked_cast<int>(i);
    return -1;
  }

  bool HasAction(const char* action) const { return ActionIndex(action) >= 0; }

  void SetWillBeginImplFrameRequestsOneBeginImplFrame(bool request) {
    will_begin_impl_frame_requests_one_begin_impl_frame_ = request;
  }
  void SetWillBeginImplFrameCausesRedraw(bool causes_redraw) {
    will_begin_impl_frame_causes_redraw_ = causes_redraw;
  }
  void SetDrawWillHappen(bool draw_will_happen) {
    draw_will_happen_ = draw_will_happen;
  }
  void SetSwapWillHappenIfDrawHappens(bool swap_will_happen_if_draw_happens) {
    swap_will_happen_if_draw_happens_ = swap_will_happen_if_draw_happens;
  }
  void SetAutomaticSubmitCompositorFrameAck(bool automatic_ack) {
    automatic_ack_ = automatic_ack;
  }
  // SchedulerClient implementation.
  void WillBeginImplFrame(const viz::BeginFrameArgs& args) override {
    EXPECT_FALSE(inside_begin_impl_frame_);
    inside_begin_impl_frame_ = true;
    PushAction("WillBeginImplFrame");
    if (will_begin_impl_frame_requests_one_begin_impl_frame_)
      scheduler_->SetNeedsOneBeginImplFrame();
    if (will_begin_impl_frame_causes_redraw_)
      scheduler_->SetNeedsRedraw();
  }
  void DidFinishImplFrame() override {
    EXPECT_TRUE(inside_begin_impl_frame_);
    inside_begin_impl_frame_ = false;
  }
  void DidNotProduceFrame(const viz::BeginFrameAck& ack) override {
    last_begin_frame_ack_ = ack;
  }

  void ScheduledActionSendBeginMainFrame(
      const viz::BeginFrameArgs& args) override {
    PushAction("ScheduledActionSendBeginMainFrame");
    last_begin_main_frame_args_ = args;
  }

  const viz::BeginFrameArgs& last_begin_main_frame_args() {
    return last_begin_main_frame_args_;
  }

  const viz::BeginFrameAck& last_begin_frame_ack() {
    return last_begin_frame_ack_;
  }

  DrawResult ScheduledActionDrawIfPossible() override {
    PushAction("ScheduledActionDrawIfPossible");
    num_draws_++;
    DrawResult result =
        draw_will_happen_ ? DRAW_SUCCESS : DRAW_ABORTED_CHECKERBOARD_ANIMATIONS;
    bool swap_will_happen =
        draw_will_happen_ && swap_will_happen_if_draw_happens_;
    if (swap_will_happen) {
      last_begin_frame_ack_ = scheduler_->CurrentBeginFrameAckForActiveTree();
      scheduler_->DidSubmitCompositorFrame();

      if (automatic_ack_)
        scheduler_->DidReceiveCompositorFrameAck();
    }
    return result;
  }
  DrawResult ScheduledActionDrawForced() override {
    PushAction("ScheduledActionDrawForced");
    last_begin_frame_ack_ = scheduler_->CurrentBeginFrameAckForActiveTree();
    return DRAW_SUCCESS;
  }
  void ScheduledActionCommit() override {
    PushAction("ScheduledActionCommit");
    scheduler_->DidCommit();
  }
  void ScheduledActionActivateSyncTree() override {
    PushAction("ScheduledActionActivateSyncTree");
  }
  void ScheduledActionBeginLayerTreeFrameSinkCreation() override {
    PushAction("ScheduledActionBeginLayerTreeFrameSinkCreation");
  }
  void ScheduledActionPrepareTiles() override {
    PushAction("ScheduledActionPrepareTiles");
    scheduler_->WillPrepareTiles();
    scheduler_->DidPrepareTiles();
  }
  void ScheduledActionInvalidateLayerTreeFrameSink() override {
    actions_.push_back("ScheduledActionInvalidateLayerTreeFrameSink");
    states_.push_back(scheduler_->AsValue());
  }
  void ScheduledActionPerformImplSideInvalidation() override {
    PushAction("ScheduledActionPerformImplSideInvalidation");
  }

  void SendBeginMainFrameNotExpectedSoon() override {
    PushAction("SendBeginMainFrameNotExpectedSoon");
  }

  void ScheduledActionBeginMainFrameNotExpectedUntil(
      base::TimeTicks time) override {
    PushAction("ScheduledActionBeginMainFrameNotExpectedUntil");
  }

  bool IsInsideBeginImplFrame() const { return inside_begin_impl_frame_; }

  base::Callback<bool(void)> InsideBeginImplFrame(bool state) {
    return base::Bind(&FakeSchedulerClient::InsideBeginImplFrameCallback,
                      base::Unretained(this), state);
  }

  bool IsCurrentFrame(int last_frame_number) const {
    return scheduler_->current_frame_number() == last_frame_number;
  }

  base::Callback<bool(void)> FrameHasNotAdvancedCallback() {
    return base::Bind(&FakeSchedulerClient::IsCurrentFrame,
                      base::Unretained(this),
                      scheduler_->current_frame_number());
  }

  void PushAction(const char* description) {
    actions_.push_back(description);
    states_.push_back(scheduler_->AsValue());
  }

  // FakeExternalBeginFrameSource::Client implementation.
  void OnAddObserver(viz::BeginFrameObserver* obs) override {
    PushAction("AddObserver(this)");
  }
  void OnRemoveObserver(viz::BeginFrameObserver* obs) override {
    PushAction("RemoveObserver(this)");
  }

  size_t CompositedAnimationsCount() const override { return 0; }
  size_t MainThreadAnimationsCount() const override { return 0; }
  size_t MainThreadCompositableAnimationsCount() const override { return 0; }

 protected:
  bool InsideBeginImplFrameCallback(bool state) {
    return inside_begin_impl_frame_ == state;
  }

  bool inside_begin_impl_frame_;
  bool will_begin_impl_frame_causes_redraw_;
  bool will_begin_impl_frame_requests_one_begin_impl_frame_;
  bool draw_will_happen_;
  bool swap_will_happen_if_draw_happens_;
  bool automatic_ack_;
  int num_draws_;
  viz::BeginFrameArgs last_begin_main_frame_args_;
  viz::BeginFrameAck last_begin_frame_ack_;
  base::TimeTicks posted_begin_impl_frame_deadline_;
  std::vector<const char*> actions_;
  std::vector<std::unique_ptr<base::trace_event::ConvertableToTraceFormat>>
      states_;
  TestScheduler* scheduler_;
};

enum BeginFrameSourceType {
  EXTERNAL_BFS,
  UNTHROTTLED_BFS,
  THROTTLED_BFS,
};

class SchedulerTest : public testing::Test {
 public:
  SchedulerTest()
      : now_src_(new base::SimpleTestTickClock()),
        task_runner_(new OrderedSimpleTaskRunner(now_src_.get(), true)),
        fake_external_begin_frame_source_(nullptr),
        fake_compositor_timing_history_(nullptr) {
    now_src_->Advance(base::TimeDelta::FromMicroseconds(10000));
    // A bunch of tests require NowTicks()
    // to be > viz::BeginFrameArgs::DefaultInterval()
    now_src_->Advance(base::TimeDelta::FromMilliseconds(100));
    // Fail if we need to run 100 tasks in a row.
    task_runner_->SetRunTaskLimit(100);
  }

  ~SchedulerTest() override = default;

 protected:
  TestScheduler* CreateScheduler(BeginFrameSourceType bfs_type) {
    viz::BeginFrameSource* frame_source = nullptr;
    unthrottled_frame_source_.reset(new viz::BackToBackBeginFrameSource(
        std::make_unique<viz::FakeDelayBasedTimeSource>(now_src_.get(),
                                                        task_runner_.get())));
    fake_external_begin_frame_source_.reset(
        new viz::FakeExternalBeginFrameSource(1.0, false));
    fake_external_begin_frame_source_->SetClient(client_.get());
    synthetic_frame_source_ = std::make_unique<viz::DelayBasedBeginFrameSource>(
        std::make_unique<viz::FakeDelayBasedTimeSource>(now_src_.get(),
                                                        task_runner_.get()),
        viz::BeginFrameSource::kNotRestartableId);
    switch (bfs_type) {
      case EXTERNAL_BFS:
        frame_source = fake_external_begin_frame_source_.get();
        break;
      case UNTHROTTLED_BFS:
        frame_source = unthrottled_frame_source_.get();
        break;
      case THROTTLED_BFS:
        frame_source = synthetic_frame_source_.get();
        break;
    }
    DCHECK(frame_source);

    std::unique_ptr<FakeCompositorTimingHistory>
        fake_compositor_timing_history = FakeCompositorTimingHistory::Create(
            scheduler_settings_.using_synchronous_renderer_compositor);
    fake_compositor_timing_history_ = fake_compositor_timing_history.get();

    scheduler_.reset(new TestScheduler(
        now_src_.get(), client_.get(), scheduler_settings_, 0,
        task_runner_.get(), std::move(fake_compositor_timing_history)));
    client_->set_scheduler(scheduler_.get());
    scheduler_->SetBeginFrameSource(frame_source);

    // Use large estimates by default to avoid latency recovery in most tests.
    fake_compositor_timing_history_->SetAllEstimatesTo(kSlowDuration);

    return scheduler_.get();
  }

  void SetUpScheduler(BeginFrameSourceType bfs_type,
                      std::unique_ptr<FakeSchedulerClient> client) {
    client_ = std::move(client);
    CreateScheduler(bfs_type);
    EXPECT_SCOPED(InitializeLayerTreeFrameSinkAndFirstCommit());
  }

  void SetUpScheduler(BeginFrameSourceType bfs_type) {
    SetUpScheduler(bfs_type, std::make_unique<FakeSchedulerClient>());
  }

  void SetUpSchedulerWithNoLayerTreeFrameSink(BeginFrameSourceType bfs_type) {
    client_ = std::make_unique<FakeSchedulerClient>();
    CreateScheduler(bfs_type);
  }

  OrderedSimpleTaskRunner& task_runner() { return *task_runner_; }
  base::SimpleTestTickClock* now_src() { return now_src_.get(); }

  // As this function contains EXPECT macros, to allow debugging it should be
  // called inside EXPECT_SCOPED like so;
  //   EXPECT_SCOPED(
  //       client.InitializeLayerTreeFrameSinkAndFirstCommit(scheduler));
  void InitializeLayerTreeFrameSinkAndFirstCommit() {
    TRACE_EVENT0(
        "cc", "SchedulerUnitTest::InitializeLayerTreeFrameSinkAndFirstCommit");
    DCHECK(scheduler_);

    // Check the client doesn't have any actions queued when calling this
    // function.
    EXPECT_NO_ACTION();
    EXPECT_FALSE(scheduler_->begin_frames_expected());

    // Start the initial LayerTreeFrameSink creation.
    scheduler_->SetVisible(true);
    scheduler_->SetCanDraw(true);
    EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");

    client_->Reset();

    // We don't see anything happening until the first impl frame.
    scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
    scheduler_->SetNeedsBeginMainFrame();
    EXPECT_TRUE(scheduler_->begin_frames_expected());
    EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    client_->Reset();

    {
      SCOPED_TRACE("Do first frame to commit after initialize.");
      AdvanceFrame();

      now_src_->Advance(base::TimeDelta::FromMilliseconds(1));
      scheduler_->NotifyBeginMainFrameStarted(now_src_->NowTicks());
      scheduler_->NotifyReadyToCommit();
      scheduler_->NotifyReadyToActivate();
      scheduler_->NotifyReadyToDraw();

      EXPECT_FALSE(scheduler_->CommitPending());

      if (scheduler_settings_.using_synchronous_renderer_compositor) {
        scheduler_->SetNeedsRedraw();
        bool resourceless_software_draw = false;
        scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
      } else {
        // Run the posted deadline task.
        EXPECT_TRUE(client_->IsInsideBeginImplFrame());
        task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
      }

      EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    }

    client_->Reset();

    {
      SCOPED_TRACE(
          "Run second frame so Scheduler calls SetNeedsBeginFrame(false).");
      AdvanceFrame();

      if (!scheduler_settings_.using_synchronous_renderer_compositor) {
        // Run the posted deadline task.
        EXPECT_TRUE(client_->IsInsideBeginImplFrame());
        task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
      }

      EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    }

    EXPECT_FALSE(scheduler_->begin_frames_expected());

    if (scheduler_->begin_frame_source() ==
        fake_external_begin_frame_source_.get()) {
      // Expect the last viz::BeginFrameAck to be for last BeginFrame, which
      // didn't cause damage.
      uint64_t last_begin_frame_number =
          fake_external_begin_frame_source_->next_begin_frame_number() - 1;
      bool has_damage = false;
      EXPECT_EQ(
          viz::BeginFrameAck(fake_external_begin_frame_source_->source_id(),
                             last_begin_frame_number, has_damage),
          client_->last_begin_frame_ack());
    }

    client_->Reset();
  }

  // As this function contains EXPECT macros, to allow debugging it should be
  // called inside EXPECT_SCOPED like so;
  //   EXPECT_SCOPED(client.AdvanceFrame());
  void AdvanceFrame() {
    TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("cc.debug.scheduler.frames"),
                 "FakeSchedulerClient::AdvanceFrame");

    // Send the next BeginFrame message if using an external source, otherwise
    // it will be already in the task queue.
    if (scheduler_->begin_frame_source() ==
        fake_external_begin_frame_source_.get()) {
      EXPECT_TRUE(scheduler_->begin_frames_expected());
      SendNextBeginFrame();
    } else {
      task_runner_->RunTasksWhile(client_->FrameHasNotAdvancedCallback());
    }
  }

  viz::BeginFrameArgs SendNextBeginFrame() {
    DCHECK_EQ(scheduler_->begin_frame_source(),
              fake_external_begin_frame_source_.get());
    // Creep the time forward so that any viz::BeginFrameArgs is not equal to
    // the last one otherwise we violate the viz::BeginFrameSource contract.
    now_src_->Advance(viz::BeginFrameArgs::DefaultInterval());
    viz::BeginFrameArgs args =
        fake_external_begin_frame_source_->CreateBeginFrameArgs(
            BEGINFRAME_FROM_HERE, now_src());
    fake_external_begin_frame_source_->TestOnBeginFrame(args);
    return args;
  }

  viz::FakeExternalBeginFrameSource* fake_external_begin_frame_source() const {
    return fake_external_begin_frame_source_.get();
  }

  void AdvanceAndMissOneFrame();
  void CheckMainFrameSkippedAfterLateCommit(bool expect_send_begin_main_frame);
  void ImplFrameSkippedAfterLateAck(bool receive_ack_before_deadline);
  void ImplFrameNotSkippedAfterLateAck();
  void BeginFramesNotFromClient(BeginFrameSourceType bfs_type);
  void BeginFramesNotFromClient_IsDrawThrottled(BeginFrameSourceType bfs_type);
  bool BeginMainFrameOnCriticalPath(TreePriority tree_priority,
                                    ScrollHandlerState scroll_handler_state,
                                    base::TimeDelta durations);

  std::unique_ptr<base::SimpleTestTickClock> now_src_;
  scoped_refptr<OrderedSimpleTaskRunner> task_runner_;
  std::unique_ptr<viz::FakeExternalBeginFrameSource>
      fake_external_begin_frame_source_;
  std::unique_ptr<viz::SyntheticBeginFrameSource> synthetic_frame_source_;
  std::unique_ptr<viz::SyntheticBeginFrameSource> unthrottled_frame_source_;
  SchedulerSettings scheduler_settings_;
  std::unique_ptr<FakeSchedulerClient> client_;
  std::unique_ptr<TestScheduler> scheduler_;
  FakeCompositorTimingHistory* fake_compositor_timing_history_;
};

TEST_F(SchedulerTest, InitializeLayerTreeFrameSinkDoesNotBeginImplFrame) {
  SetUpSchedulerWithNoLayerTreeFrameSink(EXTERNAL_BFS);
  scheduler_->SetVisible(true);
  scheduler_->SetCanDraw(true);

  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  EXPECT_NO_ACTION();
}

TEST_F(SchedulerTest, Stop) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // No scheduled actions are performed after Stop. WillBeginImplFrame is only
  // a notification and not an action performed by the scheduler.
  scheduler_->Stop();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  client_->Reset();
}

TEST_F(SchedulerTest, VideoNeedsBeginFrames) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetVideoNeedsBeginFrames(true);
  EXPECT_ACTIONS("AddObserver(this)");
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  // WillBeginImplFrame is responsible for sending BeginFrames to video.
  EXPECT_ACTIONS("WillBeginImplFrame");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");

  client_->Reset();
  scheduler_->SetVideoNeedsBeginFrames(false);
  EXPECT_NO_ACTION();

  client_->Reset();
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("RemoveObserver(this)");
  EXPECT_FALSE(scheduler_->begin_frames_expected());
}

TEST_F(SchedulerTest, RequestCommit) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // If we don't swap on the deadline, we wait for the next BeginFrame.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // NotifyReadyToCommit should trigger the commit.
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // NotifyReadyToActivate should trigger the activation.
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // BeginImplFrame should prepare the draw.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // BeginImplFrame deadline should draw.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // The following BeginImplFrame deadline should SetNeedsBeginFrame(false)
  // to avoid excessive toggles.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("RemoveObserver(this)");
  client_->Reset();
}

TEST_F(SchedulerTest, RequestCommitAfterSetDeferCommit) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetDeferCommits(true);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();

  client_->Reset();
  task_runner().RunPendingTasks();
  // There are no pending tasks or actions.
  EXPECT_NO_ACTION();
  EXPECT_FALSE(scheduler_->begin_frames_expected());

  client_->Reset();
  scheduler_->SetDeferCommits(false);
  EXPECT_ACTIONS("AddObserver(this)");

  // Start new BeginMainFrame after defer commit is off.
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
}

TEST_F(SchedulerTest, DeferCommitWithRedraw) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetDeferCommits(true);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();

  // The SetNeedsRedraw will override the SetDeferCommits(true), to allow a
  // begin frame to be needed.
  client_->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  AdvanceFrame();
  // BeginMainFrame is not sent during the defer commit is on.
  EXPECT_ACTIONS("WillBeginImplFrame");

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  client_->Reset();
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
}

TEST_F(SchedulerTest, RequestCommitAfterBeginMainFrameSent) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // Now SetNeedsBeginMainFrame again. Calling here means we need a second
  // commit.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();
  client_->Reset();

  // Finish the first commit.
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Activate it.
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Because we just swapped, the Scheduler should also request the next
  // BeginImplFrame from the LayerTreeFrameSink.
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();
  // Since another commit is needed, the next BeginImplFrame should initiate
  // the second commit.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Finishing the commit before the deadline should post a new deadline task
  // to trigger the deadline early.
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // On the next BeginImplFrame, verify we go back to a quiescent state and
  // no longer request BeginImplFrames.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(scheduler_->begin_frames_expected());
  client_->Reset();
}

class SchedulerClientThatsetNeedsDrawInsideDraw : public FakeSchedulerClient {
 public:
  SchedulerClientThatsetNeedsDrawInsideDraw()
      : FakeSchedulerClient(), request_redraws_(false) {}

  void SetRequestRedrawsInsideDraw(bool enable) { request_redraws_ = enable; }

  DrawResult ScheduledActionDrawIfPossible() override {
    // Only SetNeedsRedraw the first time this is called
    if (request_redraws_) {
      scheduler_->SetNeedsRedraw();
    }
    return FakeSchedulerClient::ScheduledActionDrawIfPossible();
  }

  DrawResult ScheduledActionDrawForced() override {
    NOTREACHED();
    return DRAW_SUCCESS;
  }

 private:
  bool request_redraws_;
};

// Tests for two different situations:
// 1. the scheduler dropping SetNeedsRedraw requests that happen inside
//    a ScheduledActionDraw
// 2. the scheduler drawing twice inside a single tick
TEST_F(SchedulerTest, RequestRedrawInsideDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw* client =
      new SchedulerClientThatsetNeedsDrawInsideDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));
  client->SetRequestRedrawsInsideDraw(true);

  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());

  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  client->SetRequestRedrawsInsideDraw(false);

  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client_->num_draws());
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // We stop requesting BeginImplFrames after a BeginImplFrame where we don't
  // swap.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(client->needs_begin_frames());
}

// Test that requesting redraw inside a failed draw doesn't lose the request.
TEST_F(SchedulerTest, RequestRedrawInsideFailedDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw* client =
      new SchedulerClientThatsetNeedsDrawInsideDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  client->SetRequestRedrawsInsideDraw(true);
  client->SetDrawWillHappen(false);

  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());

  // Fail the draw.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());

  // We have a commit pending and the draw failed, and we didn't lose the redraw
  // request.
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  client->SetRequestRedrawsInsideDraw(false);

  // Fail the draw again.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // Draw successfully.
  client->SetDrawWillHappen(true);
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(3, client->num_draws());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
}

class SchedulerClientThatSetNeedsBeginMainFrameInsideDraw
    : public FakeSchedulerClient {
 public:
  SchedulerClientThatSetNeedsBeginMainFrameInsideDraw()
      : set_needs_commit_on_next_draw_(false) {}

  DrawResult ScheduledActionDrawIfPossible() override {
    // Only SetNeedsBeginMainFrame the first time this is called
    if (set_needs_commit_on_next_draw_) {
      scheduler_->SetNeedsBeginMainFrame();
      set_needs_commit_on_next_draw_ = false;
    }
    return FakeSchedulerClient::ScheduledActionDrawIfPossible();
  }

  DrawResult ScheduledActionDrawForced() override {
    NOTREACHED();
    return DRAW_SUCCESS;
  }

  void SetNeedsBeginMainFrameOnNextDraw() {
    set_needs_commit_on_next_draw_ = true;
  }

 private:
  bool set_needs_commit_on_next_draw_;
};

// Tests for the scheduler infinite-looping on SetNeedsBeginMainFrame requests
// that happen inside a ScheduledActionDraw
TEST_F(SchedulerTest, RequestCommitInsideDraw) {
  SchedulerClientThatSetNeedsBeginMainFrameInsideDraw* client =
      new SchedulerClientThatSetNeedsBeginMainFrameInsideDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  EXPECT_FALSE(client->needs_begin_frames());
  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_EQ(0, client->num_draws());
  EXPECT_TRUE(client->needs_begin_frames());

  client->SetNeedsBeginMainFrameOnNextDraw();
  EXPECT_SCOPED(AdvanceFrame());
  client->SetNeedsBeginMainFrameOnNextDraw();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(client->needs_begin_frames());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();

  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());

  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->CommitPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // We stop requesting BeginImplFrames after a BeginImplFrame where we don't
  // swap.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->CommitPending());
  EXPECT_FALSE(client->needs_begin_frames());
}

// Tests that when a draw fails then the pending commit should not be dropped.
TEST_F(SchedulerTest, RequestCommitInsideFailedDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw* client =
      new SchedulerClientThatsetNeedsDrawInsideDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  client->SetDrawWillHappen(false);

  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());

  // Fail the draw.
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());

  // We have a commit pending and the draw failed, and we didn't lose the commit
  // request.
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // Fail the draw again.
  EXPECT_SCOPED(AdvanceFrame());

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // Draw successfully.
  client->SetDrawWillHappen(true);
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(3, client->num_draws());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
}

TEST_F(SchedulerTest, NoSwapWhenDrawFails) {
  SchedulerClientThatSetNeedsBeginMainFrameInsideDraw* client =
      new SchedulerClientThatSetNeedsBeginMainFrameInsideDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());

  // Draw successfully, this starts a new frame.
  client->SetNeedsBeginMainFrameOnNextDraw();
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());

  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(client->needs_begin_frames());

  // Fail to draw, this should not start a frame.
  client->SetDrawWillHappen(false);
  client->SetNeedsBeginMainFrameOnNextDraw();
  EXPECT_SCOPED(AdvanceFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client->num_draws());
}

class SchedulerClientNeedsPrepareTilesInDraw : public FakeSchedulerClient {
 public:
  DrawResult ScheduledActionDrawIfPossible() override {
    scheduler_->SetNeedsPrepareTiles();
    return FakeSchedulerClient::ScheduledActionDrawIfPossible();
  }
};

// Test prepare tiles is independant of draws.
TEST_F(SchedulerTest, PrepareTiles) {
  SchedulerClientNeedsPrepareTilesInDraw* client =
      new SchedulerClientNeedsPrepareTilesInDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  // Request both draw and prepare tiles. PrepareTiles shouldn't
  // be trigged until BeginImplFrame.
  client->Reset();
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_TRUE(scheduler_->PrepareTilesPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());
  EXPECT_FALSE(client->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(client->HasAction("ScheduledActionDrawIfPossible"));

  // We have no immediate actions to perform, so the BeginImplFrame should post
  // the deadline task.
  client->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  // On the deadline, the actions should have occured in the right order.
  client->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());
  EXPECT_TRUE(client->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(client->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client->ActionIndex("ScheduledActionDrawIfPossible"),
            client->ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Request a draw. We don't need a PrepareTiles yet.
  client->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_TRUE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_EQ(0, client->num_draws());

  // We have no immediate actions to perform, so the BeginImplFrame should post
  // the deadline task.
  client->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  // Draw. The draw will trigger SetNeedsPrepareTiles, and
  // then the PrepareTiles action will be triggered after the Draw.
  // Afterwards, neither a draw nor PrepareTiles are pending.
  client->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client->num_draws());
  EXPECT_TRUE(client->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(client->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client->ActionIndex("ScheduledActionDrawIfPossible"),
            client->ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // We need a BeginImplFrame where we don't swap to go idle.
  client->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_EQ(0, client->num_draws());

  // Now trigger a PrepareTiles outside of a draw. We will then need
  // a begin-frame for the PrepareTiles, but we don't need a draw.
  client->Reset();
  EXPECT_FALSE(client->needs_begin_frames());
  scheduler_->SetNeedsPrepareTiles();
  EXPECT_TRUE(client->needs_begin_frames());
  EXPECT_TRUE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(scheduler_->RedrawPending());

  // BeginImplFrame. There will be no draw, only PrepareTiles.
  client->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(0, client->num_draws());
  EXPECT_FALSE(client->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(client->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
}

// Test that PrepareTiles only happens once per frame.  If an external caller
// initiates it, then the state machine should not PrepareTiles on that frame.
TEST_F(SchedulerTest, PrepareTilesOncePerFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  // If DidPrepareTiles during a frame, then PrepareTiles should not occur
  // again.
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  EXPECT_TRUE(scheduler_->PrepareTilesPending());
  scheduler_->WillPrepareTiles();
  scheduler_->DidPrepareTiles();  // An explicit PrepareTiles.
  EXPECT_FALSE(scheduler_->PrepareTilesPending());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_FALSE(client_->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Next frame without DidPrepareTiles should PrepareTiles with draw.
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(client_->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client_->ActionIndex("ScheduledActionDrawIfPossible"),
            client_->ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // If we get another DidPrepareTiles within the same frame, we should
  // not PrepareTiles on the next frame.
  scheduler_->WillPrepareTiles();
  scheduler_->DidPrepareTiles();  // An explicit PrepareTiles.
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  EXPECT_TRUE(scheduler_->PrepareTilesPending());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_FALSE(client_->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // If we get another DidPrepareTiles, we should not PrepareTiles on the next
  // frame. This verifies we don't alternate calling PrepareTiles once and
  // twice.
  EXPECT_TRUE(scheduler_->PrepareTilesPending());
  scheduler_->WillPrepareTiles();
  scheduler_->DidPrepareTiles();  // An explicit PrepareTiles.
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  EXPECT_TRUE(scheduler_->PrepareTilesPending());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_FALSE(client_->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Next frame without DidPrepareTiles should PrepareTiles with draw.
  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(client_->HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client_->ActionIndex("ScheduledActionDrawIfPossible"),
            client_->ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler_->RedrawPending());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
}

TEST_F(SchedulerTest, DidPrepareTilesPreventsPrepareTilesForOneFrame) {
  std::unique_ptr<SchedulerClientNeedsPrepareTilesInDraw> client =
      base::WrapUnique(new SchedulerClientNeedsPrepareTilesInDraw);
  SetUpScheduler(EXTERNAL_BFS, std::move(client));

  client_->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 "ScheduledActionPrepareTiles");

  // We don't want to hinder scheduled prepare tiles for more than one frame
  // even if we call unscheduled prepare tiles many times.
  for (int i = 0; i < 10; i++) {
    scheduler_->WillPrepareTiles();
    scheduler_->DidPrepareTiles();
  }

  client_->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  // No scheduled prepare tiles because we've already counted a prepare tiles in
  // between frames.
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");

  client_->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  // Resume scheduled prepare tiles.
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 "ScheduledActionPrepareTiles");
}

TEST_F(SchedulerTest, TriggerBeginFrameDeadlineEarly) {
  SchedulerClientNeedsPrepareTilesInDraw* client =
      new SchedulerClientNeedsPrepareTilesInDraw;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());

  // The deadline should be zero since there is no work other than drawing
  // pending.
  EXPECT_EQ(base::TimeTicks(), client->posted_begin_impl_frame_deadline());
}

TEST_F(SchedulerTest, WaitForReadyToDrawDoNotPostDeadline) {
  SchedulerClientNeedsPrepareTilesInDraw* client =
      new SchedulerClientNeedsPrepareTilesInDraw;
  scheduler_settings_.commit_to_active_tree = true;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Begin new frame.
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  client_->Reset();
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");

  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  // Scheduler won't post deadline in the mode.
  client_->Reset();
  task_runner().RunPendingTasks();  // Try to run posted deadline.
  // There is no posted deadline.
  EXPECT_NO_ACTION();

  // Scheduler received ready to draw signal, and posted deadline.
  scheduler_->NotifyReadyToDraw();
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client_->num_draws());
  EXPECT_TRUE(client_->HasAction("ScheduledActionDrawIfPossible"));
}

TEST_F(SchedulerTest, WaitForReadyToDrawCancelledWhenLostLayerTreeFrameSink) {
  SchedulerClientNeedsPrepareTilesInDraw* client =
      new SchedulerClientNeedsPrepareTilesInDraw;
  scheduler_settings_.commit_to_active_tree = true;
  SetUpScheduler(EXTERNAL_BFS, base::WrapUnique(client));

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Begin new frame.
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  client_->Reset();
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");

  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  // Scheduler won't post deadline in the mode.
  client_->Reset();
  task_runner().RunPendingTasks();  // Try to run posted deadline.
  // There is no posted deadline.
  EXPECT_NO_ACTION();

  // Scheduler loses LayerTreeFrameSink, and stops waiting for ready to draw
  // signal.
  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");
}

void SchedulerTest::AdvanceAndMissOneFrame() {
  // Impl thread hits deadline before commit finishes.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame", "ScheduledActionCommit",
                 "ScheduledActionActivateSyncTree");
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  client_->Reset();
}

void SchedulerTest::CheckMainFrameSkippedAfterLateCommit(
    bool expect_send_begin_main_frame) {
  AdvanceAndMissOneFrame();

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_EQ(expect_send_begin_main_frame,
            scheduler_->MainThreadMissedLastDeadline());
  EXPECT_TRUE(client_->HasAction("WillBeginImplFrame"));
  EXPECT_EQ(expect_send_begin_main_frame,
            client_->HasAction("ScheduledActionSendBeginMainFrame"));
}

TEST_F(SchedulerTest, MainFrameNotSkippedAfterLateBeginFrame) {
  // If a begin frame is delivered extremely late (because the browser has
  // some contention), make sure that the main frame is not skipped even
  // if it can activate before the deadline.
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  AdvanceAndMissOneFrame();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->SetNeedsBeginMainFrame();

  // Advance frame and create a begin frame.
  now_src_->Advance(viz::BeginFrameArgs::DefaultInterval());
  viz::BeginFrameArgs args =
      fake_external_begin_frame_source_->CreateBeginFrameArgs(
          BEGINFRAME_FROM_HERE, now_src());

  // Deliver this begin frame super late.
  now_src_->Advance(viz::BeginFrameArgs::DefaultInterval() * 100);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);

  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_EQ(true, scheduler_->MainThreadMissedLastDeadline());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionDrawIfPossible");
}

TEST_F(SchedulerTest, MainFrameSkippedAfterLateCommit) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  bool expect_send_begin_main_frame = false;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

// Response times of BeginMainFrame's without the critical path flag set
// should not affect whether we recover latency or not.
TEST_F(SchedulerTest,
       MainFrameSkippedAfterLateCommit_LongMainFrameQueueDurationNotCritical) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationNotCriticalEstimate(kSlowDuration);

  bool expect_send_begin_main_frame = false;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

// Response times of BeginMainFrame's with the critical path flag set
// should affect whether we recover latency or not.
TEST_F(SchedulerTest,
       MainFrameNotSkippedAfterLateCommit_LongMainFrameQueueDurationCritical) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationCriticalEstimate(kSlowDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationNotCriticalEstimate(kSlowDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

TEST_F(SchedulerTest,
       MainFrameNotSkippedAfterLateCommitInPreferImplLatencyMode) {
  SetUpScheduler(EXTERNAL_BFS);
  scheduler_->SetTreePrioritiesAndScrollState(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

TEST_F(SchedulerTest,
       MainFrameNotSkippedAfterLateCommit_CommitEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameStartToReadyToCommitDurationEstimate(kSlowDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

TEST_F(SchedulerTest,
       MainFrameNotSkippedAfterLateCommit_ReadyToActivateEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetCommitToReadyToActivateDurationEstimate(
      kSlowDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

TEST_F(SchedulerTest,
       MainFrameNotSkippedAfterLateCommit_ActivateEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetActivateDurationEstimate(kSlowDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

TEST_F(SchedulerTest, MainFrameNotSkippedAfterLateCommit_DrawEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetDrawDurationEstimate(kSlowDuration);

  bool expect_send_begin_main_frame = true;
  EXPECT_SCOPED(
      CheckMainFrameSkippedAfterLateCommit(expect_send_begin_main_frame));
}

// If the BeginMainFrame aborts, it doesn't actually insert a frame into the
// queue, which means there is no latency to recover.
TEST_F(SchedulerTest, MainFrameNotSkippedAfterLateBeginMainFrameAbort) {
  SetUpScheduler(EXTERNAL_BFS);

  // Use fast estimates so we think we can recover latency if needed.
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  // Impl thread hits deadline before BeginMainFrame aborts.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame");
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());

  // After aborting the frame, make sure we don't skip the
  // next BeginMainFrame.
  client_->Reset();
  scheduler_->BeginMainFrameAborted(CommitEarlyOutReason::FINISHED_NO_UPDATES);
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(client_->HasAction("WillBeginImplFrame"));
  EXPECT_TRUE(client_->HasAction("ScheduledActionSendBeginMainFrame"));
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
}

// If the BeginMainFrame aborts, it doesn't actually insert a frame into the
// queue, which means there is no latency to recover.
TEST_F(SchedulerTest, MainFrameNotSkippedAfterCanDrawChanges) {
  SetUpScheduler(EXTERNAL_BFS);

  // Use fast estimates so we think we can recover latency if needed.
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  // Impl thread hits deadline before BeginMainFrame aborts.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame");
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());

  // Make us abort the upcoming draw.
  client_->Reset();
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree");
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->SetCanDraw(false);
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

  // Make CanDraw true after activation.
  client_->Reset();
  scheduler_->SetCanDraw(true);
  EXPECT_NO_ACTION();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

  // Make sure we don't skip the next BeginMainFrame.
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(client_->HasAction("WillBeginImplFrame"));
  EXPECT_TRUE(client_->HasAction("ScheduledActionSendBeginMainFrame"));
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
}

void SchedulerTest::ImplFrameSkippedAfterLateAck(
    bool receive_ack_before_deadline) {
  // To get into a high latency state, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Draw and swap for first BeginFrame
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->SetNeedsRedraw();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  SendNextBeginFrame();
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame");

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionDrawIfPossible");

  // Verify we skip every other frame if the swap ack consistently
  // comes back late.
  for (int i = 0; i < 10; i++) {
    // Not calling scheduler_->DidReceiveCompositorFrameAck() until after next
    // BeginImplFrame puts the impl thread in high latency mode.
    client_->Reset();
    scheduler_->SetNeedsBeginMainFrame();
    scheduler_->SetNeedsRedraw();
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
    SendNextBeginFrame();
    // Verify that we skip the BeginImplFrame
    EXPECT_NO_ACTION();
    EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

    // Verify that we do not perform any actions after we are no longer
    // swap throttled.
    client_->Reset();
    if (receive_ack_before_deadline) {
      // It shouldn't matter if the swap ack comes back before the deadline...
      scheduler_->DidReceiveCompositorFrameAck();
      task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
    } else {
      // ... or after the deadline.
      task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
      scheduler_->DidReceiveCompositorFrameAck();
    }
    EXPECT_NO_ACTION();

    // Verify that we start the next BeginImplFrame and continue normally
    // after having just skipped a BeginImplFrame.
    client_->Reset();
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
    SendNextBeginFrame();
    EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

    client_->Reset();
    scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
    scheduler_->NotifyReadyToCommit();
    scheduler_->NotifyReadyToActivate();
    task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
    EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                   "ScheduledActionDrawIfPossible");
  }
}

TEST_F(SchedulerTest,
       ImplFrameSkippedAfterLateAck_FastEstimates_SubmitAckThenDeadline) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  bool receive_ack_before_deadline = true;
  EXPECT_SCOPED(ImplFrameSkippedAfterLateAck(receive_ack_before_deadline));
}

TEST_F(SchedulerTest,
       ImplFrameSkippedAfterLateAck_FastEstimates_DeadlineThenSubmitAck) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  bool receive_ack_before_deadline = false;
  EXPECT_SCOPED(ImplFrameSkippedAfterLateAck(receive_ack_before_deadline));
}

TEST_F(SchedulerTest,
       ImplFrameSkippedAfterLateAck_LongMainFrameQueueDurationNotCritical) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationNotCriticalEstimate(kSlowDuration);

  bool receive_ack_before_deadline = false;
  EXPECT_SCOPED(ImplFrameSkippedAfterLateAck(receive_ack_before_deadline));
}

TEST_F(SchedulerTest, ImplFrameSkippedAfterLateAck_ImplLatencyTakesPriority) {
  SetUpScheduler(EXTERNAL_BFS);

  // Even if every estimate related to the main thread is slow, we should
  // still expect to recover impl thread latency if the draw is fast and we
  // are in impl latency takes priority.
  scheduler_->SetTreePrioritiesAndScrollState(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER);
  fake_compositor_timing_history_->SetAllEstimatesTo(kSlowDuration);
  fake_compositor_timing_history_->SetDrawDurationEstimate(kFastDuration);

  bool receive_ack_before_deadline = false;
  EXPECT_SCOPED(ImplFrameSkippedAfterLateAck(receive_ack_before_deadline));
}

TEST_F(SchedulerTest,
       ImplFrameSkippedAfterLateAck_OnlyImplSideUpdatesExpected) {
  // This tests that we recover impl thread latency when there are no commits.
  SetUpScheduler(EXTERNAL_BFS);

  // To get into a high latency state, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Even if every estimate related to the main thread is slow, we should
  // still expect to recover impl thread latency if there are no commits from
  // the main thread.
  fake_compositor_timing_history_->SetAllEstimatesTo(kSlowDuration);
  fake_compositor_timing_history_->SetDrawDurationEstimate(kFastDuration);

  // Draw and swap for first BeginFrame
  client_->Reset();
  scheduler_->SetNeedsRedraw();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  SendNextBeginFrame();
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame");

  client_->Reset();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");

  // Verify we skip every other frame if the swap ack consistently
  // comes back late.
  for (int i = 0; i < 10; i++) {
    // Not calling scheduler_->DidReceiveCompositorFrameAck() until after next
    // BeginImplFrame puts the impl thread in high latency mode.
    client_->Reset();
    scheduler_->SetNeedsRedraw();
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
    SendNextBeginFrame();
    // Verify that we skip the BeginImplFrame
    EXPECT_NO_ACTION();
    EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

    // Verify that we do not perform any actions after we are no longer
    // swap throttled.
    client_->Reset();
    scheduler_->DidReceiveCompositorFrameAck();
    EXPECT_NO_ACTION();

    // Verify that we start the next BeginImplFrame and continue normally
    // after having just skipped a BeginImplFrame.
    client_->Reset();
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
    SendNextBeginFrame();
    EXPECT_ACTIONS("WillBeginImplFrame");

    client_->Reset();
    // Deadline should be immediate.
    EXPECT_TRUE(client_->IsInsideBeginImplFrame());
    task_runner().RunUntilTime(now_src_->NowTicks());
    EXPECT_FALSE(client_->IsInsideBeginImplFrame());
    EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  }
}

void SchedulerTest::ImplFrameNotSkippedAfterLateAck() {
  // To get into a high latency state, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Draw and swap for first BeginFrame
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  SendNextBeginFrame();
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame");

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionDrawIfPossible");

  // Verify impl thread consistently operates in high latency mode
  // without skipping any frames.
  for (int i = 0; i < 10; i++) {
    // Not calling scheduler_->DidReceiveCompositorFrameAck() until after next
    // frame
    // puts the impl thread in high latency mode.
    client_->Reset();
    scheduler_->SetNeedsBeginMainFrame();
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
    SendNextBeginFrame();
    EXPECT_ACTIONS("WillBeginImplFrame");
    EXPECT_TRUE(client_->IsInsideBeginImplFrame());
    EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

    client_->Reset();
    scheduler_->DidReceiveCompositorFrameAck();
    scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
    scheduler_->NotifyReadyToCommit();
    scheduler_->NotifyReadyToActivate();
    task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));

    // Verify that we don't skip the actions of the BeginImplFrame
    EXPECT_ACTIONS("ScheduledActionSendBeginMainFrame", "ScheduledActionCommit",
                   "ScheduledActionActivateSyncTree",
                   "ScheduledActionDrawIfPossible");
  }
}

TEST_F(SchedulerTest,
       ImplFrameNotSkippedAfterLateAck_MainFrameQueueDurationCriticalTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationCriticalEstimate(kSlowDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameQueueDurationNotCriticalEstimate(kSlowDuration);
  EXPECT_SCOPED(ImplFrameNotSkippedAfterLateAck());
}

TEST_F(SchedulerTest, ImplFrameNotSkippedAfterLateAck_CommitEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_
      ->SetBeginMainFrameStartToReadyToCommitDurationEstimate(kSlowDuration);
  EXPECT_SCOPED(ImplFrameNotSkippedAfterLateAck());
}

TEST_F(SchedulerTest,
       ImplFrameNotSkippedAfterLateAck_ReadyToActivateEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetCommitToReadyToActivateDurationEstimate(
      kSlowDuration);
  EXPECT_SCOPED(ImplFrameNotSkippedAfterLateAck());
}

TEST_F(SchedulerTest, ImplFrameNotSkippedAfterLateAck_ActivateEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetActivateDurationEstimate(kSlowDuration);
  EXPECT_SCOPED(ImplFrameNotSkippedAfterLateAck());
}

TEST_F(SchedulerTest, ImplFrameNotSkippedAfterLateAck_DrawEstimateTooLong) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetDrawDurationEstimate(kSlowDuration);
  EXPECT_SCOPED(ImplFrameNotSkippedAfterLateAck());
}

TEST_F(SchedulerTest, MainFrameThenImplFrameSkippedAfterLateCommitAndLateAck) {
  // Set up client with custom estimates.
  // This test starts off with expensive estimates to prevent latency recovery
  // initially, then lowers the estimates to enable it once both the main
  // and impl threads are in a high latency mode.
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(kSlowDuration);

  // To get into a high latency state, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Impl thread hits deadline before commit finishes to make
  // MainThreadMissedLastDeadline true
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());

  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame", "ScheduledActionCommit",
                 "ScheduledActionActivateSyncTree");

  // Draw and swap for first commit, start second commit.
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();

  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionDrawIfPossible", "ScheduledActionCommit",
                 "ScheduledActionActivateSyncTree");

  // Don't call scheduler_->DidReceiveCompositorFrameAck() until after next
  // frame
  // to put the impl thread in a high latency mode.
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));

  EXPECT_ACTIONS("WillBeginImplFrame");
  // Note: BeginMainFrame and swap are skipped here because of
  // swap ack backpressure, not because of latency recovery.
  EXPECT_FALSE(client_->HasAction("ScheduledActionSendBeginMainFrame"));
  EXPECT_FALSE(client_->HasAction("ScheduledActionDrawIfPossible"));
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());

  // Lower estimates so that the scheduler will attempt latency recovery.
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  // Now that both threads are in a high latency mode, make sure we
  // skip the BeginMainFrame, then the BeginImplFrame, but not both
  // at the same time.

  // Verify we skip BeginMainFrame first.
  client_->Reset();
  // Previous commit request is still outstanding.
  EXPECT_TRUE(scheduler_->NeedsBeginMainFrame());
  EXPECT_TRUE(scheduler_->IsDrawThrottled());
  SendNextBeginFrame();
  EXPECT_TRUE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->DidReceiveCompositorFrameAck();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));

  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionDrawIfPossible");

  // Verify we skip the BeginImplFrame second.
  client_->Reset();
  // Previous commit request is still outstanding.
  EXPECT_TRUE(scheduler_->NeedsBeginMainFrame());
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  SendNextBeginFrame();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->DidReceiveCompositorFrameAck();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

  EXPECT_NO_ACTION();

  // Then verify we operate in a low latency mode.
  client_->Reset();
  // Previous commit request is still outstanding.
  EXPECT_TRUE(scheduler_->NeedsBeginMainFrame());
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  SendNextBeginFrame();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());
  scheduler_->DidReceiveCompositorFrameAck();
  EXPECT_FALSE(scheduler_->MainThreadMissedLastDeadline());

  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionDrawIfPossible");
}

TEST_F(
    SchedulerTest,
    Deadlock_CommitMakesProgressWhileSwapTrottledAndActiveTreeNeedsFirstDraw) {
  // NPAPI plugins on Windows block the Browser UI thread on the Renderer main
  // thread. This prevents the scheduler from receiving any pending swap acks.

  scheduler_settings_.main_frame_while_submit_frame_throttled_enabled = true;
  SetUpScheduler(EXTERNAL_BFS);

  // Disables automatic swap acks so this test can force swap ack throttling
  // to simulate a blocked Browser ui thread.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Get a new active tree in main-thread high latency mode and put us
  // in a swap throttled state.
  client_->Reset();
  EXPECT_FALSE(scheduler_->CommitPending());
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_FALSE(scheduler_->CommitPending());
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionDrawIfPossible", "ScheduledActionCommit",
                 "ScheduledActionActivateSyncTree");

  // Make sure that we can finish the next commit even while swap throttled.
  client_->Reset();
  EXPECT_FALSE(scheduler_->CommitPending());
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionCommit");

  // Make sure we do not send a BeginMainFrame while swap throttled and
  // we have both a pending tree and an active tree.
  client_->Reset();
  EXPECT_FALSE(scheduler_->CommitPending());
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_FALSE(scheduler_->CommitPending());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("WillBeginImplFrame");
}

TEST_F(
    SchedulerTest,
    CommitMakesProgressWhenIdleAndHasPendingTreeAndActiveTreeNeedsFirstDraw) {
  // This verifies we don't block commits longer than we need to
  // for performance reasons - not deadlock reasons.

  // Since we are simulating a long commit, set up a client with draw duration
  // estimates that prevent skipping main frames to get to low latency mode.
  scheduler_settings_.main_frame_while_submit_frame_throttled_enabled = true;
  scheduler_settings_.main_frame_before_activation_enabled = true;
  SetUpScheduler(EXTERNAL_BFS);

  // Disables automatic swap acks so this test can force swap ack throttling
  // to simulate a blocked Browser ui thread.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // Start a new commit in main-thread high latency mode and hold off on
  // activation.
  client_->Reset();
  EXPECT_FALSE(scheduler_->CommitPending());
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  scheduler_->DidReceiveCompositorFrameAck();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_FALSE(scheduler_->CommitPending());
  EXPECT_ACTIONS("AddObserver(this)", "WillBeginImplFrame",
                 "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionDrawIfPossible", "ScheduledActionCommit");

  // Start another commit while we still have an active tree.
  client_->Reset();
  EXPECT_FALSE(scheduler_->CommitPending());
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->CommitPending());
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  scheduler_->DidReceiveCompositorFrameAck();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionDrawIfPossible");

  // Can't commit yet because there's still a pending tree.
  client_->Reset();
  scheduler_->NotifyReadyToCommit();
  EXPECT_NO_ACTION();

  // Activate the pending tree, which also unblocks the commit immediately
  // while we are in an idle state.
  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree", "ScheduledActionCommit");
}

void SchedulerTest::BeginFramesNotFromClient(BeginFrameSourceType bfs_type) {
  SetUpScheduler(bfs_type);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame
  // without calling SetNeedsBeginFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Can't run the deadline task because it can race with begin frame for the
  // SyntheticBFS case.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // NotifyReadyToCommit should trigger the commit.
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();

  // NotifyReadyToActivate should trigger the activation.
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  client_->Reset();

  // BeginImplFrame deadline should draw. The following BeginImplFrame deadline
  // should SetNeedsBeginFrame(false) to avoid excessive toggles.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible", "WillBeginImplFrame");
  client_->Reset();

  // Make sure SetNeedsBeginFrame isn't called on the client
  // when the BeginFrame is no longer needed.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION();
  client_->Reset();
}

TEST_F(SchedulerTest, SyntheticBeginFrames) {
  BeginFramesNotFromClient(THROTTLED_BFS);
}

TEST_F(SchedulerTest, UnthrottledBeginFrames) {
  BeginFramesNotFromClient(UNTHROTTLED_BFS);
}

void SchedulerTest::BeginFramesNotFromClient_IsDrawThrottled(
    BeginFrameSourceType bfs_type) {
  SetUpScheduler(bfs_type);

  // Set the draw duration estimate to zero so that deadlines are accurate.
  fake_compositor_timing_history_->SetDrawDurationEstimate(base::TimeDelta());

  // To test swap ack throttling, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();
  client_->Reset();

  // Trigger the first BeginImplFrame and BeginMainFrame
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // NotifyReadyToCommit should trigger the pending commit.
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();

  // NotifyReadyToActivate should trigger the activation and draw.
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  client_->Reset();

  // Swapping will put us into a swap throttled state.
  // Run posted deadline.
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // While swap throttled, BeginFrames should trigger BeginImplFrames,
  // but not a BeginMainFrame or draw.
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->SetNeedsRedraw();
  EXPECT_SCOPED(AdvanceFrame());  // Run posted BeginFrame.
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  base::TimeTicks before_deadline, after_deadline;

  // The deadline is set to the regular deadline.
  before_deadline = now_src()->NowTicks();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  after_deadline = now_src()->NowTicks();
  // We can't do an equality comparison here because the scheduler uses a fudge
  // factor that's an internal implementation detail.
  EXPECT_GT(after_deadline, before_deadline);
  EXPECT_LT(after_deadline,
            before_deadline + viz::BeginFrameArgs::DefaultInterval());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());  // Run posted BeginFrame.
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Take us out of a swap throttled state.
  scheduler_->DidReceiveCompositorFrameAck();
  EXPECT_ACTIONS("ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // The deadline is set to the regular deadline.
  before_deadline = now_src()->NowTicks();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  after_deadline = now_src()->NowTicks();
  // We can't do an equality comparison here because the scheduler uses a fudge
  // factor that's an internal implementation detail.
  EXPECT_GT(after_deadline, before_deadline);
  EXPECT_LT(after_deadline,
            before_deadline + viz::BeginFrameArgs::DefaultInterval());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

TEST_F(SchedulerTest, SyntheticBeginFrames_IsDrawThrottled) {
  BeginFramesNotFromClient_IsDrawThrottled(THROTTLED_BFS);
}

TEST_F(SchedulerTest, UnthrottledBeginFrames_IsDrawThrottled) {
  BeginFramesNotFromClient_IsDrawThrottled(UNTHROTTLED_BFS);
}

TEST_F(SchedulerTest,
       DidLoseLayerTreeFrameSinkAfterLayerTreeFrameSinkIsInitialized) {
  SetUpSchedulerWithNoLayerTreeFrameSink(EXTERNAL_BFS);

  scheduler_->SetVisible(true);
  scheduler_->SetCanDraw(true);

  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  EXPECT_NO_ACTION();

  scheduler_->DidLoseLayerTreeFrameSink();
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
}

TEST_F(SchedulerTest, DidLoseLayerTreeFrameSinkAfterBeginFrameStarted) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  // RemoveObserver(this) is not called until the end of the frame.
  EXPECT_NO_ACTION();

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree");

  client_->Reset();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");
}

TEST_F(SchedulerTest,
       DidLoseLayerTreeFrameSinkAfterBeginFrameStartedWithHighLatency) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  // Do nothing when impl frame is in deadine pending state.
  EXPECT_NO_ACTION();

  client_->Reset();
  // Run posted deadline.
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  // OnBeginImplFrameDeadline didn't schedule LayerTreeFrameSink creation
  // because
  // main frame is not yet completed.
  EXPECT_ACTIONS("RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // BeginImplFrame is not started.
  client_->Reset();
  task_runner().RunUntilTime(now_src()->NowTicks() +
                             base::TimeDelta::FromMilliseconds(10));
  EXPECT_NO_ACTION();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionBeginLayerTreeFrameSinkCreation");
}

TEST_F(SchedulerTest, DidLoseLayerTreeFrameSinkAfterReadyToCommit) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");

  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  // Sync tree should be forced to activate.
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  // RemoveObserver(this) is not called until the end of the frame.
  client_->Reset();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");
}

TEST_F(SchedulerTest, DidLoseLayerTreeFrameSinkAfterSetNeedsPrepareTiles) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsPrepareTiles();
  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  // RemoveObserver(this) is not called until the end of the frame.
  EXPECT_NO_ACTION();

  client_->Reset();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionPrepareTiles",
                 "ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");
}

TEST_F(SchedulerTest, DidLoseLayerTreeFrameSinkWithDelayBasedBeginFrameSource) {
  SetUpScheduler(THROTTLED_BFS);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  EXPECT_FALSE(scheduler_->begin_frames_expected());
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  client_->Reset();
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // NotifyReadyToCommit should trigger the commit.
  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // NotifyReadyToActivate should trigger the activation.
  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  // RemoveObserver(this) is not called until the end of the frame.
  EXPECT_NO_ACTION();
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  client_->Reset();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  EXPECT_FALSE(scheduler_->begin_frames_expected());
}

TEST_F(SchedulerTest, DidLoseLayerTreeFrameSinkWhenIdle) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");

  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  client_->Reset();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");

  // Idle time between BeginFrames.
  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");
}

TEST_F(SchedulerTest, ScheduledActionActivateAfterBecomingInvisible) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->SetVisible(false);
  task_runner().RunPendingTasks();  // Run posted deadline.

  // Sync tree should be forced to activate.
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree", "RemoveObserver(this)");
}

TEST_F(SchedulerTest, ScheduledActionActivateAfterBeginFrameSourcePaused) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());

  client_->Reset();
  fake_external_begin_frame_source_->SetPaused(true);
  task_runner().RunPendingTasks();  // Run posted deadline.

  // Sync tree should be forced to activate.
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
}

// Tests to ensure frame sources can be successfully changed while drawing.
TEST_F(SchedulerTest, SwitchFrameSourceToUnthrottled) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsRedraw should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  scheduler_->SetNeedsRedraw();

  // Switch to an unthrottled frame source.
  scheduler_->SetBeginFrameSource(unthrottled_frame_source_.get());
  client_->Reset();

  // Unthrottled frame source will immediately begin a new frame.
  task_runner().RunPendingTasks();  // Run posted BeginFrame.
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // If we don't swap on the deadline, we wait for the next BeginFrame.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

// Tests to ensure frame sources can be successfully changed while a frame
// deadline is pending.
TEST_F(SchedulerTest, SwitchFrameSourceToUnthrottledBeforeDeadline) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsRedraw should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");

  // Switch to an unthrottled frame source before the frame deadline is hit.
  scheduler_->SetBeginFrameSource(unthrottled_frame_source_.get());
  client_->Reset();

  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 // Unthrottled frame source will immediately begin a new frame.
                 "WillBeginImplFrame");
  scheduler_->SetNeedsRedraw();
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

// Tests to ensure that the active frame source can successfully be changed from
// unthrottled to throttled.
TEST_F(SchedulerTest, SwitchFrameSourceToThrottled) {
  SetUpScheduler(UNTHROTTLED_BFS);

  scheduler_->SetNeedsRedraw();
  EXPECT_NO_ACTION();
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted BeginFrame.
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Switch to a throttled frame source.
  scheduler_->SetBeginFrameSource(fake_external_begin_frame_source_.get());
  client_->Reset();

  // SetNeedsRedraw should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsRedraw();
  task_runner().RunPendingTasks();
  EXPECT_NO_ACTION();
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
}

TEST_F(SchedulerTest, SwitchFrameSourceToNullInsideDeadline) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  client_->Reset();

  // Switch to a null frame source.
  scheduler_->SetBeginFrameSource(nullptr);
  EXPECT_ACTIONS("RemoveObserver(this)");
  client_->Reset();

  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // AdvanceFrame helper can't be used here because there's no deadline posted.
  scheduler_->SetNeedsRedraw();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_NO_ACTION();
  client_->Reset();

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_NO_ACTION();
  client_->Reset();

  // Switch back to the same source, make sure frames continue to be produced.
  scheduler_->SetBeginFrameSource(fake_external_begin_frame_source_.get());
  EXPECT_ACTIONS("AddObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  task_runner().RunPendingTasks();
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
}

// This test maskes sure that switching a frame source when not observing
// such as when not visible also works.
TEST_F(SchedulerTest, SwitchFrameSourceWhenNotObserving) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Begin new frame.
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  client_->Reset();
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");

  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  // Scheduler loses LayerTreeFrameSink, and stops waiting for ready to draw
  // signal.
  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  task_runner().RunPendingTasks();
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation",
                 "RemoveObserver(this)");

  // Changing begin frame source doesn't do anything.
  // The unthrottled source doesn't print Add/RemoveObserver like the fake one.
  client_->Reset();
  scheduler_->SetBeginFrameSource(unthrottled_frame_source_.get());
  EXPECT_NO_ACTION();

  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  EXPECT_NO_ACTION();

  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_NO_ACTION();

  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
}

// Tests to ensure that we send a ScheduledActionBeginMainFrameNotExpectedUntil
// when expected.
TEST_F(SchedulerTest, ScheduledActionBeginMainFrameNotExpectedUntil) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->SetMainThreadWantsBeginMainFrameNotExpected(true);
  task_runner().RunPendingTasks();
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionBeginMainFrameNotExpectedUntil",
                 "ScheduledActionDrawIfPossible");
}

// Tests to ensure that we send a BeginMainFrameNotExpectedSoon when expected.
TEST_F(SchedulerTest, SendBeginMainFrameNotExpectedSoon_Requested) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Trigger a frame draw.
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  task_runner().RunPendingTasks();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionDrawIfPossible");
  client_->Reset();

  scheduler_->SetMainThreadWantsBeginMainFrameNotExpected(true);

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionBeginMainFrameNotExpectedUntil");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // The BeginImplFrame deadline should SetNeedsBeginFrame(false) and send a
  // SendBeginMainFrameNotExpectedSoon.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("RemoveObserver(this)", "SendBeginMainFrameNotExpectedSoon");
  client_->Reset();
}

// Tests to ensure that we dont't send a BeginMainFrameNotExpectedSoon when
// possible but not requested.
TEST_F(SchedulerTest, SendBeginMainFrameNotExpectedSoon_Unrequested) {
  SetUpScheduler(EXTERNAL_BFS);

  // SetNeedsBeginMainFrame should begin the frame on the next BeginImplFrame.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Trigger a frame draw.
  EXPECT_SCOPED(AdvanceFrame());
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  task_runner().RunPendingTasks();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionCommit", "ScheduledActionActivateSyncTree",
                 "ScheduledActionDrawIfPossible");
  client_->Reset();

  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // The BeginImplFrame deadline should SetNeedsBeginFrame(false), but doesn't
  // send a SendBeginMainFrameNotExpectedSoon as it's not been requested by the
  // main thread.
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("RemoveObserver(this)");
  client_->Reset();

  scheduler_->SetMainThreadWantsBeginMainFrameNotExpected(true);

  EXPECT_ACTIONS("SendBeginMainFrameNotExpectedSoon");
}

TEST_F(SchedulerTest, SynchronousCompositorAnimation) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsOneBeginImplFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Testing the case where animation ticks a fling scroll.
  client_->SetWillBeginImplFrameCausesRedraw(true);
  // The animation isn't done so it'll cause another tick in the future.
  client_->SetWillBeginImplFrameRequestsOneBeginImplFrame(true);

  // Next vsync.
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Android onDraw. This doesn't consume the single begin frame request.
  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = false;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // The animation inside of WillBeginImplFrame changes stuff on the screen, but
  // ends here, so does not cause another frame to happen.
  client_->SetWillBeginImplFrameCausesRedraw(true);

  // Next vsync.
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Idle on next vsync, as the animation has completed.
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

TEST_F(SchedulerTest, SynchronousCompositorOnDrawDuringIdle) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = false;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("AddObserver(this)", "ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Idle on next vsync.
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

TEST_F(SchedulerTest, SetNeedsOneBeginImplFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  EXPECT_FALSE(scheduler_->begin_frames_expected());

  // Request a frame, should kick the source.
  scheduler_->SetNeedsOneBeginImplFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // The incoming WillBeginImplFrame will request another one.
  client_->SetWillBeginImplFrameRequestsOneBeginImplFrame(true);

  // Next vsync, the first requested frame happens.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // We don't request another frame here.

  // Next vsync, the second requested frame happens (the one requested inside
  // the previous frame's begin impl frame step).
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // End that frame's deadline.
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Scheduler shuts down the source now that no begin frame is requested.
  EXPECT_ACTIONS("RemoveObserver(this)");
}

TEST_F(SchedulerTest, SynchronousCompositorCommitAndVerifyBeginFrameAcks) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Next vsync.
  viz::BeginFrameArgs args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  bool has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();

  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_NO_ACTION();

  // Next vsync.
  args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();

  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();

  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  client_->Reset();

  // Next vsync.
  args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // No damage, since not drawn yet.
  // TODO(eseckler): In the future, |has_damage = false| will prevent us from
  // filtering this ack (in CompositorExternalBeginFrameSource) and instead
  // forwarding the one attached to the later submitted CompositorFrame.
  has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = false;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Idle on next vsync.
  args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, SynchronousCompositorDoubleCommitWithoutDraw) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Next vsync.
  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_NO_ACTION();

  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();

  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  client_->Reset();

  // Ask for another commit.
  scheduler_->SetNeedsBeginMainFrame();

  AdvanceFrame();
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  EXPECT_NO_ACTION();

  // Allow new commit even though previous commit hasn't been drawn.
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();
}

class SchedulerClientSetNeedsPrepareTilesOnDraw : public FakeSchedulerClient {
 public:
  SchedulerClientSetNeedsPrepareTilesOnDraw() : FakeSchedulerClient() {}

 protected:
  DrawResult ScheduledActionDrawIfPossible() override {
    scheduler_->SetNeedsPrepareTiles();
    return FakeSchedulerClient::ScheduledActionDrawIfPossible();
  }
};

TEST_F(SchedulerTest, SynchronousCompositorPrepareTilesOnDraw) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;

  std::unique_ptr<FakeSchedulerClient> client =
      base::WrapUnique(new SchedulerClientSetNeedsPrepareTilesOnDraw);
  SetUpScheduler(EXTERNAL_BFS, std::move(client));

  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Next vsync.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = false;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 "ScheduledActionPrepareTiles");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 "ScheduledActionPrepareTiles");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  client_->Reset();

  // Next vsync.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  EXPECT_ACTIONS("WillBeginImplFrame", "RemoveObserver(this)");
  EXPECT_FALSE(scheduler_->begin_frames_expected());
  client_->Reset();
}

TEST_F(SchedulerTest, SynchronousCompositorSendBeginMainFrameWhileIdle) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  EXPECT_ACTIONS("AddObserver(this)");
  client_->Reset();

  // Next vsync.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = false;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  client_->Reset();

  // Simulate SetNeedsBeginMainFrame due to input event.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("ScheduledActionSendBeginMainFrame");
  client_->Reset();

  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  client_->Reset();

  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");
  client_->Reset();

  // Next vsync.
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionInvalidateLayerTreeFrameSink");
  client_->Reset();

  // Android onDraw.
  scheduler_->SetNeedsRedraw();
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_FALSE(scheduler_->PrepareTilesPending());
  client_->Reset();

  // Simulate SetNeedsBeginMainFrame due to input event.
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_ACTIONS("ScheduledActionSendBeginMainFrame");
  client_->Reset();
}

TEST_F(SchedulerTest, SynchronousCompositorResourcelessOnDrawWhenInvisible) {
  scheduler_settings_.using_synchronous_renderer_compositor = true;
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetVisible(false);

  scheduler_->SetNeedsRedraw();
  bool resourceless_software_draw = true;
  scheduler_->OnDrawForLayerTreeFrameSink(resourceless_software_draw);
  // SynchronousCompositor has to draw regardless of visibility.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();
}

TEST_F(SchedulerTest, AuthoritativeVSyncInterval) {
  SetUpScheduler(THROTTLED_BFS);
  base::TimeDelta initial_interval = scheduler_->BeginImplFrameInterval();
  base::TimeDelta authoritative_interval =
      base::TimeDelta::FromMilliseconds(33);

  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_SCOPED(AdvanceFrame());

  EXPECT_EQ(initial_interval, scheduler_->BeginImplFrameInterval());

  scheduler_->NotifyBeginMainFrameStarted(now_src_->NowTicks());
  scheduler_->NotifyReadyToCommit();
  scheduler_->NotifyReadyToActivate();
  task_runner().RunTasksWhile(client_->InsideBeginImplFrame(true));

  // Test changing the interval on the frame source external to the scheduler.
  synthetic_frame_source_->OnUpdateVSyncParameters(now_src_->NowTicks(),
                                                   authoritative_interval);

  EXPECT_SCOPED(AdvanceFrame());

  // At the next BeginFrame, authoritative interval is used instead of previous
  // interval.
  EXPECT_NE(initial_interval, scheduler_->BeginImplFrameInterval());
  EXPECT_EQ(authoritative_interval, scheduler_->BeginImplFrameInterval());
}

TEST_F(SchedulerTest, ImplLatencyTakesPriority) {
  SetUpScheduler(THROTTLED_BFS);

  scheduler_->SetTreePrioritiesAndScrollState(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER);
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(true);
  EXPECT_TRUE(scheduler_->ImplLatencyTakesPriority());
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(false);
  EXPECT_TRUE(scheduler_->ImplLatencyTakesPriority());

  scheduler_->SetTreePrioritiesAndScrollState(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER);
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(true);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(false);
  EXPECT_TRUE(scheduler_->ImplLatencyTakesPriority());

  scheduler_->SetTreePrioritiesAndScrollState(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER);
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(true);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(false);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());

  scheduler_->SetTreePrioritiesAndScrollState(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER);
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(true);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());
  scheduler_->SetCriticalBeginMainFrameToActivateIsFast(false);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());
}

TEST_F(SchedulerTest, NoLayerTreeFrameSinkCreationWhileCommitPending) {
  SetUpScheduler(THROTTLED_BFS);

  // SetNeedsBeginMainFrame should begin the frame.
  scheduler_->SetNeedsBeginMainFrame();
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  // Lose the LayerTreeFrameSink and trigger the deadline.
  client_->Reset();
  scheduler_->DidLoseLayerTreeFrameSink();
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_NO_ACTION();

  // The scheduler should not trigger the LayerTreeFrameSink creation till the
  // commit is aborted.
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_NO_ACTION();

  // Abort the commit.
  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->BeginMainFrameAborted(
      CommitEarlyOutReason::ABORTED_LAYER_TREE_FRAME_SINK_LOST);
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
}

TEST_F(SchedulerTest, ImplSideInvalidationInsideImplFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  // Request an impl-side invalidation. Ensure that it runs before the deadline.
  bool needs_first_draw_on_activation = true;
  scheduler_->SetNeedsImplSideInvalidation(needs_first_draw_on_activation);
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame",
                 "ScheduledActionPerformImplSideInvalidation");
}

TEST_F(SchedulerTest, ImplSideInvalidationsMergedWithCommit) {
  SetUpScheduler(EXTERNAL_BFS);

  // Request a main frame and invalidation, the only action run should be
  // sending the main frame.
  scheduler_->SetNeedsBeginMainFrame();
  bool needs_first_draw_on_activation = true;
  scheduler_->SetNeedsImplSideInvalidation(needs_first_draw_on_activation);
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  // Respond with a commit. The scheduler should only perform the commit
  // actions since the impl-side invalidation request will be merged with the
  // commit.
  client_->Reset();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->NotifyReadyToCommit();
  EXPECT_ACTIONS("ScheduledActionCommit");
  EXPECT_FALSE(scheduler_->needs_impl_side_invalidation());

  // Deadline.
  client_->Reset();
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_NO_ACTION();
}

TEST_F(SchedulerTest, AbortedCommitsTriggerImplSideInvalidations) {
  SetUpScheduler(EXTERNAL_BFS);

  // Request a main frame and invalidation.
  scheduler_->SetNeedsBeginMainFrame();
  bool needs_first_draw_on_activation = true;
  scheduler_->SetNeedsImplSideInvalidation(needs_first_draw_on_activation);
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  // Abort the main frame and request another one, the impl-side invalidations
  // should not be blocked on the main frame.
  client_->Reset();
  scheduler_->SetNeedsBeginMainFrame();
  scheduler_->NotifyBeginMainFrameStarted(now_src()->NowTicks());
  scheduler_->BeginMainFrameAborted(CommitEarlyOutReason::FINISHED_NO_UPDATES);
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionPerformImplSideInvalidation");

  // Activate the sync tree.
  client_->Reset();
  scheduler_->NotifyReadyToActivate();
  EXPECT_ACTIONS("ScheduledActionActivateSyncTree");

  // Second impl frame.
  client_->Reset();
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_ACTIONS("WillBeginImplFrame", "ScheduledActionSendBeginMainFrame");

  // Deadline.
  client_->Reset();
  task_runner_->RunTasksWhile(client_->InsideBeginImplFrame(true));
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
}

// The three letters appeneded to each version of this test mean the following:s
// tree_priority: B = both trees same priority; A = active tree priority;
// scroll_handler_state: H = affects scroll handler; N = does not affect scroll
// handler;
// durations: F = fast durations; S = slow durations
bool SchedulerTest::BeginMainFrameOnCriticalPath(
    TreePriority tree_priority,
    ScrollHandlerState scroll_handler_state,
    base::TimeDelta durations) {
  SetUpScheduler(EXTERNAL_BFS);
  fake_compositor_timing_history_->SetAllEstimatesTo(durations);
  client_->Reset();
  scheduler_->SetTreePrioritiesAndScrollState(tree_priority,
                                              scroll_handler_state);
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_FALSE(client_->last_begin_main_frame_args().IsValid());
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(client_->last_begin_main_frame_args().IsValid());
  return client_->last_begin_main_frame_args().on_critical_path;
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_BNF) {
  EXPECT_TRUE(BeginMainFrameOnCriticalPath(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER,
      kFastDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_BNS) {
  EXPECT_TRUE(BeginMainFrameOnCriticalPath(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER,
      kSlowDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_BHF) {
  EXPECT_TRUE(BeginMainFrameOnCriticalPath(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER, kFastDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_BHS) {
  EXPECT_TRUE(BeginMainFrameOnCriticalPath(
      SAME_PRIORITY_FOR_BOTH_TREES,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER, kSlowDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_ANF) {
  EXPECT_FALSE(BeginMainFrameOnCriticalPath(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER,
      kFastDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_ANS) {
  EXPECT_FALSE(BeginMainFrameOnCriticalPath(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_DOES_NOT_AFFECT_SCROLL_HANDLER,
      kSlowDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_AHF) {
  EXPECT_TRUE(BeginMainFrameOnCriticalPath(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER, kFastDuration));
}

TEST_F(SchedulerTest, BeginMainFrameOnCriticalPath_AHS) {
  EXPECT_FALSE(BeginMainFrameOnCriticalPath(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER, kSlowDuration));
}

TEST_F(SchedulerTest, BeginFrameAckForFinishedImplFrame) {
  // Sets up scheduler and sends two BeginFrames, both finished.
  SetUpScheduler(EXTERNAL_BFS);

  // Run a successful redraw and verify that a new ack is sent.
  scheduler_->SetNeedsRedraw();
  client_->Reset();

  viz::BeginFrameArgs args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // Successful draw caused damage.
  bool has_damage = true;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();

  // Request another redraw, but fail it. Verify that a new ack is sent.
  scheduler_->SetNeedsRedraw();
  client_->Reset();

  args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  client_->SetDrawWillHappen(false);
  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible",
                 // Failed draw triggers SendBeginMainFrame.
                 "ScheduledActionSendBeginMainFrame");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // Failed draw: no damage.
  has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, BeginFrameAckForSkippedImplFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  // To get into a high latency state, this test disables automatic swap acks.
  client_->SetAutomaticSubmitCompositorFrameAck(false);
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);

  // Run a successful redraw that submits a compositor frame but doesn't receive
  // a swap ack. Verify that a viz::BeginFrameAck is sent for it.
  scheduler_->SetNeedsRedraw();
  client_->Reset();

  viz::BeginFrameArgs args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTIONS("ScheduledActionDrawIfPossible");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // Successful draw caused damage.
  bool has_damage = true;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();

  // Request another redraw that will be skipped because the swap ack is still
  // missing. Verify that a new viz::BeginFrameAck is sent.
  scheduler_->SetNeedsRedraw();
  client_->Reset();

  args = SendNextBeginFrame();
  EXPECT_NO_ACTION();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_TRUE(scheduler_->begin_frames_expected());

  // Skipped draw: no damage.
  has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, BeginFrameAckForBeginFrameBeforeLastDeadline) {
  SetUpScheduler(EXTERNAL_BFS);

  // Request tile preparation to schedule a proactive BeginFrame.
  scheduler_->SetNeedsPrepareTiles();
  client_->Reset();

  SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  // Until tiles were prepared, further proactive BeginFrames are expected.
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // Send the next BeginFrame before the previous one's deadline was executed.
  // This should trigger the previous BeginFrame's deadline synchronously,
  // during which tiles will be prepared. As a result of that, no further
  // BeginFrames will be needed, and the new BeginFrame should be dropped.
  viz::BeginFrameArgs args = SendNextBeginFrame();
  EXPECT_ACTIONS("ScheduledActionPrepareTiles", "RemoveObserver(this)");
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  EXPECT_FALSE(scheduler_->begin_frames_expected());

  // Latest ack should be for the dropped BeginFrame.
  bool has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, BeginFrameAckForDroppedBeginFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  // Request a single BeginFrame.
  scheduler_->SetNeedsOneBeginImplFrame();
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  client_->Reset();

  // First BeginFrame is handled by StateMachine.
  viz::BeginFrameArgs first_args = SendNextBeginFrame();
  EXPECT_ACTIONS("WillBeginImplFrame");
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
  // State machine is no longer interested in BeginFrames, but scheduler is
  // still observing the source.
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(scheduler_->BeginFrameNeeded());
  client_->Reset();

  // Send the next BeginFrame before the previous one's deadline was executed.
  // The BeginFrame should be dropped immediately, since the state machine is
  // not expecting any BeginFrames.
  viz::BeginFrameArgs second_args = SendNextBeginFrame();
  EXPECT_NO_ACTION();

  // Latest ack should be for the dropped BeginFrame.
  bool has_damage = false;
  EXPECT_EQ(viz::BeginFrameAck(second_args.source_id,
                               second_args.sequence_number, has_damage),
            client_->last_begin_frame_ack());
  client_->Reset();

  task_runner().RunPendingTasks();  // Run deadline of prior BeginFrame.
  EXPECT_ACTIONS("RemoveObserver(this)");

  // We'd expect an out-of-order ack for the prior BeginFrame.
  has_damage = false;
  EXPECT_EQ(viz::BeginFrameAck(first_args.source_id, first_args.sequence_number,
                               has_damage),
            client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, BeginFrameAckForLateMissedBeginFrame) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  client_->Reset();

  // Send a missed BeginFrame with a passed deadline.
  now_src_->Advance(viz::BeginFrameArgs::DefaultInterval());
  viz::BeginFrameArgs args =
      fake_external_begin_frame_source_->CreateBeginFrameArgs(
          BEGINFRAME_FROM_HERE, now_src());
  args.type = viz::BeginFrameArgs::MISSED;
  now_src_->Advance(viz::BeginFrameArgs::DefaultInterval());
  EXPECT_GT(now_src_->NowTicks(), args.deadline);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);

  EXPECT_NO_ACTION();
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());

  // Latest ack should be for the missed BeginFrame that was too late: no
  // damage.
  bool has_damage = false;
  EXPECT_EQ(
      viz::BeginFrameAck(args.source_id, args.sequence_number, has_damage),
      client_->last_begin_frame_ack());
  client_->Reset();
}

TEST_F(SchedulerTest, CriticalBeginMainFrameToActivateIsFast) {
  SetUpScheduler(EXTERNAL_BFS);

  scheduler_->SetNeedsRedraw();
  base::TimeDelta estimate_duration = base::TimeDelta::FromMilliseconds(1);
  fake_compositor_timing_history_->SetAllEstimatesTo(estimate_duration);

  // If we have a scroll handler but the critical main frame is slow, we should
  // still prioritize impl thread latency.
  scheduler_->SetTreePrioritiesAndScrollState(
      SMOOTHNESS_TAKES_PRIORITY,
      ScrollHandlerState::SCROLL_AFFECTS_SCROLL_HANDLER);
  scheduler_->SetNeedsRedraw();
  // An interval of 2ms makes sure that the main frame is considered slow.
  base::TimeDelta interval = base::TimeDelta::FromMilliseconds(2);
  now_src_->Advance(interval);
  viz::BeginFrameArgs args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, 0u, 1u, now_src_->NowTicks(),
      now_src_->NowTicks() + interval, interval, viz::BeginFrameArgs::NORMAL);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);
  EXPECT_TRUE(scheduler_->ImplLatencyTakesPriority());

  task_runner().RunPendingTasks();  // Run posted deadline to finish the frame.
  ASSERT_FALSE(client_->IsInsideBeginImplFrame());

  // Set an interval of 10ms. The bmf_to_activate_interval should be 1*4 = 4ms,
  // to account for queue + main_frame + pending_tree + activation durations.
  // With a draw time of 1ms and fudge factor of 1ms, the interval available for
  // the main frame to be activated is 8ms, so it should be considered fast.
  scheduler_->SetNeedsRedraw();
  interval = base::TimeDelta::FromMilliseconds(10);
  now_src_->Advance(interval);
  args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, 0u, 2u, now_src_->NowTicks(),
      now_src_->NowTicks() + interval, interval, viz::BeginFrameArgs::NORMAL);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);
  EXPECT_FALSE(scheduler_->ImplLatencyTakesPriority());

  task_runner().RunPendingTasks();  // Run posted deadline to finish the frame.
  ASSERT_FALSE(client_->IsInsideBeginImplFrame());

  // Increase the draw duration to decrease the time available for the main
  // frame. This should prioritize the impl thread.
  scheduler_->SetNeedsRedraw();
  fake_compositor_timing_history_->SetDrawDurationEstimate(
      base::TimeDelta::FromMilliseconds(7));
  now_src_->Advance(interval);
  args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, 0u, 3u, now_src_->NowTicks(),
      now_src_->NowTicks() + interval, interval, viz::BeginFrameArgs::NORMAL);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);
  EXPECT_TRUE(scheduler_->ImplLatencyTakesPriority());
}

TEST_F(SchedulerTest, WaitForAllPipelineStagesSkipsMissedBeginFrames) {
  scheduler_settings_.wait_for_all_pipeline_stages_before_draw = true;
  client_ = std::make_unique<FakeSchedulerClient>();
  CreateScheduler(EXTERNAL_BFS);

  // Initialize frame sink so that state machine Scheduler and state machine
  // need BeginFrames.
  scheduler_->SetVisible(true);
  scheduler_->SetCanDraw(true);
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  base::TimeDelta interval = base::TimeDelta::FromMilliseconds(16);
  now_src_->Advance(interval);
  viz::BeginFrameArgs args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, 0u, 1u, now_src_->NowTicks(),
      now_src_->NowTicks() + interval, interval, viz::BeginFrameArgs::MISSED);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
}

TEST_F(
    SchedulerTest,
    WaitForAllPipelineStagesUsesMissedBeginFramesWithSurfaceSynchronization) {
  scheduler_settings_.wait_for_all_pipeline_stages_before_draw = true;
  scheduler_settings_.enable_surface_synchronization = true;
  client_ = std::make_unique<FakeSchedulerClient>();
  CreateScheduler(EXTERNAL_BFS);

  // Initialize frame sink so that Scheduler and state machine need BeginFrames.
  scheduler_->SetVisible(true);
  scheduler_->SetCanDraw(true);
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  scheduler_->SetNeedsBeginMainFrame();
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // Uses MISSED BeginFrames even after the deadline has passed.
  base::TimeDelta interval = base::TimeDelta::FromMilliseconds(16);
  now_src_->Advance(interval);
  base::TimeTicks timestamp = now_src_->NowTicks();
  // Deadline should have passed after this.
  now_src_->Advance(interval * 2);
  viz::BeginFrameArgs args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, 0u, 1u, timestamp, timestamp + interval, interval,
      viz::BeginFrameArgs::MISSED);
  fake_external_begin_frame_source_->TestOnBeginFrame(args);
  EXPECT_TRUE(client_->IsInsideBeginImplFrame());
}

TEST_F(SchedulerTest, WaitForAllPipelineStagesAlwaysObservesBeginFrames) {
  scheduler_settings_.wait_for_all_pipeline_stages_before_draw = true;
  scheduler_settings_.enable_surface_synchronization = true;
  client_ = std::make_unique<FakeSchedulerClient>();
  CreateScheduler(EXTERNAL_BFS);

  // Initialize frame sink, but don't request a main frame, so that state
  // machine doesn't need BeginFrames, but Scheduler still observes.
  scheduler_->SetVisible(true);
  scheduler_->SetCanDraw(true);
  EXPECT_ACTIONS("ScheduledActionBeginLayerTreeFrameSinkCreation");
  client_->Reset();
  scheduler_->DidCreateAndInitializeLayerTreeFrameSink();
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(client_->IsInsideBeginImplFrame());
  client_->Reset();

  // In full-pipe mode, the Scheduler always observes BeginFrames, even if the
  // state machine doesn't need any.
  EXPECT_TRUE(scheduler_->begin_frames_expected());
  EXPECT_FALSE(scheduler_->BeginFrameNeeded());
}

TEST_F(SchedulerTest, CriticalBeginMainFrameIsFast_CommitEstimateSlow) {
  SetUpScheduler(EXTERNAL_BFS);
  scheduler_->SetNeedsBeginMainFrame();
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  fake_compositor_timing_history_->SetCommitDurationEstimate(kSlowDuration);
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_FALSE(scheduler_->state_machine()
                   .critical_begin_main_frame_to_activate_is_fast());
}

TEST_F(SchedulerTest, CriticalBeginMainFrameIsFast_CommitEstimateFast) {
  SetUpScheduler(EXTERNAL_BFS);
  scheduler_->SetNeedsBeginMainFrame();
  fake_compositor_timing_history_->SetAllEstimatesTo(kFastDuration);
  EXPECT_SCOPED(AdvanceFrame());
  EXPECT_TRUE(scheduler_->state_machine()
                  .critical_begin_main_frame_to_activate_is_fast());
}

}  // namespace
}  // namespace cc