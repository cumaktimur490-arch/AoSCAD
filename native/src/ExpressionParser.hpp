#pragma once

#include <QMap>
#include <QString>

#include <stdexcept>

/** A user-facing error raised while evaluating a CAD expression. */
class ExpressionError final : public std::runtime_error {
public:
    explicit ExpressionError(const QString& message);
};

/**
 * Small, deterministic arithmetic expression evaluator for CAD parameters.
 *
 * Expressions intentionally do not execute C++ or JavaScript. Supported syntax:
 * +, -, *, /, ^, parentheses, pi, variables, sin/cos/sqrt/min/max.
 * Trigonometric functions use radians. Every result must be finite.
 */
class ExpressionParser final {
public:
    using Definitions = QMap<QString, QString>;
    using Values = QMap<QString, double>;

    /** Resolve a complete variable table, detecting unknown names and cycles. */
    static Values evaluateVariables(const Definitions& definitions);

    /** Evaluate an expression using named expressions as variable definitions. */
    static double evaluate(const QString& expression, const Definitions& definitions);

    /** Evaluate an expression using already-resolved numeric values. */
    static double evaluate(const QString& expression, const Values& values);
};
