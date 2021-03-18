#include "Halide.h"

using namespace Halide;
using namespace Halide::Internal;

Func g2_test(Func input, Expr offset, int scaling) {
    Var x, y;

    Func output;
    output(x, y) = input(x, y) * scaling + offset;
    output.compute_root();

    return output;
}

int main(int argc, char **argv) {
    using Constant = ArgInfoDetector::Constant;
    using Input = ArgInfoDetector::Input;
    using Output = ArgInfoDetector::Output;

    ArgInfoDetector g2_tester{
        g2_test,
        {Input("input", Int(32), 2), Input("offset", Int(32)), Constant("scaling", "2")},
        Output("output", Int(32), 2),
    };
    g2_tester.inspect();

    int lambda_scaling = 22;
    ArgInfoDetector lambda_tester{
        [lambda_scaling](Func input, Expr offset) -> Func {
            return g2_test(input, offset, lambda_scaling);
        },
        {Input("input", Int(32), 2), Input("offset", Int(32))},
        Output("output", Int(32), 2),
    };
    lambda_tester.inspect();

    return 0;
}
