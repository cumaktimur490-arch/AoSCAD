#include "ExpressionParser.hpp"

#include <QChar>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace {

class Parser final {
public:
    using Resolver = std::function<double(const QString&)>;

    Parser(QString expression, Resolver resolver)
        : source_(std::move(expression)), resolver_(std::move(resolver))
    {
    }

    double parse()
    {
        if (source_.size() > 4096)
            fail(QStringLiteral("Выражение превышает допустимую длину 4096 символов."));
        skipWhitespace();
        if (atEnd())
            fail(QStringLiteral("Введите выражение."));
        const double value = parseExpression();
        skipWhitespace();
        if (!atEnd())
            fail(QStringLiteral("Неожиданный символ «%1».").arg(source_.mid(position_, 1)));
        return checked(value);
    }

private:
    QString source_;
    Resolver resolver_;
    qsizetype position_{0};
    int recursionDepth_{0};

    bool atEnd() const { return position_ >= source_.size(); }

    void skipWhitespace()
    {
        while (!atEnd() && source_[position_].isSpace())
            ++position_;
    }

    bool consume(QChar expected)
    {
        skipWhitespace();
        if (atEnd() || source_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    [[noreturn]] void fail(const QString& message) const
    {
        throw ExpressionError(QStringLiteral("%1 (позиция %2)").arg(message).arg(position_ + 1));
    }

    double checked(double value) const
    {
        if (!std::isfinite(value))
            fail(QStringLiteral("Результат выражения не является конечным числом."));
        return value;
    }

    double parseExpression()
    {
        if (++recursionDepth_ > 128) {
            --recursionDepth_;
            fail(QStringLiteral("Слишком глубокая вложенность выражения."));
        }
        double value = parseTerm();
        for (;;) {
            if (consume(QLatin1Char('+'))) {
                value = checked(value + parseTerm());
            } else if (consume(QLatin1Char('-'))) {
                value = checked(value - parseTerm());
            } else {
                break;
            }
        }
        --recursionDepth_;
        return value;
    }

    double parseTerm()
    {
        double value = parseUnary();
        for (;;) {
            if (consume(QLatin1Char('*'))) {
                value = checked(value * parseUnary());
            } else if (consume(QLatin1Char('/'))) {
                const double divisor = parseUnary();
                if (divisor == 0.0)
                    fail(QStringLiteral("Деление на ноль."));
                value = checked(value / divisor);
            } else {
                return value;
            }
        }
    }

    double parseUnary()
    {
        if (++recursionDepth_ > 128) {
            --recursionDepth_;
            fail(QStringLiteral("Слишком глубокая вложенность унарных операторов."));
        }
        double value = 0.0;
        if (consume(QLatin1Char('+')))
            value = parseUnary();
        else if (consume(QLatin1Char('-')))
            value = checked(-parseUnary());
        else
            value = parsePower();
        --recursionDepth_;
        return value;
    }

    double parsePower()
    {
        const double base = parsePrimary();
        if (!consume(QLatin1Char('^')))
            return base;
        const double exponent = parseUnary();
        return checked(std::pow(base, exponent));
    }

    double parsePrimary()
    {
        skipWhitespace();
        if (consume(QLatin1Char('('))) {
            const double value = parseExpression();
            if (!consume(QLatin1Char(')')))
                fail(QStringLiteral("Ожидалась закрывающая скобка."));
            return value;
        }
        if (atEnd())
            fail(QStringLiteral("Ожидалось число, переменная или скобка."));
        if (source_[position_].isDigit() || source_[position_] == QLatin1Char('.'))
            return parseNumber();
        if (source_[position_].isLetter() || source_[position_] == QLatin1Char('_'))
            return parseIdentifier();
        fail(QStringLiteral("Ожидалось число, переменная или функция."));
    }

    double parseNumber()
    {
        const qsizetype start = position_;
        bool hasDigit = false;
        bool hasExponent = false;
        bool hasDecimal = false;
        while (!atEnd()) {
            const QChar c = source_[position_];
            if (c.isDigit()) {
                hasDigit = true;
                ++position_;
            } else if (c == QLatin1Char('.') && !hasDecimal && !hasExponent) {
                hasDecimal = true;
                ++position_;
            } else if ((c == QLatin1Char('e') || c == QLatin1Char('E')) && !hasExponent && hasDigit) {
                hasExponent = true;
                ++position_;
                if (!atEnd() && (source_[position_] == QLatin1Char('+') || source_[position_] == QLatin1Char('-')))
                    ++position_;
            } else {
                break;
            }
        }
        if (!hasDigit)
            fail(QStringLiteral("Некорректное число."));
        bool ok = false;
        const double value = source_.mid(start, position_ - start).toDouble(&ok);
        if (!ok)
            fail(QStringLiteral("Некорректное число."));
        return checked(value);
    }

    double parseIdentifier()
    {
        skipWhitespace();
        const qsizetype start = position_;
        while (!atEnd()) {
            const QChar c = source_[position_];
            if (c.isLetterOrNumber() || c == QLatin1Char('_'))
                ++position_;
            else
                break;
        }
        const QString name = source_.mid(start, position_ - start);
        skipWhitespace();
        if (consume(QLatin1Char('(')))
            return parseFunction(name);
        if (name.compare(QStringLiteral("pi"), Qt::CaseInsensitive) == 0)
            return std::acos(-1.0);
        return checked(resolver_(name));
    }

    double parseFunction(const QString& name)
    {
        std::vector<double> arguments;
        skipWhitespace();
        if (!consume(QLatin1Char(')'))) {
            for (;;) {
                if (arguments.size() >= 64)
                    fail(QStringLiteral("Функция принимает не более 64 аргументов."));
                arguments.push_back(parseExpression());
                if (consume(QLatin1Char(')')))
                    break;
                if (!consume(QLatin1Char(',')) && !consume(QLatin1Char(';')))
                    fail(QStringLiteral("Ожидалась запятая или закрывающая скобка."));
            }
        }

        const QString function = name.toLower();
        if (function == QStringLiteral("sin") || function == QStringLiteral("cos") ||
            function == QStringLiteral("sqrt")) {
            if (arguments.size() != 1)
                fail(QStringLiteral("Функция %1 принимает один аргумент.").arg(name));
            if (function == QStringLiteral("sin"))
                return checked(std::sin(arguments[0]));
            if (function == QStringLiteral("cos"))
                return checked(std::cos(arguments[0]));
            if (arguments[0] < 0.0)
                fail(QStringLiteral("Нельзя извлечь квадратный корень из отрицательного числа."));
            return checked(std::sqrt(arguments[0]));
        }
        if (function == QStringLiteral("min") || function == QStringLiteral("max")) {
            if (arguments.size() < 2)
                fail(QStringLiteral("Функция %1 требует не менее двух аргументов.").arg(name));
            double result = arguments.front();
            for (std::size_t i = 1; i < arguments.size(); ++i)
                result = function == QStringLiteral("min")
                    ? std::min(result, arguments[i])
                    : std::max(result, arguments[i]);
            return checked(result);
        }
        fail(QStringLiteral("Неизвестная функция «%1».").arg(name));
    }
};

// Parse with a resolver which returns numeric values for known variables.
double evaluateWithResolver(const QString& expression, const Parser::Resolver& resolver)
{
    Parser parser(expression, resolver);
    return parser.parse();
}

} // namespace

ExpressionError::ExpressionError(const QString& message)
    : std::runtime_error(message.toUtf8().constData())
{
}

ExpressionParser::Values ExpressionParser::evaluateVariables(const Definitions& definitions)
{
    if (definitions.size() > 512)
        throw ExpressionError(QStringLiteral("В проекте слишком много переменных (максимум 512)."));
    Values values;
    QMap<QString, int> states; // 0/unseen, 1/visiting, 2/resolved
    std::function<double(const QString&)> resolve;
    resolve = [&](const QString& name) -> double {
        const auto definition = definitions.constFind(name);
        if (definition == definitions.cend())
            throw ExpressionError(QStringLiteral("Неизвестная переменная «%1».").arg(name));
        const int state = states.value(name, 0);
        if (state == 1)
            throw ExpressionError(QStringLiteral("Обнаружен цикл в определении переменной «%1».").arg(name));
        if (state == 2)
            return values.value(name);

        states.insert(name, 1);
        const double value = evaluateWithResolver(definition.value(), resolve);
        states.insert(name, 2);
        values.insert(name, value);
        return value;
    };

    for (auto it = definitions.cbegin(); it != definitions.cend(); ++it)
        resolve(it.key());
    return values;
}

double ExpressionParser::evaluate(const QString& expression, const Definitions& definitions)
{
    const Values values = evaluateVariables(definitions);
    return evaluate(expression, values);
}

double ExpressionParser::evaluate(const QString& expression, const Values& values)
{
    return evaluateWithResolver(expression, [&values](const QString& name) -> double {
        const auto it = values.constFind(name);
        if (it == values.cend())
            throw ExpressionError(QStringLiteral("Неизвестная переменная «%1».").arg(name));
        return it.value();
    });
}
