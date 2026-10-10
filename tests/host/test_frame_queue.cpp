#include "check.h"
#include "media/frame_queue.h"

#include <atomic>
#include <thread>

static AVFrame* makeFrame() {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_NV12;
    f->width = 64;
    f->height = 32;
    av_frame_get_buffer(f, 0);
    return f;
}

int main() {
    // Order and timestamps survive the trip; finish() still delivers.
    {
        FrameQueue q;
        for (int i = 0; i < 3; i++) q.push(makeFrame(), i * 0.5);
        CHECK_EQ((int)q.size(), 3);
        q.finish();
        CHECK(!q.isDrained());
        for (int i = 0; i < 3; i++) {
            QueuedVideoFrame f;
            CHECK(q.pop(f, 10));
            CHECK_NEAR(f.pts, i * 0.5, 1e-9);
            CHECK(f.frame != nullptr);
            av_frame_free(&f.frame);
        }
        QueuedVideoFrame none;
        CHECK(!q.pop(none, 10));
        CHECK(q.isDrained());
        // Pushing after finish is harmless (frame is freed, not queued).
        q.push(makeFrame(), 9.0);
        CHECK_EQ((int)q.size(), 0);
    }

    // pop() times out on an empty, still-open queue.
    {
        FrameQueue q;
        QueuedVideoFrame f;
        CHECK(!q.pop(f, 20));
        CHECK(!q.isDrained());
    }

    // stop() drops everything and unblocks a producer stuck on a full queue.
    {
        FrameQueue q;
        for (size_t i = 0; i < FrameQueue::MAX_SIZE; i++) q.push(makeFrame(), 0.0);
        std::atomic<bool> producerDone{false};
        std::thread producer([&] {
            q.push(makeFrame(), 1.0); // blocks: queue is full
            producerDone = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!producerDone);
        q.stop();
        producer.join();
        CHECK(producerDone);
        CHECK_EQ((int)q.size(), 0);
        QueuedVideoFrame f;
        CHECK(!q.pop(f, 10));
        CHECK(q.isDrained());
    }

    // A blocked producer resumes when the consumer pops.
    {
        FrameQueue q;
        for (size_t i = 0; i < FrameQueue::MAX_SIZE; i++) q.push(makeFrame(), (double)i);
        std::atomic<int> pushed{0};
        std::thread producer([&] {
            q.push(makeFrame(), 100.0);
            pushed = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_EQ(pushed.load(), 0);
        QueuedVideoFrame f;
        CHECK(q.pop(f, 10));
        CHECK_NEAR(f.pts, 0.0, 1e-9);
        av_frame_free(&f.frame);
        producer.join();
        CHECK_EQ(pushed.load(), 1);
        CHECK_EQ((int)q.size(), (int)FrameQueue::MAX_SIZE);
        q.stop();
    }

    // Capacity scales with frame size so decoded-frame *bytes* stay at the
    // 720p level; 720p (and anything smaller / unknown) is unchanged.
    {
        const size_t f720 = 1280u * 720u * 3u / 2u;
        CHECK_EQ((int)FrameQueue::capacityForFrameBytes(f720), (int)FrameQueue::MAX_SIZE);
        CHECK_EQ((int)FrameQueue::capacityForFrameBytes(0), (int)FrameQueue::MAX_SIZE);
        CHECK_EQ((int)FrameQueue::capacityForFrameBytes(64u * 32u * 3u / 2u), (int)FrameQueue::MAX_SIZE);
        const size_t f1080 = 1920u * 1080u * 3u / 2u; // 3,110,400
        size_t c1080 = FrameQueue::capacityForFrameBytes(f1080);
        CHECK_EQ((int)c1080, 16);
        CHECK(c1080 * f1080 <= FrameQueue::BIG_PICTURE_BUDGET_BYTES);
        CHECK_EQ((int)FrameQueue::capacityForFrameBytes(1920u * 1088u * 3u / 2u), 16);
        // Absurdly large frames still get the minimum depth, never zero.
        CHECK_EQ((int)FrameQueue::capacityForFrameBytes((size_t)64 << 20), (int)FrameQueue::MIN_CAPACITY);

        FrameQueue dflt;
        CHECK_EQ((int)dflt.capacity(), (int)FrameQueue::MAX_SIZE);
        FrameQueue small(10);
        CHECK_EQ((int)small.capacity(), 10);
        for (int i = 0; i < 10; i++) small.push(makeFrame(), (double)i);
        std::atomic<int> pushed{0};
        std::thread producer([&] { small.push(makeFrame(), 10.0); pushed = 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK_EQ(pushed.load(), 0); // full at 10, blocks
        QueuedVideoFrame f;
        CHECK(small.pop(f, 10));
        av_frame_free(&f.frame);
        producer.join();
        CHECK_EQ(pushed.load(), 1);
        CHECK_EQ((int)small.size(), 10);
        small.stop();
    }

    // lastPushedPts(): the timeline end of decoded video, independent of how many
    // frames are still queued (Player measures decoded-ahead time with it).
    {
        FrameQueue q(4);
        CHECK(std::isnan(q.lastPushedPts())); // nothing yet
        q.push(makeFrame(), 10.0);
        q.push(makeFrame(), 10.5);
        CHECK_NEAR(q.lastPushedPts(), 10.5, 1e-9);
        QueuedVideoFrame f;
        CHECK(q.pop(f, 10));
        av_frame_free(&f.frame);
        CHECK(q.pop(f, 10));
        av_frame_free(&f.frame);
        CHECK_EQ((int)q.size(), 0);
        CHECK_NEAR(q.lastPushedPts(), 10.5, 1e-9); // still the newest, though drained
        q.push(makeFrame(), NAN);                  // a frame without a timestamp
        CHECK(std::isnan(q.lastPushedPts()));      // reported honestly, not carried over
        q.stop();
    }

    return check::finish("test_frame_queue");
}
