#include "doctest.h"

#include <fstream>

#include "listen.h"

using namespace moonshine;

namespace {

std::filesystem::path fake_model(const std::filesystem::path& root, const std::string& name) {
    const std::filesystem::path dir = root / name / "quantized";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "tokenizer.bin") << "x";
    std::ofstream(dir / "frontend.ort") << "x";
    return dir;
}

}  // namespace

TEST_CASE("the model's architecture is read off its name, through the quantized folder") {
    CHECK(stt_arch_for("/cache/model/tiny-streaming-en/quantized") == 2);
    CHECK(stt_arch_for("/cache/model/base-streaming-en/quantized") == 3);
    CHECK(stt_arch_for("/cache/model/small-streaming-en/quantized") == 4);
    CHECK(stt_arch_for("/cache/model/medium-streaming-en") == 5);
    CHECK(stt_arch_for("/cache/model/tiny-en/quantized") == 0);
    CHECK(stt_arch_for("/cache/model/base-en/quantized_26_08_21") == 1);
    CHECK(stt_arch_for("/cache/model/spelling-en/quantized") == -1);
    CHECK(stt_arch_for("") == -1);
}

TEST_CASE("a model is found by path or by its name in the cache") {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "moonshine-stt-test";
    std::filesystem::remove_all(root);
    const std::filesystem::path tiny = fake_model(root, "tiny-streaming-en");
    const std::filesystem::path base = fake_model(root, "base-en");

    // A directory that holds the files, or whose quantized/ does.
    CHECK(find_stt_model(tiny) == tiny);
    CHECK(find_stt_model(root / "tiny-streaming-en") == tiny);
    CHECK(find_stt_model(root / "base-en") == base);
    // A path that is not a model is not one; it is not looked for by name.
    CHECK(find_stt_model(root / "nothing-here").empty());
    CHECK(find_stt_model(root).empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("without the library there is nothing to hear") {
    // The tests are built without Moonshine Voice; the app says so rather than guessing.
    if (!listen_built()) {
        CHECK_THROWS(Ear(std::filesystem::path("/nowhere/tiny-streaming-en/quantized")));
    }
}
