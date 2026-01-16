# GBADoom Engine Component
file(GLOB_RECURSE GBADOOM_SOURCES "source/*.c")

idf_component_register(
    SRCS "${GBADOOM_SOURCES}"
    INCLUDE_DIRS "include"
    REQUIRES driver
)

# Compiler options for prboom - suppress warnings from legacy code
# This is old C code that needs many warnings disabled to compile cleanly
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Os
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-but-set-parameter
    -Wno-unused-value
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-char-subscripts
    -Wno-unused-const-variable
    -Wno-missing-field-initializers
    -Wno-stringop-truncation
    -Wno-format-truncation
    -Wno-format-overflow
    -Wno-misleading-indentation
    -Wno-pointer-sign
    -Wno-type-limits
    -Wno-int-to-pointer-cast
    -Wno-parentheses
    -Wno-address
    -Wno-duplicate-decl-specifier
    -Wno-nonnull
    -Wno-sizeof-pointer-div
    -Wno-implicit-function-declaration
    -Wno-discarded-qualifiers
    -Wno-switch
    -Wno-uninitialized
    -Wno-attributes
    -Wno-overflow
    -Wno-deprecated-declarations
    -Wno-incompatible-pointer-types
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-write-strings
    -ffunction-sections
    -fdata-sections
    -Wno-dangling-pointer
    -Wno-extern-initializer
    -Wno-compare-distinct-pointer-types
)
