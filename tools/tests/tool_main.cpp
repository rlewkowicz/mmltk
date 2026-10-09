import std;
import mmltk.tools.tests.tool_dispatch;
int main(int argc, char** argv) {
 if (argc > 1 && std::string_view(argv[1]) == "--tool") return mmltk::tools::tests::dispatch::entry(argc - 2, argv + 2);
 std::cerr << "mmltk-tests: expected --tool COMMAND -- ARGS...\n";
 return 2;
}
