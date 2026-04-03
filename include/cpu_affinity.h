#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mmltk {

std::vector<int> allowed_cpu_set();
std::vector<int> parse_cpu_list(const std::string& spec);
std::vector<int> resolve_cpu_affinity(const std::string& spec);
std::string format_cpu_list(const std::vector<int>& cpus);
void pin_thread_to_cpu(const std::vector<int>& cpus, size_t worker_index);
void set_thread_name(const std::string& name);

} // namespace mmltk
