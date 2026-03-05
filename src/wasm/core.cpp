#include <emscripten/bind.h>
#include <iostream>

using namespace emscripten;

int add(int a, int b) {
    std::cout << "C++ 收到計算請求: " << a << " + " << b << std::endl;
    return a + b;
}

EMSCRIPTEN_BINDINGS(my_module) {
    function("add", &add);
}