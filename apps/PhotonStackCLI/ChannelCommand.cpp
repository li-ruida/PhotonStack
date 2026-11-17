#include "ChannelCommand.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include <cmath>
#include <iostream>
#include <optional>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
using namespace photonstack::cli;
// Compare actual pixel coordinates, including distortion, on a 9x9 grid.
// This checks supplied WCS consistency, not absolute astrometric accuracy.
void checkSkyGrid(const SolvedPanel& reference, const SolvedPanel& other, unsigned width, unsigned height) {
    const SkyProjection ref(reference, reference.grid), projection(other, reference.grid);
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 9; ++x) {
        const double px = (width-1)*(x/8.0), py = (height-1)*(y/8.0);
        const auto sky = projection.toCanvas(px, py), back = ref.toPanel(sky.x, sky.y);
        if (!std::isfinite(back.x) || !std::isfinite(back.y) || std::hypot(back.x-px, back.y-py) > .05)
            throw std::invalid_argument("Master WCS grids differ by more than 0.05 pixel; register the masters before composition");
    }
}
}
int runChannels(const std::vector<std::string>& args) {
    ChannelRecipe recipe; fs::path directory, sourceRecipe; std::size_t workers = 0;
    std::set<std::string> seen;
    const bool fresh = args[0] == "channel-compose", inspect = args[0] == "channel-inspect";
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto option = args[i];
        if (option != "--input" && !seen.insert(option).second) throw std::invalid_argument("Duplicate channel option");
        if (option == "--aligned" && fresh) { recipe.aligned = true; continue; }
        if (++i == args.size()) throw std::invalid_argument("Missing channel option value");
        const auto& value = args[i];
        if (option == "--recipe" && !fresh) sourceRecipe = value;
        else if (option == "--output-dir" && !inspect) directory = value;
        else if (option == "--input" && fresh) {
            const auto split = value.find('=');
            if (split == std::string::npos) throw std::invalid_argument("Use --input symbol=/path/master.fits");
            if (recipe.sources.size() >= 16) throw std::invalid_argument("At most 16 masters are supported");
            recipe.sources.push_back({value.substr(0, split), fs::canonical(value.substr(split+1)), {}});
        } else if ((option == "--red" || option == "--green" || option == "--blue") && !inspect)
            recipe.expressions[option == "--red" ? 0 : option == "--green" ? 1 : 2] = value;
        else if (option == "--workers" && !inspect) {
            std::size_t used = 0; workers = std::stoul(value, &used);
            if (used != value.size() || workers < 1 || workers > 64) throw std::invalid_argument("Workers must be 1..64");
        } else throw std::invalid_argument("Unknown channel option: " + option);
    }
    if (!fresh) {
        if (sourceRecipe.empty()) throw std::invalid_argument("Specify --recipe");
        const auto overrides = recipe.expressions;
        recipe = readChannelRecipe(sourceRecipe, inspect);
        for (std::size_t c = 0; c < 3; ++c) if (seen.contains(std::array{"--red", "--green", "--blue"}[c])) recipe.expressions[c] = overrides[c];
    } else for (auto& s : recipe.sources) s.sha = fileSHA256(s.path);
    verifyChannelSources(recipe);
    if (inspect) { printChannelRecipe(recipe, sourceRecipe.parent_path(), std::cout); return 0; }
    if (directory.empty() || fs::exists(directory)) throw std::invalid_argument("Choose a new output directory");
    const auto programs = recipe.compile();
    std::vector<ImageBuffer> planes; std::optional<SolvedPanel> sky;
    bool allSolved = true;
    for (const auto& s : recipe.sources) {
        auto master = readLinearMonoMaster(s.path);
        if (!planes.empty() && (planes[0].width != master.image.width || planes[0].height != master.image.height))
            throw std::invalid_argument("Register all masters onto the same pixel grid");
        if (master.sky) {
            if (sky) checkSkyGrid(*sky, *master.sky, master.image.width, master.image.height);
            else sky = master.sky;
        } else allSolved = false;
        planes.push_back(std::move(master.image));
    }
    if (!allSolved) sky.reset(); // Do not attach an unverified solution to other masters.
    const auto result = composeChannels(planes, programs, workers);
    if (result.invalidPixels == result.image.pixelCount()) throw std::invalid_argument("Composition has no valid pixels; check expressions and master coverage");
    if (!fs::create_directory(directory)) throw std::invalid_argument("Choose a new output directory");
    try {
        recipe.products.clear();
        for (const auto& [name, image] : std::array<std::pair<const char*, const ImageBuffer*>, 2>{{{"RGB.fits", &result.image}, {"validity.fits", &result.validity}}}) {
            const auto saved = FitsCodec().write(*image, directory/name, sky ? &sky->grid : nullptr, sky ? &sky->distortion : nullptr);
            if (!saved.ok) throw std::runtime_error(saved.message);
            recipe.products[name] = fileSHA256(directory/name);
        }
        verifyChannelSources(recipe);
        writeChannelRecipe(directory/"recipe.txt", recipe);
        printChannelRecipe(recipe, directory, std::cout, result.invalidPixels, result.workers, sky.has_value());
    } catch (...) {
        std::error_code ignored;
        for (const char* name : {"RGB.fits", "validity.fits", "recipe.txt"}) fs::remove(directory/name, ignored);
        fs::remove(directory, ignored); throw;
    }
    return 0;
}
