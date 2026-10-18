# Box64's x86-64 CPU core (interpreter + ARM64 dynarec) as a static library, without its ELF
# loader, wrapped libraries or signal/thread layers: the port drives it (bb_box64.c).
cmake_minimum_required(VERSION 3.13)
project(box64core C ASM)
set(BOX64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../box64" CACHE PATH "Box64 source tree")
set(B ${BOX64_ROOT})
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
# Box64's generated headers (its own build makes them with git and a Python script), written
# into the build directory, which is on the include path.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/BOX64_COMMIT B64_COMMIT_LINE)
string(REGEX MATCH "^[0-9a-f]+" B64_COMMIT "${B64_COMMIT_LINE}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/git_head.h.tmp "#define GITREV \"${B64_COMMIT}\"\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/git_head.h.tmp ${CMAKE_CURRENT_BINARY_DIR}/git_head.h COPYONLY)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${B}/tools/gen_dynacache_hashes.py --root ${B}
                        --output ${CMAKE_CURRENT_BINARY_DIR}/dynacache_hashes.h
                RESULT_VARIABLE B64_HASHES_RESULT)
if(NOT B64_HASHES_RESULT EQUAL 0)
    message(FATAL_ERROR "box64core: gen_dynacache_hashes.py failed")
endif()
foreach(STEP_VALUE RANGE 3)
    add_library(b64_pass${STEP_VALUE} OBJECT ${DYNAREC_PASS})
    target_compile_definitions(b64_pass${STEP_VALUE} PRIVATE STEP=${STEP_VALUE})
endforeach()
add_library(b64_test_interpreter OBJECT ${INTERPRETER})
target_compile_definitions(b64_test_interpreter PRIVATE TEST_INTERPRETER)
set_source_files_properties(${DYNAREC_ASM} PROPERTIES COMPILE_OPTIONS "-mcpu=cortex-a76")
add_library(box64core STATIC ${CORE_SRC} ${INTERPRETER} ${DYNAREC_ASM} ${CMAKE_CURRENT_SOURCE_DIR}/bb_box64.c
    $<TARGET_OBJECTS:b64_pass0> $<TARGET_OBJECTS:b64_pass1> $<TARGET_OBJECTS:b64_pass2> $<TARGET_OBJECTS:b64_pass3>
    $<TARGET_OBJECTS:b64_test_interpreter>)
set(B64_DEFS DYNAREC ARM64 BOX64CORE_EMBEDDED _GNU_SOURCE NOALIGN)
if(ANDROID)
    list(APPEND B64_DEFS ANDROID)
endif()
set(B64_INC ${B}/src/include ${B}/src ${B}/src/dynarec/arm64 ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
foreach(t box64core b64_pass0 b64_pass1 b64_pass2 b64_pass3 b64_test_interpreter)
    target_compile_definitions(${t} PRIVATE ${B64_DEFS})
    target_include_directories(${t} PRIVATE ${B64_INC})
    target_compile_options(${t} PRIVATE -fno-strict-aliasing -w -O2 -fPIC)
endforeach()
target_include_directories(box64core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
