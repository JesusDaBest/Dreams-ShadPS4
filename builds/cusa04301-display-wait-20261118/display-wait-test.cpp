#include "video_core/amdgpu/dreams_display_wait.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <semaphore>

using namespace std::chrono_literals;

struct Display {
    std::mutex label_mutex;
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::binary_semaphore first_check{0};
    bool ready{};
    unsigned pending{};
    bool reported_check{};

    bool Wait() {
        return DreamsDisplayWait::WaitForLabelOrCommands(
            submit_mutex, submit_cv, this,
            [&] {
                std::scoped_lock lock{label_mutex};
                if (!reported_check) {
                    reported_check = true;
                    first_check.release();
                }
                return ready;
            },
            [&] { return pending != 0; });
    }

    void Submit() {
        std::scoped_lock lock{submit_mutex};
        ++pending;
        submit_cv.notify_one();
    }

    void ReleaseLabel() {
        {
            std::scoped_lock lock{label_mutex};
            ready = true;
        }
        if (DreamsDisplayWait::waiting_port.load() == this) {
            Submit();
        }
    }
};

static bool Get(std::future<bool>& result) {
    if (result.wait_for(2s) != std::future_status::ready) {
        std::fputs("Display wait failed to wake\n", stderr);
        std::abort();
    }
    return result.get();
}

int main() {
    constexpr unsigned repetitions = 200;
    for (unsigned index = 0; index < repetitions; ++index) {
        Display early_command;
        early_command.Submit();
        assert(!early_command.Wait());

        Display early_label;
        early_label.ReleaseLabel();
        early_label.Submit();
        assert(early_label.Wait());

        Display delayed_command;
        auto command = std::async(std::launch::async, [&] { return delayed_command.Wait(); });
        assert(delayed_command.first_check.try_acquire_for(2s));
        delayed_command.Submit();
        assert(!Get(command));
        assert(!delayed_command.ready);

        Display delayed_label;
        auto label = std::async(std::launch::async, [&] { return delayed_label.Wait(); });
        assert(delayed_label.first_check.try_acquire_for(2s));
        // Waiter remains registered with the original port through a spurious signal.
        delayed_label.submit_cv.notify_one();
        assert(label.wait_for(1ms) == std::future_status::timeout);
        delayed_label.ReleaseLabel();
        assert(Get(label));
        assert(!DreamsDisplayWait::waiting_port.load());
    }
    std::printf("Verified %u runs each for early and late host work, early and late display releases, and spurious wakes\n",
                repetitions);
}
