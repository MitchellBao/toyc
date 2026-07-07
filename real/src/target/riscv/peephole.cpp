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

std::vector<std::string> splitOperands(const std::string& text)
{
    std::vector<std::string> operands;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        operands.push_back(trim(text.substr(begin, end - begin)));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return operands;
}

bool parseInstruction(const std::string& text, std::string& opcode, std::vector<std::string>& operands)
{
    const std::string line = trim(text);
    if (line.empty() || line[0] == '.' || isLabel(line)) {
        return false;
    }
    const std::size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) {
        opcode = line;
        operands.clear();
        return true;
    }
    opcode = line.substr(0, space);
    operands = splitOperands(line.substr(space + 1));
    return true;
}

std::string indentOf(const std::string& line)
{
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return line.substr(0, first);
}

std::string formatInstruction(const std::string& indent, const std::string& opcode, const std::vector<std::string>& operands)
{
    std::ostringstream out;
    out << indent << opcode;
    if (!operands.empty()) {
        out << ' ';
        for (std::size_t i = 0; i < operands.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << operands[i];
        }
    }
    return out.str();
}

bool isZeroRegister(const std::string& reg)
{
    return reg == "zero" || reg == "x0";
}

bool isMemorySlot(const std::string& operand)
{
    const std::size_t open = operand.find('(');
    return open != std::string::npos && operand.find(')', open + 1) == operand.size() - 1;
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
        std::string opcode;
        std::vector<std::string> operands;
        const bool parsed = parseInstruction(text, opcode, operands);

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
        if (parsed && opcode == "addi" && operands.size() == 3 && operands[2] == "0") {
            if (operands[0] == operands[1]) {
                continue;
            }
            out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[1]}));
            continue;
        }
        if (parsed && opcode == "add" && operands.size() == 3) {
            if (isZeroRegister(operands[2])) {
                if (operands[0] == operands[1]) {
                    continue;
                }
                out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[1]}));
                continue;
            }
            if (isZeroRegister(operands[1])) {
                if (operands[0] == operands[2]) {
                    continue;
                }
                out.push_back(formatInstruction(indentOf(lines[i]), "mv", {operands[0], operands[2]}));
                continue;
            }
        }
        if (text.rfind("j ", 0) == 0 && i + 1 < lines.size()) {
            const std::string target = trim(text.substr(2));
            if (isLabel(trim(lines[i + 1])) && trim(lines[i + 1]) == target + ":") {
                continue;
            }
        }
        if (parsed && opcode == "sw" && operands.size() == 2 && isMemorySlot(operands[1]) && i + 1 < lines.size()) {
            std::string nextOpcode;
            std::vector<std::string> nextOperands;
            if (parseInstruction(trim(lines[i + 1]), nextOpcode, nextOperands)
                && nextOpcode == "lw"
                && nextOperands.size() == 2
                && nextOperands[1] == operands[1]) {
                out.push_back(lines[i]);
                if (nextOperands[0] != operands[0]) {
                    out.push_back(formatInstruction(indentOf(lines[i + 1]), "mv", {nextOperands[0], operands[0]}));
                }
                ++i;
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
