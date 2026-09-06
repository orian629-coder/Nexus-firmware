# Shared warning profile applied to every Nexus module target.
# Kept strict but not -Werror by default (opt-in via NEXUS_WERROR) so third-party
# headers don't break the build on host toolchains.

function(nexus_apply_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
  )
  if(NEXUS_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
