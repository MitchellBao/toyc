#include "peephole.h"

#include <sstream>
#include <string>
#include <vector>

namespace toyc::riscv {
namespace {

std::string trim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool isLabel(const std::string& line)
{
    return !line.empty() && line.back() == ':';
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

} // namespace

std::string peephole(const std::string& assembly)
{
    std::vector<std::string> lines = splitLines(assembly);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string text = trim(lines[i]);
        if (text.rfind("mv ", 0) == 0) {
            const std::size_t comma = text.find(',');
            if (comma != std::string::npos) {
                const std::string lhs = trim(text.substr(3, comma - 3));
                const std::string rhs = trim(text.substr(comma + 1));
                if (lhs == rhs) {
                    continue;
                }
            }
        }
        if (text.rfind("j ", 0) == 0 && i + 1 < lines.size()) {
            const std::string target = trim(text.substr(2));
            if (isLabel(trim(lines[i + 1])) && trim(lines[i + 1]) == target + ":") {
                continue;
            }
        }
        out.push_back(lines[i]);
    }

    std::ostringstream joined;
    for (const std::string& line : out) {
        joined << line << '\n';
    }
    return joined.str();
}

} // namespace toyc::riscv
