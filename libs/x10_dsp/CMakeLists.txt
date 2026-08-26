# x10_dsp — header-only DSP core shared by the rompler and the saturation plugin.
# Hard constraint (plan D1): no JUCE, no third-party headers. Standard library only.

add_library(x10_dsp INTERFACE)
add_library(x10::dsp ALIAS x10_dsp)

target_include_directories(x10_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(x10_dsp INTERFACE cxx_std_20)

# Gate 1: warnings are errors. Applied only to targets we own, never to JUCE.
add_library(x10_strict INTERFACE)
add_library(x10::strict ALIAS x10_strict)

target_compile_options(x10_strict INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wconversion
    -Wdouble-promotion      # catches accidental float -> double in the audio path
    -Wfloat-equal
)

if(X10_SANITIZE)
    target_compile_options(x10_strict INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined)
    target_link_options(x10_strict INTERFACE -fsanitize=address,undefined)
endif()

# Header list is declarative only (IDE grouping); the target stays INTERFACE.
set(X10_DSP_HEADERS
    include/x10/dsp/Config.h
    include/x10/dsp/Concepts.h
    include/x10/dsp/math/FastMath.h
    include/x10/dsp/nonlinear/Adaa1.h
    include/x10/dsp/nonlinear/Curves.h
    include/x10/dsp/nonlinear/Naive.h
    include/x10/dsp/filter/DCBlocker.h
    include/x10/dsp/filter/TptSvf.h
    include/x10/dsp/envelope/Adsr.h
)
target_sources(x10_dsp INTERFACE ${X10_DSP_HEADERS})
