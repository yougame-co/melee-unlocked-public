Nayuki's QR Code generator library, C++ version, unchanged from tag v1.8.0 of
https://github.com/nayuki/QR-Code-generator (cpp/qrcodegen.cpp, cpp/qrcodegen.hpp). MIT, see LICENSE.

Built into every runtime through port/runtime/gx/qr_code.cpp, which includes qrcodegen.cpp, so the
runtime source globs (Windows and Linux CMake, port/mac/build_full.py) pick it up with no list of their own.
Used by the in-game "Sign in by code" panel (port/runtime/gx/signin_overlay.cpp).
