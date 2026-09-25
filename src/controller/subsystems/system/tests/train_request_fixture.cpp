#include <cstdio>
#include <exception>
#include <string_view>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
int main(int argc, char** argv) {
 try {
  if (argc != 5 || std::string_view(argv[1]) != "rfdetr" || std::string_view(argv[2]) != "train" || std::string_view(argv[3]) != "--request-json") return 2;
  const auto request = mmltk::backend::models::rfdetr::decode_train_request_json(argv[4]);
  return std::fputs(request.output_dir.c_str(), stdout) < 0 ? 3 : 0;
 } catch (const std::exception& error) {
  std::fprintf(stderr, "training request fixture: %s\n", error.what());
  return 1;
 }
}
