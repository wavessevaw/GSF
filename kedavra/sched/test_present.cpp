// SPDX-License-Identifier: GPL-3.0-or-later
#include "kfg_present.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

struct Frame { uint64_t source; unsigned phase; };
int main()
{
    KfgPresentQueue<Frame, 3> queue;
    auto a = queue.reserve({17, 1});
    // Flip can finish BEFORE atomic commit returns and OnPresented runs.
    auto fast = queue.complete(a);
    assert(fast && fast->source == 17 && fast->phase == 1);
    assert(!queue.complete(a));
    auto failed = queue.reserve({18, 2});
    queue.cancel(failed);
    assert(!queue.complete(failed));
    auto b = queue.reserve({20, 0}), c = queue.reserve({21, 1}), d = queue.reserve({22, 2});
    assert(queue.reserve({23, 0}) == 0 && queue.dropped == 1);
    assert(queue.complete(d)->source == 22); // completion order need not be FIFO
    assert(queue.complete(b)->source == 20);
    queue.reset();
    auto after_reset = queue.reserve({24, 0});
    assert(after_reset > d && !queue.complete(c));
    assert(queue.complete(after_reset)->source == 24);

    KfgPresentQueue<Frame> stress;
    uint64_t random = 5;
    std::vector<std::pair<uint64_t, Frame>> pending;
    for (uint64_t i = 1; i <= 100000; ++i) {
        random = random * 6364136223846793005ull + 1;
        if (i % 199 == 0) { stress.reset(); pending.clear(); }
        Frame frame{i, unsigned(i % 3)};
        auto token = stress.reserve(frame);
        if (token) pending.emplace_back(token, frame);
        if (!pending.empty() && random % 3) {
            size_t index = size_t(random % pending.size());
            auto [id, expected] = pending[index];
            if (random % 7 == 0) { stress.cancel(id); assert(!stress.complete(id)); }
            else {
                auto got = stress.complete(id);
                assert(got && got->source == expected.source && got->phase == expected.phase);
            }
            pending.erase(pending.begin() + index);
        }
    }
    puts("PASS: 100000 present-token events, fast flips, failed commits, reset, reorder and overflow");
}
