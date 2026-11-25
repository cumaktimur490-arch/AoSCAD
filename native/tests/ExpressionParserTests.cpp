#include "ExpressionParser.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1.0e-10;
}

void expectError(const QString& expression, const ExpressionParser::Definitions& variables = {})
{
    bool threw = false;
    try {
        (void)ExpressionParser::evaluate(expression, variables);
    } catch (const ExpressionError&) {
        threw = true;
    }
    require(threw, "expected the expression to be rejected");
}
} // namespace

int main()
{
    try {
        const ExpressionParser::Definitions noVariables;
        require(near(ExpressionParser::evaluate(QStringLiteral("2 + 3 * 4"), noVariables), 14.0),
                "operator precedence failed");
        require(near(ExpressionParser::evaluate(QStringLiteral("-(2 + 3)^2"), noVariables), -25.0),
                "unary/power precedence failed");
        require(near(ExpressionParser::evaluate(QStringLiteral("2^3^2"), noVariables), 512.0),
                "power must be right-associative");
        require(near(ExpressionParser::evaluate(QStringLiteral("sqrt(81) + max(2, min(7, 4))"), noVariables), 13.0),
                "function evaluation failed");
        require(near(ExpressionParser::evaluate(QStringLiteral("sin(pi / 2)"), noVariables), 1.0),
                "trigonometry must use radians");

        ExpressionParser::Definitions variables;
        variables.insert(QStringLiteral("L"), QStringLiteral("120"));
        variables.insert(QStringLiteral("W"), QStringLiteral("L / 2"));
        variables.insert(QStringLiteral("Wall"), QStringLiteral("max(2, sqrt(16))"));
        const auto resolved = ExpressionParser::evaluateVariables(variables);
        require(near(resolved.value(QStringLiteral("W")), 60.0), "dependent variable evaluation failed");
        require(near(resolved.value(QStringLiteral("Wall")), 4.0), "nested variable formula failed");
        require(near(ExpressionParser::evaluate(QStringLiteral("W + Wall"), resolved), 64.0),
                "expression with resolved variables failed");

        expectError(QStringLiteral("1 / 0"));
        expectError(QStringLiteral("sqrt(-1)"));
        expectError(QStringLiteral("missing + 1"));
        expectError(QStringLiteral("unknown(2)"));
        expectError(QStringLiteral("sin(1, 2)"));
        expectError(QStringLiteral("1 + (2"));

        ExpressionParser::Definitions cycle;
        cycle.insert(QStringLiteral("A"), QStringLiteral("B + 1"));
        cycle.insert(QStringLiteral("B"), QStringLiteral("A + 1"));
        bool cycleDetected = false;
        try {
            (void)ExpressionParser::evaluateVariables(cycle);
        } catch (const ExpressionError&) {
            cycleDetected = true;
        }
        require(cycleDetected, "variable cycle was not detected");

        std::cout << "Expression parser tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Expression parser test failure: " << error.what() << '\n';
        return 1;
    }
}
