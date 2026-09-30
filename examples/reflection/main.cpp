#include <iostream>
#include <memory>
#include <string>

#include <livec/reflection.hpp>

struct Player {
    int health;
    float speed;

    void damage(int amount) {
        health -= amount;
    }
};

struct Number {
    virtual ~Number() = default;
    virtual int value() const = 0;
};

struct Seven final : Number {
    int value() const override { return 7; }
};

int score = 42;
std::string greeting = "hello from a C++ global";

int add(int left, int right) {
    return left + right;
}

double add(double left, double right) {
    return left + right;
}

int caughtValue() {
    try {
        throw 42;
    } catch (int value) {
        return value;
    }
}

int main() {
    auto addFunction = livec::function<int(int, int)>("add");
    std::cout << "function: " << addFunction.name << " " << addFunction.signature
              << " -> " << addFunction(20, 22) << '\n';
    auto addDouble = livec::function<double(double, double)>("add");
    std::cout << "overload: " << addDouble.signature << " -> " << addDouble(1.5, 2.5) << '\n';

    Player player{100, 3.5f};
    player.damage(10);
    auto playerType = livec::type("Player");
    std::cout << "type: " << playerType.kind << " " << playerType.name
              << " size=" << playerType.size << " align=" << playerType.alignment << '\n';
    for (const auto &field : playerType.fields) {
        std::cout << "  field: " << field.name << " " << field.type
                  << " offset=" << field.offset_bits << " bits\n";
    }
    for (const auto &method : playerType.methods) {
        std::cout << "  method: " << method.name << " " << method.signature << '\n';
    }

    std::unique_ptr<Number> polymorphic = std::make_unique<Seven>();
    std::cout << "virtual call: " << polymorphic->value() << '\n';
    std::cout << "dynamic cast: " << (dynamic_cast<Seven *>(polymorphic.get()) != nullptr) << '\n';
    std::cout << "caught exception: " << caughtValue() << '\n';

    auto scoreVariable = livec::variable("score");
    std::cout << "variable: " << scoreVariable.name << " " << scoreVariable.type
              << " value=" << scoreVariable.as<int>() << '\n';
    std::cout << "global object: " << greeting << '\n';
    for (const auto &variable : livec::variables())
        std::cout << "  global: " << variable.name << " " << variable.type << '\n';
    std::cout << "registry: " << livec::functions().size() << " functions, "
              << livec::variables().size() << " variables, "
              << livec::types().size() << " types\n";
    return 0;
}
