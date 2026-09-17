#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include "src/backend/models/rfdetr/training/detail/evaluation_runtime.h"
using namespace mmltk::backend::models::rfdetr;
TEST_CASE("Evaluation encoding collection preserves image order", "[rfdetr][evaluation][settlement]") {
    PendingPredictionBatchEncoding pending;
    for (int image_id : {7, 3, 11}) {
        pending.images.push_back(std::async(std::launch::deferred, [image_id] {
            PredictionBatchItem image;
            image.image_id = image_id;
            return image;
        }));
    }
    const auto results = collect_prediction_batch_encoding(std::move(pending));
    REQUIRE(results.size() == 3U);
    CHECK(results[0].image_id == 7);
    CHECK(results[1].image_id == 3);
    CHECK(results[2].image_id == 11);
    for (const auto& future : pending.images) CHECK_FALSE(future.valid());
}
TEST_CASE("Evaluation encoding failure settles every sibling before propagating", "[rfdetr][evaluation][settlement]") {
    for (int failure_index : {0, 1, 2}) {
        PendingPredictionBatchEncoding pending;
        std::atomic<int> completed{0};
        auto lifetime = std::make_shared<int>(0);
        std::weak_ptr<int> borrowed_lifetime = lifetime;
        for (int index = 0; index < 3; ++index) {
            pending.images.push_back(std::async(std::launch::deferred, [&, index, lifetime]() mutable {
                auto retained = std::move(lifetime);
                ++completed;
                if (index == failure_index) throw std::runtime_error("first image failure");
                if (index > failure_index) throw std::runtime_error("later image failure");
                return PredictionBatchItem{};
            }));
        }
        lifetime.reset();
        CHECK_THROWS_WITH(collect_prediction_batch_encoding(std::move(pending)), "first image failure");
        CHECK(completed.load() == 3);
        CHECK(borrowed_lifetime.expired());
        for (const auto& future : pending.images) CHECK_FALSE(future.valid());
        CHECK(collect_prediction_batch_encoding(std::move(pending)).empty());
    }
}
TEST_CASE("Evaluation encoding collection joins a running sibling after a consumed failure", "[rfdetr][evaluation][settlement]") {
    PendingPredictionBatchEncoding pending;
    std::promise<PredictionBatchItem> failed;
    pending.images.push_back(failed.get_future());
    failed.set_exception(std::make_exception_ptr(std::runtime_error("already observed")));
    CHECK_THROWS_WITH(pending.images.front().get(), "already observed");
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    std::atomic<bool> completed{false};
    pending.images.push_back(std::async(std::launch::async, [&] {
        entered.set_value();
        released.get();
        completed = true;
        PredictionBatchItem image;
        image.image_id = 19;
        return image;
    }));
    entered.get_future().get();
    auto collection = std::async(std::launch::async, [&] { return collect_prediction_batch_encoding(std::move(pending)); });
    release.set_value();
    const auto results = collection.get();
    CHECK(completed.load());
    REQUIRE(results.size() == 1U);
    CHECK(results.front().image_id == 19);
    for (const auto& future : pending.images) CHECK_FALSE(future.valid());
}
