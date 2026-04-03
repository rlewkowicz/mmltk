#include "mmltk/rfdetr/module.h"

#include "rfdetr/cli.h"

namespace mmltk::rfdetr {

namespace {

class RfdetrCliModule final : public mmltk::model::CliModule {
public:
    [[nodiscard]] std::string_view command_name() const override {
        return "rfdetr";
    }

    [[nodiscard]] std::string_view summary() const override {
        return "RF-DETR model tooling, inference, evaluation, and training";
    }

    int handle_cli(int argc, char** argv) const override {
        return mmltk::rfdetr::handle_cli(argc, argv);
    }
};

} // namespace

const mmltk::model::CliModule& cli_module() {
    static const RfdetrCliModule module;
    return module;
}

} // namespace mmltk::rfdetr
