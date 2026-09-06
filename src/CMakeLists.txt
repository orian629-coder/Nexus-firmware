# Phase-1 (fully implemented) modules first, then stub modules, then the executable.
# Order matters only for readability; CMake resolves inter-target deps regardless.

add_subdirectory(core)
add_subdirectory(logging)
add_subdirectory(config)
add_subdirectory(storage)
add_subdirectory(identity)
add_subdirectory(system)

# Stub modules (compiling no-ops implementing IService; filled in later phases).
add_subdirectory(network)
add_subdirectory(discovery)
add_subdirectory(pairing)
add_subdirectory(bluetooth)
add_subdirectory(control)
add_subdirectory(audio)
add_subdirectory(dsp)
add_subdirectory(amplifier)
add_subdirectory(microphone)
add_subdirectory(calibration)
add_subdirectory(measure)
add_subdirectory(status)
add_subdirectory(diagnostics)
add_subdirectory(updater)
add_subdirectory(web)

add_subdirectory(main)
