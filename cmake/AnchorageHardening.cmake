# Hardened build flags (see CLAUDE.md "Security rules", rule 10).
# Applied to every target through the `anchorage_hardening` interface library.

include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(anchorage_hardening INTERFACE)

target_compile_options(anchorage_hardening INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-qual -Wnull-dereference
    -fstack-protector-strong
    -fstack-clash-protection
    $<$<BOOL:${ANCHORAGE_WERROR}>:-Werror>
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(anchorage_hardening INTERFACE -fcf-protection=full)
endif()

target_link_options(anchorage_hardening INTERFACE
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
)

if(ANCHORAGE_SANITIZE)
    # FORTIFY_SOURCE and ASan don't mix well; sanitizer builds rely on ASan instead.
    target_compile_options(anchorage_hardening INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(anchorage_hardening INTERFACE -fsanitize=address,undefined)
else()
    # Ubuntu's compiler already defines _FORTIFY_SOURCE; redefine cleanly to level 3.
    # FORTIFY only has an effect when optimising.
    target_compile_options(anchorage_hardening INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>)
endif()
