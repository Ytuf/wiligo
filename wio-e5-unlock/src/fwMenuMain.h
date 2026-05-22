// Stub. rpUART.cpp #includes "../fwMenuMain.h" unconditionally AND declares
// `extern fwMenuMain obMenu;` at file scope, then only USES obMenu inside
// `#ifndef WILI_TWO_DISPLAY`. We define WILI_TWO_DISPLAY in CMake so the
// usage block compiles out, but the extern needs `fwMenuMain` to be a type.
#pragma once
class fwMenuMain {};
