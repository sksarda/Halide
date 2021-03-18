#include "Halide.h"

#include "g2_generator.h"

namespace Halide {
namespace Testing {

Func g2_test(Func input, Expr offset, int scaling) {
    Var x, y;

    Func output;
    output(x, y) = input(x, y) * scaling + offset;
    output.compute_root();

    return output;
}

}  // namespace Testing
}  // namespace Halide

using namespace Halide;
using namespace Halide::Internal;

const char *G2_NAME = "g2";

RegisterGenerator register_something(
    G2_NAME,
    [](const GeneratorContext &context) -> std::unique_ptr<AbstractGenerator> {
        ArgInfoDetector d{
            Halide::Testing::g2_test,
            {ArgInfoDetector::Input("input", Int(32), 2), ArgInfoDetector::Input("offset", Int(32)), ArgInfoDetector::Constant("scaling", "2")},
            ArgInfoDetector::Output("output", Int(32), 2),
        };
        return G2GeneratorFactory(G2_NAME, std::move(d))(context);
    });
