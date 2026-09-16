#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include "src/backend/models/rfdetr/training/training_partition.h"
#include "src/common/system/numa_topology.h"
TEST_CASE("Distributed ranks resolve independent known and unknown GPU locality", "[rfdetr][placement][topology]") {
    using namespace mmltk::backend::models::rfdetr;
    using namespace mmltk::common::system;
    const NumaTopology topology{.permitted_cpus = {2, 8}, .permitted_nodes = {0, 3}, .cpus = {{2, 0, 0, 0}, {8, 3, 1, 0}}, .nodes = {{0, 4096}, {3, 4096}}};
    TrainRequest request;
    request.device_ids = {5, 1};
    request.workers = 7;
    auto ranks = select_distributed_training_partitions(request);
    REQUIRE(ranks.size() == 2);
    CHECK(ranks[0].device_id == 5);
    CHECK(ranks[1].device_id == 1);
    CHECK(ranks[0].worker_budget == 4);
    CHECK(ranks[1].worker_budget == 3);
    CHECK(resolve_placement(topology, 0, ranks[0].numa_node).cpus == std::vector<int>{2});
    CHECK(resolve_placement(topology, 3, ranks[1].numa_node).cpus == std::vector<int>{8});
    for (const auto& rank : ranks) CHECK_THROWS_AS(resolve_placement(topology, -1, rank.numa_node), std::invalid_argument);
    request.numa_node = 0;
    CHECK_THROWS_AS(select_distributed_training_partitions(request), std::invalid_argument);
    request.numa_node = -1;
    request.numa_nodes = {0, 3};
    ranks = select_distributed_training_partitions(request);
    CHECK(resolve_placement(topology, -1, ranks[0].numa_node).numa_node == 0);
    CHECK(resolve_placement(topology, -1, ranks[1].numa_node).numa_node == 3);
    CHECK_THROWS_AS(resolve_placement(topology, 0, ranks[1].numa_node), std::invalid_argument);
    for (const auto& rank : ranks) {
        auto worker = request;
        apply_training_partition(worker, rank);
        CHECK(worker.device_id == rank.device_id);
        CHECK(worker.numa_node == rank.numa_node);
        CHECK(worker.workers == rank.worker_budget);
        CHECK(worker.device_ids.empty());
        CHECK(worker.numa_nodes.empty());
        CHECK_NOTHROW(validate_train_placement(worker));
    }
    request.numa_nodes = {3};
    CHECK_THROWS_AS(select_distributed_training_partitions(request), std::invalid_argument);
    request.numa_nodes = {-2, 3};
    CHECK_THROWS_AS(select_distributed_training_partitions(request), std::invalid_argument);
    request.numa_nodes = {3, 3};
    CHECK_NOTHROW(select_distributed_training_partitions(request));
}
