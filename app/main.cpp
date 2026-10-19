#include "simlab/codec.hpp"

#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw simlab::Error("cannot open input file"); }
    std::string text;
    std::array<char, 4096> buffer{};
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
        const auto count = static_cast<std::size_t>(file.gcount());
        if (text.size() + count > simlab::max_text_bytes) { throw simlab::Error("input exceeds 4 MiB limit"); }
        text.append(buffer.data(), count);
    }
    if (!file.eof()) { throw simlab::Error("failed to read input file"); }
    return text;
}

void write_new(const std::filesystem::path& path, const std::string& text) {
    // Best-effort no-clobber, not a concurrent-writer or crash-atomic guarantee.
    if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found) {
        throw simlab::Error("output path already exists");
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) { throw simlab::Error("cannot create output file"); }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) { throw simlab::Error("failed to write output file"); }
}

std::string execute(const simlab::Scenario& scenario) {
    auto simulation = simlab::make_simulation(scenario);
    const auto trace = simlab::drain(simulation);
    return simlab::format_result(trace, simulation.snapshot());
}

std::size_t step_count(const std::string& text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 2 * simlab::max_jobs) {
        throw simlab::Error("steps must be an integer between 0 and 8192");
    }
    return static_cast<std::size_t>(value);
}

int usage() {
    std::cerr << "usage:\n"
              << "  simlab run PLAN\n"
              << "  simlab record PLAN RECORD\n"
              << "  simlab replay RECORD\n"
              << "  simlab checkpoint PLAN STEPS SNAPSHOT\n"
              << "  simlab resume SNAPSHOT\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { return usage(); }
    try {
        const std::string command = argv[1];
        std::string output;
        if (command == "run" && argc == 3) {
            output = execute(simlab::parse_plan(read_file(argv[2])));
        } else if (command == "record" && argc == 4) {
            const auto scenario = simlab::parse_plan(read_file(argv[2]));
            output = execute(scenario);
            write_new(argv[3], simlab::format_record(scenario, output));
        } else if (command == "replay" && argc == 3) {
            const auto record = simlab::parse_record(read_file(argv[2]));
            output = execute(record.scenario);
            if (output != record.expected_result) { throw simlab::Error("replay mismatch: result differs from record"); }
        } else if (command == "checkpoint" && argc == 5) {
            auto simulation = simlab::make_simulation(simlab::parse_plan(read_file(argv[2])));
            const auto count = step_count(argv[3]);
            for (std::size_t i = 0; i < count; ++i) {
                if (!simulation.step()) { throw simlab::Error("requested steps exceed pending work"); }
            }
            output = simlab::format_snapshot(simulation.snapshot());
            write_new(argv[4], output);
        } else if (command == "resume" && argc == 3) {
            simlab::Simulation simulation;
            simulation.restore(simlab::parse_snapshot(read_file(argv[2])));
            const auto trace = simlab::drain(simulation);
            output = simlab::format_result(trace, simulation.snapshot());
        } else { return usage(); }
        std::cout << output;
        std::cout.flush();
        if (!std::cout) { throw simlab::Error("failed to write standard output"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
